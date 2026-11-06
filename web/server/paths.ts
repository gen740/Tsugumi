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

/** What a running server writes for local clients of this user (mode 0600):
 *  the Claude Code plugin's MCP server. Its key is not the page's: it reaches
 *  only the routes Claude Code works through (AGENT_ROUTES in main.ts), never
 *  the answers to approvals, the event stream, terminals or files. */
export interface ServerInfo {
  port: number
  pid: number
  /** Sent in the X-Tsugumi-Agent header. */
  agentKey: string
}

/** The header that carries ServerInfo.agentKey. */
export const AGENT_KEY_HEADER = 'x-tsugumi-agent'

/** The routes the agent key reaches: what the plugin's MCP server calls. */
export const AGENT_ROUTES: ReadonlySet<string> = new Set([
  'POST /api/project/open',
  'POST /api/project/create',
  'POST /api/agent/tools',
  'POST /api/agent/open',
  'POST /api/agent/call',
])
