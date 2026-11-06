// Application wide state: setup, the Demos catalog, recent projects, one
// ProjectSession per project shared by all of its views, and the detail
// windows (floating inside the page, like the macOS app's own windows).
import { api, onServerEvent, Status } from '../api'
import { uuid } from '../shared/rpc'
import { ChatController } from './chat'
import { Observable } from './observable'
import { CardKey, ProjectSession } from './session'
import { CardEditorCategory } from './workspace'

export type WindowKind =
  | { kind: 'card'; card: CardKey; category?: CardEditorCategory }
  | { kind: 'definitions'; definition?: string }
  | { kind: 'versions' }
  | { kind: 'executions'; card: CardKey }
  | { kind: 'artifacts'; card: CardKey }
  | { kind: 'workflowSettings' }
  | { kind: 'visual'; card: CardKey }
  | { kind: 'chat' }
  | { kind: 'appSettings' }

export interface DetailWindow {
  id: string
  session?: ProjectSession
  what: WindowKind
  x: number
  y: number
  width: number
  height: number
  z: number
}

const SIZES: Record<WindowKind['kind'], [number, number]> = {
  card: [860, 620], definitions: [900, 600], versions: [980, 620], executions: [900, 560], artifacts: [900, 560],
  workflowSettings: [680, 480], visual: [900, 600], chat: [460, 720], appSettings: [760, 520],
}

export class AppModel extends Observable {
  status?: Status
  error?: string
  /** Menu commands for Home: ⌘N asks for the new project sheet, ⌘O for the folder chooser. */
  requestNewProject = false
  requestOpenProject = false
  sessions = new Map<string, ProjectSession>()
  windows: DetailWindow[] = []
  private chats = new Map<string, ChatController>()
  // Above the canvas overlays (z-index 20–50), below popovers and sheets.
  private z = 1000

  constructor() {
    super()
    onServerEvent((e) => {
      if (e.type === 'setup' || e.type === 'reconnected') void this.refresh()
    })
  }

  async refresh() {
    try {
      this.status = await api<Status>('/api/status')
    } catch (e) {
      this.error = String((e as Error).message ?? e)
    }
    this.changed()
  }

  setError(e: unknown) {
    this.error = e === undefined ? undefined : String((e as Error)?.message ?? e)
    this.changed()
  }

  get setupNeeded(): boolean {
    const s = this.status?.setup
    return !!s && !(s.tsugumid.ok && s.worker.ok && s.sdk.ok)
  }

  /** The open session for a project folder, opening the project if needed. */
  async session(dir: string): Promise<ProjectSession> {
    const info = await api<{ path: string; key: string; name: string }>('/api/project/open', { path: dir })
    let s = this.sessions.get(info.key)
    if (!s) {
      s = new ProjectSession(info.path, info.key, info.path.split('/').pop() ?? info.name)
      s.makeRepositoryOnConnect = this.fresh.delete(info.path) || this.fresh.delete(dir)
      this.sessions.set(info.key, s)
      s.start()
    }
    void this.refresh()
    return s
  }

  /** Takes projects off the recent list (the folders stay). */
  async forgetRecents(paths: Iterable<string>) {
    try {
      await api('/api/recents/forget', { paths: [...paths] })
    } catch (e) {
      this.setError(e)
    }
    await this.refresh()
  }

  /** Projects made or copied here: they become git repositories when they
   *  first connect (a demo's history file becomes its versions). */
  private fresh = new Set<string>()

  /** Copies a bundled demo into a new, editable project (the original stays untouched). */
  async copyDemo(id: string): Promise<string> {
    const p = (await api<{ path: string }>('/api/demo/copy', { id })).path
    this.fresh.add(p)
    return p
  }

  /** Creates an empty project laid out in the given direction. */
  async createProject(parent: string, name: string, flow: string): Promise<string> {
    const p = (await api<{ path: string }>('/api/project/create', { parent, name, flow })).path
    this.fresh.add(p)
    return p
  }

  /** A copy of a project's saved documents and sources in a new folder. */
  async duplicateProject(from: string, parent: string, name: string): Promise<string> {
    return (await api<{ path: string }>('/api/project/duplicate', { path: from, parent, name })).path
  }

  request(what: 'requestNewProject' | 'requestOpenProject', v: boolean) {
    this[what] = v
    this.changed()
  }

  /** One chat per project, shared by its panel and its detached window. */
  chat(s: ProjectSession): ChatController {
    let c = this.chats.get(s.key)
    if (!c) {
      c = new ChatController(s, this)
      this.chats.set(s.key, c)
    }
    return c
  }

  /** Opens a detail window, focusing an existing one for the same target. */
  openWindow(session: ProjectSession | undefined, what: WindowKind) {
    const same = this.windows.find((w) => w.session === session && JSON.stringify(w.what) === JSON.stringify(what))
    if (same) {
      this.focusWindow(same.id)
      return
    }
    const [width, height] = SIZES[what.kind]
    const n = this.windows.length
    const vw = window.innerWidth, vh = window.innerHeight
    const w = Math.min(width, vw - 40), h = Math.min(height, vh - 60)
    this.windows = [...this.windows, {
      id: uuid(), session, what, width: w, height: h, z: ++this.z,
      x: Math.max(10, (vw - w) / 2 + n * 24), y: Math.max(10, (vh - h) / 2 - 20 + n * 24),
    }]
    this.changed()
  }

  focusWindow(id: string) {
    const w = this.windows.find((w) => w.id === id)
    if (!w || w.z === this.z) return
    w.z = ++this.z
    this.changed()
  }

  moveWindow(id: string, r: Partial<Pick<DetailWindow, 'x' | 'y' | 'width' | 'height'>>) {
    const w = this.windows.find((w) => w.id === id)
    if (!w) return
    Object.assign(w, r)
    this.changed()
  }

  closeWindow(id: string) {
    this.windows = this.windows.filter((w) => w.id !== id)
    this.changed()
  }

  /** The front-most window, when keyboard focus is in it. */
  get frontWindow(): DetailWindow | undefined {
    return [...this.windows].sort((a, b) => b.z - a.z)[0]
  }
}

export const app = new AppModel()
