import { Observable } from './observable'
import type { Stage } from './canvasKit'
import { D, uuid } from '../shared/rpc'
import { CardKey, ProjectSession } from './session'

export type CardEditorCategory = 'general' | 'nix' | 'source' | 'ports' | 'executions' | 'artifacts' | 'context'
export const CATEGORY: Record<CardEditorCategory, { title: string; icon: string }> = {
  general: { title: '一般', icon: 'slider.horizontal.3' },
  nix: { title: 'Nix 定義', icon: 'snowflake' },
  source: { title: 'ソース', icon: 'doc.text' },
  ports: { title: '入出力', icon: 'arrow.left.arrow.right' },
  executions: { title: '実行履歴', icon: 'clock.arrow.circlepath' },
  artifacts: { title: '成果物', icon: 'shippingbox' },
  context: { title: '追加コンテキスト', icon: 'text.bubble' },
}

/** One tab of the workbench: a scope (the root workflow or a child workflow)
 *  with its own selection and back history. */
export class WorkflowTab extends Observable {
  readonly id = uuid()
  back: string[][] = []
  selectedConnection?: number

  constructor(public scope: string[], public selection = new Set<CardKey>()) {
    super()
  }

  setSelection(s: Iterable<CardKey>) {
    this.selection = new Set(s)
    this.changed()
  }

  /** Switches the edited scope; the previous one goes on the back stack. */
  enter(s: string[]) {
    if (s.join('/') === this.scope.join('/')) return
    this.back.push(this.scope)
    this.scope = s
    this.selection = new Set()
    this.selectedConnection = undefined
    this.changed()
  }

  goBack() {
    const s = this.back.pop()
    if (!s) return
    this.scope = s
    this.selection = new Set()
    this.selectedConnection = undefined
    this.changed()
  }
}

export type BottomPanel = 'log' | 'timeline' | 'terminal'
export const BOTTOM_PANELS: BottomPanel[] = ['log', 'timeline', 'terminal']
export const BOTTOM_TITLES: Record<BottomPanel, string> = { log: 'ログ', timeline: 'タイムライン', terminal: 'ターミナル' }
export const BOTTOM_ICONS: Record<BottomPanel, string> = { log: 'text.alignleft', timeline: 'chart.bar.xaxis', terminal: 'terminal' }

/** State of one main window: which project is open, its tabs and panels. */
export class Workspace extends Observable {
  session?: ProjectSession
  tabs: WorkflowTab[] = []
  activeTab?: string
  showChat = true
  /** The sidebar column (NavigationSplitView's columns: all / detail only). */
  showSidebar = true
  showBottom = false
  bottomPanel: BottomPanel = 'log'
  /** Log filter chosen from elsewhere (an issue, a Card menu, a stage icon). */
  logCard?: CardKey
  logInvocation?: bigint
  logStream: D.LogStream = D.LogStream.journal
  /** Stage whose log section to open (a click on a stage icon). */
  logStage?: Stage
  /** Leaves the project for Home (set by the main window; undefined in a
   *  window that shows a child workflow only). */
  switchProject?: () => void

  get active(): WorkflowTab | undefined {
    return this.tabs.find((t) => t.id === this.activeTab) ?? this.tabs[0]
  }

  open(s: ProjectSession, scope?: string[]) {
    this.session = s
    const tab = new WorkflowTab(scope ?? s.initialScope, scope ? new Set() : new Set(s.initialSelection))
    this.tabs = [tab]
    this.activeTab = tab.id
    this.changed()
  }

  close() {
    this.session = undefined
    this.tabs = []
    this.activeTab = undefined
    this.changed()
  }

  /** Opens a scope in a tab, reusing a tab that already shows it. */
  openTab(scope: string[]) {
    const t = this.tabs.find((t) => t.scope.join('/') === scope.join('/'))
    if (t) {
      this.activeTab = t.id
    } else {
      const n = new WorkflowTab(scope)
      this.tabs = [...this.tabs, n]
      this.activeTab = n.id
    }
    this.changed()
  }

  closeTab(id: string) {
    const i = this.tabs.findIndex((t) => t.id === id)
    if (this.tabs.length <= 1 || i < 0) return
    this.tabs = this.tabs.filter((t) => t.id !== id)
    if (this.activeTab === id) this.activeTab = this.tabs[Math.max(0, i - 1)].id
    this.changed()
  }

  selectTab(id: string) {
    this.activeTab = id
    this.changed()
  }

  set<K extends 'showChat' | 'showSidebar' | 'showBottom' | 'bottomPanel' | 'logCard' | 'logInvocation' | 'logStream' | 'logStage'>(k: K, v: Workspace[K]) {
    ;(this as Workspace)[k] = v
    this.changed()
  }

  /** The log / timeline / terminal icons: a click opens that panel, or closes
   *  it when it is already the one shown. */
  togglePanel(p: BottomPanel) {
    if (this.showBottom && this.bottomPanel === p) this.showBottom = false
    else {
      this.bottomPanel = p
      this.showBottom = true
    }
    this.changed()
  }

  /** Opens the log of a Card (or of the whole run), at a stage's section. */
  showLog(card: CardKey | undefined, stage?: Stage, invocation?: bigint) {
    this.logCard = card
    this.logInvocation = invocation
    this.logStage = stage
    if (card === undefined) this.logStream = D.LogStream.journal
    this.bottomPanel = 'log'
    this.showBottom = true
    this.changed()
  }
}
