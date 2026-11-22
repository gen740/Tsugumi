// The page's local routes: what the Node Web host answered over HTTP, now
// assembled in the page from Host and Agent RPC. hostd owns all state; the
// route names stay so the screens keep one call shape (api() in ../api.ts).
import { chatRoutes, chatStatus, hostRequest } from './chat'
import { CoreHost, CoreHostError } from './coreHost'
import { hostEnvironments } from './environment'
import { hostProjectErrorText } from './hostProjects'
import { language } from '../i18n'

type Handler = (b: any, url: URL, signal?: AbortSignal) => unknown | Promise<unknown>

export class RouteError extends Error {
  constructor(readonly status: number, message: string, readonly code?: string) { super(message) }
}

interface Home {
  home: string; documents: string; repo?: string; hostFolders: boolean; vm: boolean; nix: boolean
  tsugumid?: { path: string; ok: boolean }; worker?: { path: string; ok: boolean }
}

type Listener = (event: unknown) => void

/** The page's routes and the listeners chat.ts broadcasts to. */
export function pageRoutes(broadcast: Listener) {
  const envs = hostEnvironments((method, fields) => core.user.call(method, fields), () => [location.origin])
  const core = new CoreHost(envs)
  let routes: Promise<Record<string, Handler>> | undefined
  const home = () => core.user.call<Home>('HostHomeRequest')
  const build = async () => {
    const local: Record<string, Handler> = {
      'GET /api/runtime': () => hostRequest('HostRuntimeRequest'),
      'POST /api/runtime/cache-usage': async () => (await home()).vm ? hostRequest('HostRuntimeRequest', { action: 'Command', command: 'cache-usage' }) : {},
      'POST /api/runtime/verify-store': () => hostRequest('HostRuntimeRequest', { action: 'VerifyStore' }),
      'POST /api/runtime/reset': () => hostRequest('HostRuntimeRequest', { action: 'Reset' }),
      'GET /api/status': async () => {
        const now = await home()
        const ok = { path: '', ok: true }
        return {
          setup: { tsugumid: now.tsugumid ?? ok, worker: now.worker ?? ok, nix: { ok: now.nix }, repo: now.repo ?? '' },
          demos: await hostRequest('HostDemoListRequest', { language: language() }),
          recents: await hostRequest('HostRecentListRequest'), home: now.home, launchProject: null,
          ...await hostRequest('HostProjectDefaultsRequest'),
          hostFolders: now.hostFolders, ...chatStatus(), documents: now.documents,
        }
      },
      'POST /api/project/open': async (b) => {
        const recent = await hostRequest('HostRecentRememberRequest', { path: b.path })
        return { path: recent.path, key: recent.path, name: recent.name }
      },
      'POST /api/project/create': (b) => hostRequest('HostProjectCreateRequest', { parent: b.parent, name: b.name, flow: b.flow }),
      'POST /api/project/duplicate': (b) => hostRequest('HostProjectDuplicateRequest', { path: b.path, parent: b.parent, name: b.name }),
      'POST /api/recents/forget': (b) => hostRequest('HostRecentForgetRequest', { paths: b.paths }),
      'POST /api/reveal': async (b) => { await hostRequest('HostRevealRequest', { path: String(b.path) }); return { ok: true } },
      'POST /api/demo/copy': async (b) => hostRequest('HostDemoCopyRequest', { id: b.id, parent: (await home()).documents, language: language() }),
      'GET /api/fs': (_b, url) => hostRequest('HostFolderListRequest', { path: url.searchParams.get('path') || undefined }),
      'POST /api/fs/mkdir': (b) => hostRequest('HostFolderCreateRequest', { parent: String(b.parent), name: String(b.name) }),
    }
    const chat = chatRoutes({
      envs, projectDir: String, broadcast,
      httpError: (status, message) => new RouteError(status, message), core,
    })
    return { ...local, ...chat }
  }
  return {
    core,
    async call(method: string, path: string, body: unknown, signal?: AbortSignal): Promise<unknown> {
      const table = await (routes ??= build().catch((error) => { routes = undefined; throw error }))
      const url = new URL(path, location.href)
      const handler = table[`${method} ${url.pathname}`]
      if (!handler) throw new RouteError(404, 'not found')
      try {
        return await handler(body ?? {}, url, signal)
      } catch (e) {
        if (e instanceof CoreHostError) throw new RouteError(400, hostProjectErrorText(e.code, e.message), e.code)
        throw e
      }
    },
  }
}
