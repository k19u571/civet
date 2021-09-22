import { window, OverviewItemLoadEvent, IResource, utility } from 'civet'

let gridview = window.createOverview('gridview', 'waterfall layout');

const fs = require('fs')
let frame = ''
fs.readFile(utility.extensionPath + '/grid_view/view.html', 'utf-8', (err, data)=> {
  if (err) {
    console.error(err)
    return;
  }
  frame = data.toString();
})

function a() {
  console.info('aaaaaaa')
}

gridview.onResourcesLoading((e: OverviewItemLoadEvent) => {
  console.info('grid view onResourcesLoading', e.resources.length)
  if (e.classes && e.classes.length) {
    frame = frame.replace('{{classes}}', JSON.stringify(e.classes))
  } else {
    frame = frame.replace('{{classes}}', '')
  }
  if (e.resources && e.resources.length) {
    frame = frame.replace('{{resources}}', JSON.stringify(e.resources))
  } else {
    frame = frame.replace('{{resources}}', '')
  }
  console.info('GRID:', a.toString())
  gridview.html = frame
}, gridview);