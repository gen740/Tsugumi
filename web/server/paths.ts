// Where the web UI host keeps its data, shared by the server and the Claude
// Code plugin's MCP server (which finds a running server there).
import os from 'node:os'
import path from 'node:path'

export function webDataDir(): string {
  return process.env.TSUGUMI_WEB_DATA ??
    (process.platform === 'darwin'
      ? path.join(os.homedir(), 'Library', 'Application Support', 'Tsugumi', 'web')
      : path.join(process.env.XDG_DATA_HOME ?? path.join(os.homedir(), '.local', 'share'), 'tsugumi-web'))
}

/** What a running server writes for local clients of this user (mode 0600). */
export interface ServerInfo {
  port: number
  key: string
  pid: number
  /** The cookie the key goes in. */
  cookie: string
}
