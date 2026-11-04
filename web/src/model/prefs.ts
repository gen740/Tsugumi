// Application-wide settings kept in this browser (localStorage). Project
// settings live in the project documents instead; skill folders and the
// model connection live in the host server because it reads them.
import { Observable } from './observable'

export const isMac = typeof navigator !== 'undefined' && /Mac|iPhone|iPad/.test(navigator.userAgent)

/** Commands whose keys can be reassigned in Settings > Shortcuts. */
export const SHORTCUT_ACTIONS = [
  // The app and the project
  'newProject', 'openProject', 'home', 'duplicateProject', 'find', 'run', 'stop',
  // The canvas, while it has the focus (single keys; never while typing)
  'openSettings', 'openArtifacts', 'openLog', 'toggleCollapse', 'duplicate', 'fitAll', 'actualSize', 'zoomIn', 'zoomOut', 'arrange', 'group',
  'addDraft', 'generateDrafts',
  // Panels and the rest
  'toggleChat', 'toggleLog', 'toggleTimeline', 'toggleTerminal', 'showDetails', 'addNote', 'formatSource', 'toggleGuide',
] as const
export type ShortcutAction = (typeof SHORTCUT_ACTIONS)[number]

export const SHORTCUT_TITLES: Record<ShortcutAction, string> = {
  newProject: '新しいプロジェクト', openProject: 'プロジェクトを開く', home: 'Home へ戻る', duplicateProject: 'プロジェクトを複製',
  find: 'テキスト検索', run: 'ワークフローを実行', stop: '実行を停止', openSettings: 'Card 設定を開く', openArtifacts: '成果物を開く',
  openLog: '実行ログを開く', toggleCollapse: '折りたたみを切り替える', duplicate: 'Card を複製', fitAll: '全体表示', actualSize: '100% で表示',
  zoomIn: '拡大', zoomOut: '縮小', arrange: '整列', group: 'グループ操作', addDraft: 'Draft Card を追加', generateDrafts: 'Draft から Card を生成',
  toggleChat: 'チャットの表示切替', toggleLog: 'ログの表示切替', toggleTimeline: 'タイムラインの表示切替', toggleTerminal: 'ターミナルの表示切替',
  showDetails: '選択した Card の詳細', addNote: '付箋を追加', formatSource: 'ソースコードを整形', toggleGuide: 'ガイドのツグミに聞く',
}

/** Works only while the canvas has the focus (a key without ⌘ would
 *  otherwise be taken from a text field). */
export const CANVAS_ONLY: ReadonlySet<ShortcutAction> = new Set<ShortcutAction>([
  'openSettings', 'openArtifacts', 'openLog', 'toggleCollapse', 'duplicate', 'fitAll', 'actualSize', 'zoomIn', 'zoomOut', 'arrange', 'group',
  'addDraft', 'generateDrafts',
])

export interface KeyBinding {
  key: string
  command?: boolean
  option?: boolean
  shift?: boolean
  control?: boolean
}

export const DEFAULT_SHORTCUTS: Record<ShortcutAction, KeyBinding> = {
  newProject: { key: 'n', command: true },
  openProject: { key: 'o', command: true },
  home: { key: 'h', command: true, shift: true },
  duplicateProject: { key: 's', command: true, shift: true },
  find: { key: 'f', command: true },
  run: { key: 'r', command: true },
  stop: { key: '.', command: true },
  openSettings: { key: 's' },
  openArtifacts: { key: 'a' },
  openLog: { key: 'l' },
  toggleCollapse: { key: 'f' },
  duplicate: { key: 'c' },
  fitAll: { key: '0' },
  actualSize: { key: '1' },
  zoomIn: { key: '=', shift: true },
  zoomOut: { key: '-' },
  arrange: { key: 'a', shift: true },
  group: { key: 'g' },
  addDraft: { key: 'e' },
  generateDrafts: { key: 'd', shift: true },
  toggleChat: { key: 'j', command: true, option: true },
  toggleLog: { key: 'l', command: true, option: true },
  toggleTimeline: { key: 't', command: true, option: true },
  toggleTerminal: { key: '`', control: true },
  showDetails: { key: 'i', command: true, option: true },
  addNote: { key: 'n', command: true, option: true },
  formatSource: { key: 'f', option: true, shift: true },
  toggleGuide: { key: 'g', command: true, option: true },
}

export const sameBinding = (a: KeyBinding, b: KeyBinding) =>
  a.key === b.key && !!a.command === !!b.command && !!a.option === !!b.option && !!a.shift === !!b.shift && !!a.control === !!b.control

export function bindingLabel(b: KeyBinding): string {
  const k = b.key === 'return' ? '↩' : b.key === 'escape' ? '⎋' : b.key.length === 1 ? b.key.toUpperCase() : b.key
  if (isMac) return (b.control ? '⌃' : '') + (b.option ? '⌥' : '') + (b.shift ? '⇧' : '') + (b.command ? '⌘' : '') + k
  return [b.control && 'Ctrl', b.command && 'Ctrl', b.option && 'Alt', b.shift && 'Shift', k].filter(Boolean).join('+')
}

const CODE_KEYS: Record<string, string> = {
  Period: '.', Comma: ',', Slash: '/', Semicolon: ';', Quote: "'", BracketLeft: '[', BracketRight: ']',
  Backslash: '\\', Minus: '-', Equal: '=', Backquote: '`', Space: ' ', Enter: 'return', Escape: 'escape',
  Backspace: 'delete', Delete: 'forwarddelete', Tab: 'tab',
}

/** The key of an event independent of the keyboard layout's option glyphs. */
export function eventKey(e: KeyboardEvent): string {
  if (e.code.startsWith('Key')) return e.code.slice(3).toLowerCase()
  if (e.code.startsWith('Digit')) return e.code.slice(5)
  return CODE_KEYS[e.code] ?? e.key.toLowerCase()
}

/** ⌘ on macOS; Ctrl stands in for it elsewhere. */
export function eventBinding(e: KeyboardEvent): KeyBinding {
  return {
    key: eventKey(e),
    command: isMac ? e.metaKey : e.ctrlKey,
    option: e.altKey,
    shift: e.shiftKey,
    control: isMac ? e.ctrlKey : false,
  }
}

export type Theme = 'system' | 'light' | 'dark'
export type FlowDirection = 'topToBottom' | 'leftToRight'
export const FLOW_LABELS: Record<FlowDirection, string> = { topToBottom: '上から下', leftToRight: '左から右' }
/** Whether the Tsugumi bird (Home, loading) moves. */
export type Motion = 'system' | 'on' | 'off'
export const MOTION_LABELS: Record<Motion, string> = { system: 'システムに従う（視差効果を減らす）', on: '動かす', off: '動かさない' }
/** Colors of the code editor (syntax highlighting). */
export type EditorTheme = 'standard' | 'contrast' | 'muted'
export const EDITOR_THEME_LABELS: Record<EditorTheme, string> = { standard: '標準', contrast: '高コントラスト', muted: '控えめ' }
export type SendKey = 'returnKey' | 'commandReturn'

export const THEME_LABELS: Record<Theme, string> = { system: 'システムに従う', light: 'ライト', dark: 'ダーク' }
export const SEND_KEY_LABELS: Record<SendKey, string> = {
  returnKey: 'Return で送信（⇧Return で改行）',
  commandReturn: `${isMac ? '⌘' : 'Ctrl+'}Return で送信（Return で改行）`,
}

interface Stored {
  maxConcurrency: number
  sendKey: SendKey
  theme: Theme
  showGrid: boolean
  gridSize: number
  animateLines: boolean
  indentWidth: number
  showIndentGuides: boolean
  editorFontSize: number
  editorTheme: EditorTheme
  flowDirection: FlowDirection
  tsugumiMotion: Motion
  shortcuts: Partial<Record<ShortcutAction, KeyBinding>>
}

const DEFAULTS: Stored = {
  maxConcurrency: 4, sendKey: 'returnKey', theme: 'system', showGrid: true, gridSize: 20, animateLines: true,
  indentWidth: 4, showIndentGuides: true, editorFontSize: 12, editorTheme: 'standard', flowDirection: 'topToBottom', tsugumiMotion: 'system',
  shortcuts: {},
}

const STORAGE_KEY = 'tsugumi.prefs'

export class AppPreferences extends Observable {
  private v: Stored

  constructor() {
    super()
    let stored: Partial<Stored> = {}
    try {
      stored = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '{}')
    } catch {}
    this.v = { ...DEFAULTS, ...stored }
    this.applyTheme()
    window.matchMedia?.('(prefers-color-scheme: dark)').addEventListener('change', () => this.applyTheme())
  }

  get<K extends keyof Stored>(k: K): Stored[K] {
    return this.v[k]
  }

  set<K extends keyof Stored>(k: K, value: Stored[K]) {
    this.v = { ...this.v, [k]: value }
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(this.v))
    } catch {}
    if (k === 'theme') this.applyTheme()
    this.changed()
  }

  get maxConcurrency() { return this.v.maxConcurrency }
  get sendKey() { return this.v.sendKey }
  get theme() { return this.v.theme }
  get showGrid() { return this.v.showGrid }
  get gridSize() { return this.v.gridSize }
  get animateLines() { return this.v.animateLines }
  get indentWidth() { return this.v.indentWidth }
  get showIndentGuides() { return this.v.showIndentGuides }
  get editorFontSize() { return this.v.editorFontSize }
  get editorTheme() { return this.v.editorTheme }
  get flowDirection() { return this.v.flowDirection }
  get tsugumiMotion() { return this.v.tsugumiMotion }

  /** The bird stays still: as set here, or by default as the system's "reduce motion". */
  get tsugumiStill(): boolean {
    if (this.v.tsugumiMotion === 'system') return !!window.matchMedia?.('(prefers-reduced-motion: reduce)').matches
    return this.v.tsugumiMotion === 'off'
  }

  shortcut(a: ShortcutAction): KeyBinding {
    return this.v.shortcuts[a] ?? DEFAULT_SHORTCUTS[a]
  }

  isCustom(a: ShortcutAction) {
    return !!this.v.shortcuts[a]
  }

  setShortcut(a: ShortcutAction, b: KeyBinding | undefined) {
    const s = { ...this.v.shortcuts }
    if (b) s[a] = b
    else delete s[a]
    this.set('shortcuts', s)
  }

  /** The action already using this key, if any (to warn about duplicates). */
  conflict(b: KeyBinding, except: ShortcutAction): ShortcutAction | undefined {
    return SHORTCUT_ACTIONS.find((a) => a !== except && sameBinding(this.shortcut(a), b))
  }

  /** The action bound to a key event, if any. Canvas-only actions (single
   *  keys) count only while the canvas has the focus. */
  action(e: KeyboardEvent, canvasFocused: boolean): ShortcutAction | undefined {
    const b = eventBinding(e)
    return SHORTCUT_ACTIONS.find((a) => (canvasFocused || !CANVAS_ONLY.has(a)) && sameBinding(this.shortcut(a), b))
  }

  label(a: ShortcutAction) {
    return bindingLabel(this.shortcut(a))
  }

  get dark(): boolean {
    if (this.v.theme === 'system') return !!window.matchMedia?.('(prefers-color-scheme: dark)').matches
    return this.v.theme === 'dark'
  }

  applyTheme() {
    document.documentElement.dataset.theme = this.dark ? 'dark' : 'light'
  }
}

export const prefs = new AppPreferences()
