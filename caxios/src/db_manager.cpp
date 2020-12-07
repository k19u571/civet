#include "db_manager.h"
#include <iostream>
#include "log.h"
#include "util/util.h"
#include "table/TableMeta.h"
#include <set>
#include <utility>
#include "util/pinyin.h"
#include "QueryAction.h"

#define READ_BEGIN(dbname) \
  if (m_mDBs[dbname] == -1) {\
    m_mDBs[dbname] = m_pDatabase->OpenDatabase(dbname);\
    T_LOG("manager", "OpenDatabase: %s, DBI: %d", dbname, m_mDBs[dbname]);\
    if (m_mDBs[dbname] == -1) return false;\
  }

#define WRITE_BEGIN()  m_pDatabase->Begin()
#define WRITE_END()    m_pDatabase->Commit()

#define BIT_INIT_OFFSET  0
#define BIT_TAG_OFFSET   1
#define BIT_CLASS_OFFSET 2
#define BIT_TAG     (1<<BIT_TAG_OFFSET)
#define BIT_CLASS   (1<<BIT_CLASS_OFFSET)
//#define BIT_ANNO  (1<<3)

namespace caxios {
  const char* g_tables[] = {
    TABLE_SCHEMA,
    TABLE_FILESNAP,
    TABLE_FILE_META,
    TABLE_KEYWORD_INDX,
    TABLE_INDX_KEYWORD,
    TABLE_KEYWORD2FILE,
    TABLE_FILE2KEYWORD,
    TABLE_TAG,
    TABLE_TAG2FILE,
    TABLE_TAG_INDX,
    TABLE_HASH2CLASS,
    TABLE_CLASS2HASH,
    TABLE_FILE2CLASS,
    TABLE_CLASS2FILE,
    TABLE_COUNT,
    TABLE_ANNOTATION,
    TABLE_MATCH_META,
    TABLE_MATCH
  };

  DBManager::DBManager(const std::string& dbdir, int flag, const std::string& meta/* = ""*/)
  {
    _flag = (flag == 0 ? ReadWrite : ReadOnly);
    if (m_pDatabase == nullptr) {
      m_pDatabase = new CDatabase(dbdir, _flag);
    }
    if (m_pDatabase == nullptr) {
      std::cerr << "new CDatabase fail.\n";
      return;
    }
    // open all database
    int cnt = sizeof(g_tables) / sizeof(char*);
    for (int idx = 0; idx < cnt; ++idx) {
      MDB_dbi dbi = m_pDatabase->OpenDatabase(g_tables[idx]);
      if (dbi >= 0) {
        m_mDBs[g_tables[idx]] = dbi;
      }
    }
    if (!meta.empty() && meta.size()>4 && meta != "[]") {
      ParseMeta(meta);
    }
    // 检查数据库版本是否匹配, 如果不匹配, 则开启线程，将当前数据库copy一份，进行升级, 并同步后续的所有写操作

    // 如果期间程序退出中断, 记录中断点, 下次启动时继续 
  }

  DBManager::~DBManager()
  {
    if (m_pDatabase != nullptr) {
      for (auto item : m_mDBs) {
        m_pDatabase->CloseDatabase(item.second);
      }
      m_mDBs.clear();
      for (auto item : m_mTables) {
        delete item.second;
      }
      m_mTables.clear();
      delete m_pDatabase;
      m_pDatabase = nullptr;
    }
  }

  std::vector<caxios::FileID> DBManager::GenerateNextFilesID(int cnt /*= 1*/)
  {
    std::vector<FileID> filesID;
    if (_flag == ReadOnly) return std::move(filesID);
    void* pData = nullptr;
    FileID lastID = 0;
    uint32_t len = 0;
    if (m_pDatabase->Get(m_mDBs[TABLE_FILESNAP], 0, pData, len)) {
      if (pData != nullptr) {
        lastID = *(FileID*)pData;
        T_LOG("generate", "val: %u, L: %d, %d", lastID, len, sizeof(uint32_t));
      }
    }
    for (int idx = 0; idx < cnt; ++idx) {
      lastID += 1;
      filesID.emplace_back(lastID);
    }
    WRITE_BEGIN();
    if (!m_pDatabase->Put(m_mDBs[TABLE_FILESNAP], 0, &lastID, sizeof(FileID))) {
    }
    WRITE_END();
    return std::move(filesID);
  }

  bool DBManager::AddFiles(const std::vector <std::tuple< FileID, MetaItems, Keywords >>& files)
  {
    if (_flag == ReadOnly) return false;
    WRITE_BEGIN();
    for (auto item : files) {
      if (!AddFile(std::get<0>(item), std::get<1>(item), std::get<2>(item))) {
        return false;
      }
    }
    WRITE_END();
    return true;
  }

  bool DBManager::AddClasses(const std::vector<std::string>& classes, const std::vector<FileID>& filesID)
  {
    if (_flag == ReadOnly || classes.size() == 0) return false;
    WRITE_BEGIN();
    std::vector<FileID> existID = mapExistFiles(filesID);
    auto mIndexes = GetWordsIndex(classes);
    T_LOG("class", "mIndexes size: %d, classes: %s", mIndexes.size(), format_vector(classes).c_str());
    std::vector<std::vector<WordIndex>> vIndexes;
    for (auto& clazz : classes) {
      std::vector<std::string> vTokens = split(clazz, '/');
      std::vector<WordIndex> vClassPath;
      T_LOG("class", "class path %d", vClassPath.size());
      std::for_each(vTokens.begin(), vTokens.end(), [this, &mIndexes, &vClassPath, &existID](const std::string& token) {
        T_LOG("class", "files count %d, class token: %s, %d", existID.size(), token.c_str(), mIndexes[token]);
        vClassPath.emplace_back(mIndexes[token]);
        for (FileID fileID : existID)
        {
          this->AddKeyword2File(mIndexes[token], fileID);
          this->AddFileID2Keyword(mIndexes[token], fileID);
        }
      });
      T_LOG("class", "class path %d", vClassPath.size());
      std::string classPath = serialize(vClassPath);
      this->AddFileID2Class(existID, classPath);
      vIndexes.emplace_back(vClassPath);
      
    }
    for (auto fileID : existID) {
      void* pData = nullptr;
      uint32_t len = 0;
      if (!m_pDatabase->Get(m_mDBs[TABLE_FILE2CLASS], fileID, pData, len)) {
        std::string sIndexes = serialize(vIndexes);
        m_pDatabase->Put(m_mDBs[TABLE_FILE2CLASS], fileID, (void*)sIndexes.data(), sIndexes.size());
      }
      else {
        std::string str((char*)pData, len);
        std::vector< std::vector<WordIndex> > vResult = deserialize<std::vector<std::vector<WordIndex>>>(str);
        vResult.insert(vResult.end(), vIndexes.begin(), vIndexes.end());
        std::string sResult = serialize(vResult);
        m_pDatabase->Put(m_mDBs[TABLE_FILE2CLASS], fileID, (void*)sResult.data(), sResult.size());
      }
      SetSnapStep(fileID, BIT_CLASS_OFFSET);
    }
    WRITE_END();
    return true;
  }

  bool DBManager::AddClasses(const std::vector<std::string>& classes)
  {
    if (_flag == ReadOnly || classes.size() == 0) return false;
    WRITE_BEGIN();
    auto mIndexes = GetWordsIndex(classes);
    void* pData = nullptr;
    uint32_t len = 0;
    for (auto& clazz : classes) {
      std::vector<std::string> vTokens = split(clazz, '/');
      std::vector<WordIndex> vClassPath;
      std::for_each(vTokens.begin(), vTokens.end(), [&mIndexes, &vClassPath](const std::string& token) {
        T_LOG("class", "class token: %s, %d", token.c_str(), mIndexes[token]);
        vClassPath.emplace_back(mIndexes[token]);
      });
      std::string key = serialize(vClassPath);
      T_LOG("class", "class key: %s", format_x16(key).c_str());
      m_pDatabase->Get(m_mDBs[TABLE_CLASS2FILE], key, pData, len);
      if (len == 0) {
        m_pDatabase->Put(m_mDBs[TABLE_CLASS2FILE], key, pData, len);
      }
    }
    WRITE_END();
    return true;
  }

  bool DBManager::RemoveFiles(const std::vector<FileID>& filesID)
  {
    WRITE_BEGIN();
    for (auto fileID: filesID)
    {
      RemoveFile(fileID);
    }
    WRITE_END();
    return true;
  }

  bool DBManager::RemoveTags(const std::vector<FileID>& filesID, const Tags& tags)
  {
    WRITE_BEGIN();
    for (auto fileID : filesID)
    {
    }
    WRITE_END();
    return true;
  }

  bool DBManager::SetTags(const std::vector<FileID>& filesID, const std::vector<std::string>& tags)
  {
    if (_flag == ReadOnly) return false;
    WRITE_BEGIN();
    std::vector<FileID> existID = mapExistFiles(filesID);
    auto mIndexes = GetWordsIndex(tags);
    std::for_each(mIndexes.begin(), mIndexes.end(), [this, &existID](std::pair<std::string, WordIndex> item) {
      AddTagPY(item.first, item.second);
      AddFileID2Tag(existID, item.second);
      for (FileID fileID : existID) {
        this->AddFileID2Keyword(fileID, item.second);
        this->AddKeyword2File(item.second, fileID);
      }
    });
    for (auto fileID : existID) {
      if (!IsFileExist(fileID)) continue;
      // 所有标签以数组形式保存, 第0位为数组长度
      void* pData = nullptr;
      uint32_t len = 0;
      std::string value;
      WordIndex* indexes = (WordIndex*)malloc(tags.size() * sizeof(WordIndex));
      // 不存在则添加
      WordIndex* ptr = indexes;
      std::for_each(mIndexes.begin(), mIndexes.end(), [&ptr](std::pair<std::string, WordIndex> item) {
        *ptr = item.second;
        ++ptr;
      });
      m_pDatabase->Put(m_mDBs[TABLE_TAG], fileID, indexes, tags.size() * sizeof(WordIndex));
      free(indexes);
      SetSnapStep(fileID, BIT_TAG_OFFSET);
    }
    WRITE_END();
    return true;
  }

  bool DBManager::GetFilesInfo(const std::vector<FileID>& filesID, std::vector< FileInfo>& filesInfo)
  {
    READ_BEGIN(TABLE_FILE_META);
    for (auto fileID: filesID)
    {
      void* pData = nullptr;
      uint32_t len = 0;
      if (!m_pDatabase->Get(m_mDBs[TABLE_FILE_META], fileID, pData, len)) {
        T_LOG("files", "Get TABLE_FILE_META Fail: %d", fileID);
        continue;
      }
      std::string sMeta((char*)pData, len);
      std::string sDebug((char*)pData, 500 <len? 500: len);
      T_LOG("files", "meta: %s", sDebug.c_str());
      using namespace nlohmann;
      json meta = json::parse(sMeta);
      MetaItems items;
      for (auto value: meta) {
        MetaItem item;
        for (auto it = value.begin(); it!=value.end();++it)
        {
          item[it.key()] = trunc(it.value().dump());
        }
        items.emplace_back(item);
      }
      // tag
      Tags tags;
      if (m_pDatabase->Get(m_mDBs[TABLE_TAG], fileID, pData, len)) {
        WordIndex* pWordIndex = (WordIndex*)pData;
        tags = GetWordByIndex(pWordIndex, len/sizeof(WordIndex));
        T_LOG("files", "file tags cnt: %d", tags.size());
        for (auto& t : tags)
        {
          T_LOG("files", "file %d tags: %s", fileID, t.c_str());
        }
      }
      // ann
      // clazz
      Classes classes;
      if (m_pDatabase->Get(m_mDBs[TABLE_FILE2CLASS], fileID, pData, len)) {
        WordIndex* pWordIndex = (WordIndex*)pData;
        classes = GetWordByIndex(pWordIndex, len / sizeof(WordIndex));
      }
      // keyword
      Keywords keywords;
      if (m_pDatabase->Get(m_mDBs[TABLE_FILE2KEYWORD], fileID, pData, len)) {
        WordIndex* pWordIndex = (WordIndex*)pData;
        keywords = GetWordByIndex(pWordIndex, len / sizeof(WordIndex));
        T_LOG("files", "keyword[%d]: %s", keywords.size(), format_vector(keywords).c_str());
      }
      FileInfo fileInfo{ fileID, items, tags, classes, {}, keywords };
      filesInfo.emplace_back(fileInfo);
    }
    return true;
  }

  bool DBManager::GetFilesSnap(std::vector< Snap >& snaps)
  {
    READ_BEGIN(TABLE_FILESNAP);
    m_pDatabase->Filter(m_mDBs[TABLE_FILESNAP], [&snaps](uint32_t k, void* pData, uint32_t len) -> bool {
      if (k == 0) return false;
      using namespace nlohmann;
      std::string js((char*)pData, len);
      json file=json::parse(js);
      T_LOG("snap", "GetFilesSnap: %s", js.c_str());
      try {
        std::string display = trunc(to_string(file["value"]));
        std::string val = trunc(file["step"].dump());
        T_LOG("snap", "File step: %s", val.c_str());
        int step = std::stoi(val);
        T_LOG("snap", "File step: %d, %s", step, val.c_str());
        Snap snap{ k, display, step };
        snaps.emplace_back(snap);
      }catch(json::exception& e){
        T_LOG("snap", "ERR: %s", e.what());
      }
      return false;
    });
    return true;
  }

  bool DBManager::GetUntagFiles(std::vector<FileID>& filesID)
  {
    std::vector<Snap> vSnaps;
    if (!GetFilesSnap(vSnaps)) return false;
    T_LOG("tag", "File Snaps: %d", vSnaps.size());
    for (auto& snap : vSnaps) {
      char bits = std::get<2>(snap);
      T_LOG("tag", "untag file %d: %d", std::get<0>(snap), bits);
      if (!(bits & BIT_TAG)) {
        filesID.emplace_back(std::get<0>(snap));
      }
    }
    return true;
  }

  bool DBManager::GetUnClassifyFiles(std::vector<FileID>& filesID)
  {
    std::vector<Snap> vSnaps;
    if (!GetFilesSnap(vSnaps)) return false;
    for (auto& snap : vSnaps) {
      char bits = std::get<2>(snap);
      T_LOG("class", "unclassify file %d: %d", std::get<0>(snap), bits);
      if (!(bits & BIT_CLASS)) {
        filesID.emplace_back(std::get<0>(snap));
      }
    }
    return true;
  }

  bool DBManager::GetTagsOfFiles(const std::vector<FileID>& filesID, std::vector<Tags>& vTags)
  {
    for (auto fileID : filesID) {
      Tags tags;
      GetFileTags(fileID, tags);
      vTags.emplace_back(tags);
    }
    return true;
  }

  bool DBManager::GetAllClasses(nlohmann::json& classes)
  {
    READ_BEGIN(TABLE_CLASS2FILE);
    std::set<WordIndex> sIndexes;
    std::map<std::string, std::vector<FileID>> vClassPath;
    m_pDatabase->Filter(m_mDBs[TABLE_CLASS2FILE], [&sIndexes, &vClassPath](const std::string& classPath, void* pData, uint32_t len)->bool {
      std::vector<WordIndex> vWordIndx = deserialize< std::vector<WordIndex> >(classPath);
      auto& path = vClassPath[classPath];
      if (len != 0) {
        FileID* filesID = (FileID*)pData;
        std::copy(filesID, filesID + len / sizeof(FileID), std::back_inserter(path));
      }
      sIndexes.insert(vWordIndx.begin(), vWordIndx.end());
      return false;
    });
    auto words = GetWordByIndex(sIndexes.begin(), sIndexes.end());
    for (auto& item : vClassPath) {
      std::vector<WordIndex> vWordIndx = deserialize< std::vector<WordIndex> >(item.first);
      nlohmann::json* child = &(classes[words[vWordIndx[0]]]);
      child->operator[]("type") = "clz";
      if (vWordIndx.size() > 1) {
        //nlohmann::json children;
        //children["name"] = words[vWordIndx[vWordIndx.size() - 1]];
        for (size_t idx = 1; idx < vWordIndx.size(); ++idx) {
          child = &(child->operator[](words[vWordIndx[idx]]));
          //T_LOG("class json key: %s", words[vWordIndx[idx]].c_str());
        }
        child->operator[]("type") = "clz";
      }
      for (FileID fid : item.second)
      {
        child->operator[]("children").push_back(fid);
      }
      //T_LOG("children class json: %s, files %d", classes.dump().c_str(), item.second.size());
      T_LOG("class", "class json(%s): %s", words[vWordIndx[0]].c_str(), child->dump().c_str());
    }
    return true;
  }

  bool DBManager::GetAllTags(TagTable& tags)
  {
    READ_BEGIN(TABLE_TAG_INDX);
    READ_BEGIN(TABLE_TAG2FILE);

    m_pDatabase->Filter(m_mDBs[TABLE_TAG_INDX], [this, &tags](const std::string& alphabet, void* pData, uint32_t len)->bool {
      typedef std::tuple<std::string, std::string, std::vector<FileID>> TagInfo;
      WordIndex* pIndx = (WordIndex*)pData;
      size_t cnt = len / sizeof(WordIndex);
      std::vector<std::string> words = this->GetWordByIndex(pIndx, cnt);
      std::vector<std::vector<FileID>> vFilesID = this->GetFilesIDByTagIndex(pIndx, cnt);
      std::vector<TagInfo> vTags;
      for (size_t idx = 0; idx < words.size(); ++idx) {
        TagInfo tagInfo = std::make_tuple(alphabet, words[idx], vFilesID[idx]);
        vTags.emplace_back(tagInfo);
      }
      tags[alphabet[0]] = vTags;
      return false;
    });
    return true;
  }

  bool DBManager::UpdateFilesClasses(const std::vector<FileID>& filesID, const std::vector<std::string>& classes)
  {
    return this->AddClasses(classes, filesID);
  }

  bool DBManager::UpdateClassName(const std::string& oldName, const std::string& newName)
  {
    WRITE_BEGIN();
    std::vector<std::string> names({ oldName, newName });
    auto mIndexes = GetWordsIndex(names);
    std::vector<std::string> vOldTokens = split(oldName, '/');
    std::vector<WordIndex> vOldPath;
    std::for_each(vOldTokens.begin(), vOldTokens.end(), [&mIndexes, &vOldPath](const std::string& token) {
      vOldPath.emplace_back(mIndexes[token]);
    });
    std::vector<std::string> vNewTokens = split(newName, '/');
    std::vector<WordIndex> vNewPath;
    std::for_each(vNewTokens.begin(), vNewTokens.end(), [&mIndexes, &vNewPath](const std::string& token) {
      vNewPath.emplace_back(mIndexes[token]);
    });
    // TODO: get old class's children classes
    // get old class map files
    auto vFilesID = GetFilesByClass(vOldPath);
    // TODO: update files class
    // TODO: update class and children
    WRITE_END();
    return true;
  }

  bool DBManager::Query(const std::string& query, std::vector< FileInfo>& filesInfo)
  {
    namespace pegtl = TAO_PEGTL_NAMESPACE;
    QueryAction::Init();
    QueryAction action(this);
    pegtl::memory_input input(query, "");
    T_LOG("query", "sql: %s", query.c_str());
    if (pegtl::parse< caxios::QueryGrammar, caxios::action>(input, action)) {
      std::vector<FileID> vFilesID = action.GetResult();
      return GetFilesInfo(vFilesID, filesInfo);
    }
    T_LOG("query", "Fail sql: %s", query.c_str());
    return false;
  }

  bool DBManager::QueryKeyword(const std::string& tableName, const std::string& value, std::vector<FileID>& outFilesID)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    T_LOG("query", "table: %s, DBI: %d, keyword: %s", tableName.c_str(), m_mDBs[tableName], value.c_str());
    WordIndex index = GetWordIndex(value);
    if (index == 0) return false;
    if (!m_pDatabase->Get(m_mDBs[tableName], index, pData, len)) return false;
    if (len) {
      outFilesID.assign((FileID*)pData, (FileID*)pData + len / sizeof(FileID));
    }
    return true;
  }

  void DBManager::ValidVersion()
  {
    void* pData = nullptr;
    uint32_t len = 0;
    if (m_pDatabase->Get(m_mDBs[TABLE_SCHEMA], SCHEMA_VERSION, pData, len)) {
    }
  }

  bool DBManager::AddFile(FileID fileid, const MetaItems& meta, const Keywords& keywords)
  {
    using namespace nlohmann;
    json dbMeta;
    dbMeta = meta;
    std::string value = to_string(dbMeta);
    if (!m_pDatabase->Put(m_mDBs[TABLE_FILE_META], fileid, (void*)(value.data()), value.size())) {
      T_LOG("file", "Put TABLE_FILE_META Fail %s", value.c_str());
      return false;
    }
    T_LOG("file", "Write ID: %d, Meta Info: %s", fileid, value.c_str());
    //snap
    json snaps;
    for (MetaItem m: meta)
    {
      if (m["name"] == "filename"){
        snaps["value"] = m["value"];
        break;
      }
    }
    snaps["step"] = (char)0x1;
    std::string sSnaps = to_string(snaps);
    if (!m_pDatabase->Put(m_mDBs[TABLE_FILESNAP], fileid, (void*)(sSnaps.data()), sSnaps.size())) {
      T_LOG("file", "Put TABLE_FILESNAP Fail %s", sSnaps.c_str());
      return false;
    }
    T_LOG("file", "Write Snap: %s", sSnaps.c_str());
    // meta
    for (MetaItem m : meta) {
      std::string& name = m["name"];
      if(m_mTables.find(name) == m_mTables.end()) continue;
      std::vector<FileID> vID;
      vID.emplace_back(fileid);
      m_mTables[name]->Add(m["value"], vID);
    }
    // counts
    SetSnapStep(fileid, BIT_INIT_OFFSET);
    // keyword
    return true;
  }

  bool DBManager::AddFileID2Tag(const std::vector<FileID>& vFilesID, WordIndex index)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    if (!m_pDatabase->Get(m_mDBs[TABLE_TAG2FILE], index, pData, len)) {
      m_pDatabase->Put(m_mDBs[TABLE_TAG2FILE], index, (void*)vFilesID.data(), vFilesID.size() * sizeof(FileID));
    }
    else {
      FileID* pID = (FileID*)pData;
      std::set<FileID> sFilesID(pID, pID + len / sizeof(FileID));
      sFilesID.insert(vFilesID.begin(), vFilesID.end());
      std::vector<FileID> vID(sFilesID.begin(), sFilesID.end());
      m_pDatabase->Put(m_mDBs[TABLE_TAG2FILE], index, (void*)vID.data(), vID.size() * sizeof(FileID));
    }
    return true;
  }

  bool DBManager::AddFileID2Keyword(FileID fileID, WordIndex keyword)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    m_pDatabase->Get(m_mDBs[TABLE_KEYWORD2FILE], keyword, pData, len);
    if (len > 0) {
      FileID* pIDs = (FileID*)pData;
      size_t cnt = len / sizeof(FileID);
      std::vector<FileID> vFilesID(pIDs, pIDs + cnt);
      auto ptr = std::lower_bound(vFilesID.begin(), vFilesID.end(), fileID);
      if (*ptr == fileID) return true;  // file id exist
      vFilesID.insert(ptr, fileID);
      if (!m_pDatabase->Put(m_mDBs[TABLE_KEYWORD2FILE], keyword, &(vFilesID[0]), sizeof(FileID) * vFilesID.size())) return false;
    }
    else {
      if (!m_pDatabase->Put(m_mDBs[TABLE_KEYWORD2FILE], keyword, &fileID, sizeof(FileID))) return false;
    }
    return true;
  }

  bool DBManager::AddKeyword2File(WordIndex keyword, FileID fileID)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    m_pDatabase->Get(m_mDBs[TABLE_FILE2KEYWORD], fileID, pData, len);
    T_LOG("keyword", "add keyword Index: %d, len: %d", keyword, len);
    if (len > 0) {
      WordIndex* pWords = (WordIndex*)pData;
      size_t cnt = len / sizeof(WordIndex);
      std::vector<WordIndex> vWordIndx(pWords, pWords + cnt);
      auto ptr = std::lower_bound(vWordIndx.begin(), vWordIndx.end(), fileID);
      if (*ptr == keyword) return true;  // file id exist
      vWordIndx.insert(ptr, keyword);
      T_LOG("keyword", "add keywords: %d, fileID: %d", keyword, fileID);
      if (!m_pDatabase->Put(m_mDBs[TABLE_FILE2KEYWORD], fileID, &(vWordIndx[0]), sizeof(WordIndex) * vWordIndx.size())) return false;
    }
    else {
      if (!m_pDatabase->Put(m_mDBs[TABLE_FILE2KEYWORD], fileID, &keyword, sizeof(WordIndex))) return false;
      T_LOG("keyword", "add new keywords: %d, fileID: %d", keyword, fileID);
    }
    return true;
  }

  bool DBManager::AddTagPY(const std::string& tag, WordIndex indx)
  {
    T_LOG("tag", "input py: %s", tag.c_str());
    std::wstring wTag = string2wstring(tag);
    T_LOG("tag", "input wpy: %s, %d", wTag.c_str(), wTag[0]);
    auto py = getLocaleAlphabet(wTag[0]);
    T_LOG("tag", "Pinyin: %s -> %s", wTag.c_str(), py[0].c_str());
    WRITE_BEGIN();
    std::set<WordIndex> sIndx;
    void* pData = nullptr;
    uint32_t len = 0;
    if (m_pDatabase->Get(m_mDBs[TABLE_TAG_INDX], py[0], pData, len)) {
      WordIndex* pWords = (WordIndex*)pData;
      size_t cnt = len / sizeof(WordIndex);
      sIndx = std::set<WordIndex>(pWords, pWords + cnt);
    }
    sIndx.insert(indx);
    std::vector< WordIndex> vIndx(sIndx.begin(), sIndx.end());
    m_pDatabase->Put(m_mDBs[TABLE_TAG_INDX], py[0], vIndx.data(), vIndx.size() * sizeof(WordIndex));
    WRITE_END();
    return true;
  }

  bool DBManager::AddFileID2Class(const std::vector<FileID>& vFilesID, const std::string& key)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    if (!m_pDatabase->Get(m_mDBs[TABLE_CLASS2FILE], key, pData, len)) {
      //T_LOG("Put new class: %d", index);
      m_pDatabase->Put(m_mDBs[TABLE_CLASS2FILE], key, (void*)vFilesID.data(), vFilesID.size() * sizeof(FileID));
    }
    else {
      FileID* pID = (FileID*)pData;
      std::set<FileID> sFilesID(pID, pID + len / sizeof(FileID));
      sFilesID.insert(vFilesID.begin(), vFilesID.end());
      std::vector<FileID> vID(sFilesID.begin(), sFilesID.end());
      //T_LOG("Put class: %d", index);
      m_pDatabase->Put(m_mDBs[TABLE_CLASS2FILE], key, (void*)vID.data(), vID.size() * sizeof(FileID));
    }
    return true;
  }

  bool DBManager::RemoveFile(FileID fileID)
  {
    using namespace nlohmann;
    //
    void* pData = nullptr;
    uint32_t len = 0;
    if (!m_pDatabase->Get(m_mDBs[TABLE_FILE_META], fileID, pData, len)) {
      T_LOG("file", "GET TABLE_FILE_META Fail %d", fileID);
      return false;
    }
    T_LOG("file", "meta len: %d", len);
    std::string info((char*)pData, len);
    json meta = json::parse(info);
    // meta
    for (MetaItem m : meta) {
      std::string& name = m["name"];
      if (m_mTables.find(name) == m_mTables.end()) continue;
      m_mTables[name]->Delete(m["value"], fileID);
    }
    // tag
    // snap
    m_pDatabase->Del(m_mDBs[TABLE_FILESNAP], fileID);
    m_pDatabase->Del(m_mDBs[TABLE_FILE_META], fileID);
    // TODO: 回收键值
    return true;
  }

  bool DBManager::RemoveTag(FileID fileID, const Tags& tags)
  {
    READ_BEGIN(TABLE_TAG);
    void* pData = nullptr;
    uint32_t len = 0;
    m_pDatabase->Get(m_mDBs[TABLE_TAG], fileID, pData, len);
    if (len == 0) return true;
    auto indexes = GetWordsIndex(tags);
    WordIndex* pWordIndx = (WordIndex*)pData;
    size_t cnt = len / sizeof(WordIndex);
    for (auto& item : indexes)
      for (size_t idx = 0; idx < cnt; ++idx) {
      {
        if (pWordIndx[idx] == item.second) {
          pWordIndx[idx] = 0;
          break;
        }
      }
    }
    m_pDatabase->Put(m_mDBs[TABLE_TAG], fileID, pData, len);
    return true;
  }

  bool DBManager::GetFileInfo(FileID fileID, MetaItems& meta, Keywords& keywords, Tags& tags, Annotations& anno)
  {
    // meta
    return true;
  }

  bool DBManager::GetFileTags(FileID fileID, Tags& tags)
  {
    READ_BEGIN(TABLE_TAG);
    void* pData = nullptr;
    uint32_t len = 0;
    if (!m_pDatabase->Get(m_mDBs[TABLE_TAG], fileID, pData, len)) return true;
    WordIndex* pWordIndx = (WordIndex*)pData;
    size_t cnt = len / sizeof(WordIndex);
    tags = GetWordByIndex(pWordIndx, cnt);
    return true;
  }

  std::vector<caxios::FileID> DBManager::GetFilesByClass(const std::vector<WordIndex>& clazz)
  {
    std::vector<caxios::FileID> vFilesID;
    std::string sPath = serialize(clazz);
    void* pData = nullptr;
    uint32_t len = 0;
    m_pDatabase->Get(m_mDBs[TABLE_CLASS2FILE], sPath, pData, len);
    if (len == 0) {
      T_LOG("file", "get files by class error: %s", format_vector(clazz).c_str());
    }
    else {
      vFilesID.assign((FileID*)pData, (FileID*)pData + len / sizeof(FileID));
    }
    return std::move(vFilesID);
  }

  bool DBManager::IsFileExist(FileID fileID)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    m_pDatabase->Get(m_mDBs[TABLE_FILESNAP], fileID, pData, len);
    if (len > 0) return true;
    return false;
  }

  bool DBManager::IsClassExist(const std::string& clazz)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    m_pDatabase->Get(m_mDBs[TABLE_CLASS2HASH], clazz, pData, len);
    if (len == 0) return false;
    return true;
  }

  uint32_t DBManager::GenerateClassHash(const std::vector<WordIndex>& clazz)
  {
    std::string s = serialize(clazz);
    if (IsClassExist(s)) return GetClassHash(s);
  }

  uint32_t DBManager::GetClassHash(const std::string& clazz)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    if (!m_pDatabase->Get(m_mDBs[TABLE_CLASS2HASH], clazz, pData, len)) return 0;
    return *(uint32_t*)pData;
  }

  std::vector<caxios::FileID> DBManager::mapExistFiles(const std::vector<FileID>& filesID)
  {
    std::vector<caxios::FileID> vFiles;
    for (auto fileID : filesID) {
      vFiles.emplace_back(fileID);
    }
    return std::move(vFiles);
  }

  void DBManager::ParseMeta(const std::string& meta)
  {
    nlohmann::json jsn = nlohmann::json::parse(meta);
    for (auto item : jsn)
    {
      if (item["db"] == true) {
        std::string name = trunc(item["name"].dump());
        m_mTables[name] = new TableMeta(m_pDatabase, name, trunc(item["type"].dump()));
        //T_LOG("schema: %s", item["name"].dump().c_str());
      }
    }
    T_LOG("construct", "schema: %s", meta.c_str());
  }

  void DBManager::UpdateCount1(CountType ct, int value)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    uint32_t cnt = 0;
    m_pDatabase->Get(m_mDBs[TABLE_COUNT], CT_UNCALSSIFY, pData, len);
    if (pData) cnt = *(uint32_t*)pData;
    cnt += value;
    m_pDatabase->Put(m_mDBs[TABLE_COUNT], CT_UNCALSSIFY, (void*)&cnt, sizeof(uint32_t));
  }

  void DBManager::SetSnapStep(FileID fileID, int offset)
  {
    nlohmann::json jSnap;
    int step = GetSnapStep(fileID, jSnap);
    if (step & (1 << offset)) return;
    step |= (1 << offset);
    jSnap["step"] = step;
    std::string snap = jSnap.dump();
    T_LOG("snap", "put snap value: file %d, %d, bit %d", fileID, step, offset);
    m_pDatabase->Put(m_mDBs[TABLE_FILESNAP], fileID, (void*)snap.data(), snap.size());
  }

  char DBManager::GetSnapStep(FileID fileID, nlohmann::json& jSnap)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    m_pDatabase->Get(m_mDBs[TABLE_FILESNAP], fileID, pData, len);
    if (len == 0) return 0;
    std::string snap((char*)pData, len);
    jSnap = nlohmann::json::parse(snap);
    int step = atoi(jSnap["step"].dump().c_str());
    return step;
  }

  std::map<std::string, caxios::WordIndex> DBManager::GetWordsIndex(const std::vector<std::string>& words)
  {
    std::map<std::string, caxios::WordIndex> mIndexes;
    WordIndex lastIndx = 0;
    void* pData = nullptr;
    uint32_t len = 0;
    if (m_pDatabase->Get(m_mDBs[TABLE_KEYWORD_INDX], 0, pData, len)) {
      if (pData != nullptr) {
        lastIndx = *(WordIndex*)pData;
      }
    }
    lastIndx += 1;
    bool bUpdate = false;
    for (auto& word : words) {
      std::vector<std::string> vTokens = split(word, '/');
      for (auto& token: vTokens)
      {
        // 取字索引
        if (!m_pDatabase->Get(m_mDBs[TABLE_KEYWORD_INDX], token, pData, len)) {
          // 如果tag字符串不存在，添加
          T_LOG("dict", "Add new word: %s", token.c_str());
          m_pDatabase->Put(m_mDBs[TABLE_KEYWORD_INDX], token, &lastIndx, sizeof(WordIndex));
          m_pDatabase->Put(m_mDBs[TABLE_INDX_KEYWORD], lastIndx, (void*)token.data(), token.size());
          mIndexes[token] = lastIndx;
          lastIndx += 1;
          bUpdate = true;
          continue;
        }
        mIndexes[token] = *(WordIndex*)pData;
        T_LOG("dict", "Get word: %s, %d", token.c_str(), mIndexes[token]);
      }
    }
    if (bUpdate) {
      m_pDatabase->Put(m_mDBs[TABLE_KEYWORD_INDX], 0, &lastIndx, sizeof(WordIndex));
    }
    //m_pDatabase->Filter(m_mDBs[TABLE_KEYWORD_INDX], [](const std::string& k, void* pData, uint32_t len)->bool {
    //  T_LOG("keyword indx: %s", k.c_str());
    //  return false;
    //});
    return std::move(mIndexes);
  }

  WordIndex DBManager::GetWordIndex(const std::string& word)
  {
    void* pData = nullptr;
    uint32_t len = 0;
    if (!m_pDatabase->Get(m_mDBs[TABLE_KEYWORD_INDX], word, pData, len)) return 0;
    return *(WordIndex*)pData;
  }

  std::vector<std::string> DBManager::GetWordByIndex(const WordIndex* const wordsIndx, size_t cnt)
  {
    std::vector<std::string> vWords(cnt);
    for (size_t idx = 0; idx< cnt;++idx)
    {
      WordIndex index = wordsIndx[idx];
      if (index == 0) {
        T_LOG("dict", "word index: 0");
        continue;
      }
      void* pData = nullptr;
      uint32_t len = 0;
      if(!m_pDatabase->Get(m_mDBs[TABLE_INDX_KEYWORD], index, pData, len)) continue;
      std::string word((char*)pData, len);
      vWords[idx] = word;
    }
    return std::move(vWords);
  }

  std::vector<std::vector<FileID>> DBManager::GetFilesIDByTagIndex(const WordIndex* const wordsIndx, size_t cnt)
  {
    std::vector<std::vector<FileID>> vFilesID;
    for (size_t idx = 0; idx < cnt; ++idx) {
      WordIndex wordIdx = wordsIndx[idx];
      void* pData = nullptr;
      uint32_t len = 0;
      if(!m_pDatabase->Get(m_mDBs[TABLE_TAG2FILE], wordIdx, pData, len)) continue;
      FileID* pFileID = (FileID*)pData;
      std::vector<FileID> filesID(pFileID, pFileID + len / sizeof(FileID));
      vFilesID.emplace_back(filesID);
    }
    return std::move(vFilesID);
  }

}