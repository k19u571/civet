export class ResourcePath {
  private localPath: string;
  private remotePath: string | null;

  constructor(local: string, remote: string | null = null) {
    this.localPath = local;
    this.remotePath = remote;
  }

  local(): string {
    return this.localPath;
  }

  remote(): string | null {
    return this.remotePath;
  }
}