// App-wide settings; the category on the left switches the content. While a
// project is open its own settings follow the app's (kept in the project).
import { ReactNode, useEffect, useState } from 'react'
import { api } from '../api'
import { app } from '../model/app'
import { guide } from '../model/guide'
import { Skill } from '../model/chat'
import { PERMISSIONS } from '../model/permissions'
import { useObserve } from '../model/observable'
import {
  bindingLabel, CANVAS_ONLY, EDITOR_THEME_LABELS, EditorTheme, eventBinding, FLOW_LABELS, FlowDirection, Motion, MOTION_LABELS, prefs,
  SEND_KEY_LABELS, SendKey, SHORTCUT_ACTIONS, SHORTCUT_TITLES, ShortcutAction, Theme, THEME_LABELS,
} from '../model/prefs'
import { errorText, ProjectSession } from '../model/session'
import { Button, copyText, Empty, Form, Row, Section, Segmented, Select, Sheet, Stepper, TextField, Toggle } from './controls'
import { CodeEditor } from './editor'
import { Icon } from './icons'
import * as projectViews from './projectViews'
import { FolderPicker } from './sidebar'
import { adoptLanguage, L, language, type Language } from '../i18n'

type Category = 'general' | 'appearance' | 'editor' | 'shortcuts' | 'skills' | 'api' | 'connections'
const CATS: { id: Category; title: string; icon: string; storage: string }[] = [
  { id: 'general', title: L('一般'), icon: 'gearshape', storage: L('このブラウザ（localStorage）') },
  { id: 'appearance', title: L('外観'), icon: 'paintpalette', storage: L('このブラウザ（localStorage）') },
  { id: 'editor', title: L('エディター'), icon: 'text.cursor', storage: L('このブラウザ（localStorage）') },
  { id: 'shortcuts', title: L('ショートカット'), icon: 'keyboard', storage: L('このブラウザ（localStorage）') },
  { id: 'skills', title: 'Skills', icon: 'wand.and.stars', storage: L('このホスト（Tsugumi web サーバーの設定）') },
  { id: 'api', title: 'API', icon: 'key', storage: L('このホスト（API キーは macOS のキーチェーン）') },
  { id: 'connections', title: L('外部接続'), icon: 'network', storage: L('表示のみ（開いているプロジェクトの接続先）') },
]

/** The settings of one project: kept in the project (or its environment). */
export type ProjectSettingsPane = 'general' | 'canvas' | 'databases' | 'history' | 'cache'
const PANES: { id: ProjectSettingsPane; title: string; icon: string }[] = [
  { id: 'general', title: L('名前と実行環境'), icon: 'folder.badge.gearshape' }, { id: 'canvas', title: 'Canvas', icon: 'rectangle.3.group' },
  { id: 'databases', title: L('データベース接続先'), icon: 'cylinder.split.1x2' },
  { id: 'history', title: L('実行履歴'), icon: 'clock.arrow.circlepath' }, { id: 'cache', title: L('ビルドキャッシュ'), icon: 'shippingbox' },
]
function paneStorage(p: ProjectSettingsPane, s: ProjectSession): string {
  const name = s.dir.split('/').pop() ?? ''
  switch (p) {
    case 'general': case 'databases': return L('このプロジェクト（{0}/tsugumi.json）', name)
    case 'canvas': return L('このプロジェクト（{0}/layout.json）', name)
    case 'history': return L('このプロジェクトの実行環境（書き出しは {0}/.tsugumi/runs.sqlite）', name)
    case 'cache': return L('このプロジェクトの実行環境（このホスト）')
  }
}

/** The project panes come from the detail area when it provides them. */
const ProjectSettingsContent = (projectViews as unknown as {
  ProjectSettingsContent?: (p: { pane: ProjectSettingsPane; session: ProjectSession }) => ReactNode
}).ProjectSettingsContent

const STORE = { category: 'tsugumi.settings.category', project: 'tsugumi.settings.project' }
const load = (k: string, d: string) => {
  try {
    return localStorage.getItem(k) ?? d
  } catch {
    return d
  }
}
const store = (k: string, v: string) => {
  try {
    localStorage.setItem(k, v)
  } catch {}
}

/** Opens the settings window at a project's pane. */
export function openProjectSettings(s: ProjectSession, pane: ProjectSettingsPane = 'general') {
  store(STORE.category, 'project.' + pane)
  store(STORE.project, s.dir)
  window.dispatchEvent(new CustomEvent(OPEN_EVENT, { detail: { category: 'project.' + pane, project: s.dir } }))
  app.openWindow(undefined, { kind: 'appSettings' })
}
const OPEN_EVENT = 'tsugumi:openSettings'

export function ApplicationSettings() {
  useObserve(prefs, app)
  /** "<category>" for the app's panes, "project.<pane>" for the project's. */
  const [stored, setStored] = useState(() => load(STORE.category, 'general'))
  const [projectPath, setProjectPath] = useState(() => load(STORE.project, ''))
  const select = (v: string) => {
    setStored(v)
    store(STORE.category, v)
  }
  // Opened again at another pane while it is open.
  useEffect(() => {
    const f = (e: Event) => {
      const d = (e as CustomEvent<{ category: string; project: string }>).detail
      setStored(d.category)
      setProjectPath(d.project)
    }
    window.addEventListener(OPEN_EVENT, f)
    return () => window.removeEventListener(OPEN_EVENT, f)
  }, [])
  const projects = [...app.sessions.values()].sort((a, b) => a.scopeName([]).localeCompare(b.scopeName([])))
  const project = projects.find((s) => s.dir === projectPath) ?? projects[0]
  const pane = stored.startsWith('project.') && project ? PANES.find((p) => 'project.' + p.id === stored) : undefined
  const category = CATS.find((c) => c.id === stored) ?? CATS[0]
  const current = pane ? 'project.' + pane.id : category.id

  return (
    <div className="settings-window">
      <div className="settings-nav">
        <div className="settings-nav-header">{L('アプリ')}</div>
        {CATS.map((c) => (
          <button key={c.id} type="button" className={`nav-row${current === c.id ? ' selected' : ''}`} onClick={() => select(c.id)}>
            <Icon name={c.icon} />{c.title}
          </button>
        ))}
        {project && (
          <>
            <div className="settings-nav-header">
              {projects.length > 1 ? (
                <Select value={project.dir} help={L('設定を表示するプロジェクト（開いているプロジェクトから選びます）')}
                  onChange={(v) => { setProjectPath(v); store(STORE.project, v) }} options={projects.map((s) => ({ value: s.dir, label: s.scopeName([]) }))} />
              ) : (
                <>
                  <span>{L('このプロジェクト')}</span>
                  <span className="caption">{project.scopeName([])}</span>
                </>
              )}
            </div>
            {PANES.map((p) => (
              <button key={p.id} type="button" className={`nav-row${current === 'project.' + p.id ? ' selected' : ''}`} onClick={() => select('project.' + p.id)}>
                <Icon name={p.icon} />{p.title}
              </button>
            ))}
          </>
        )}
      </div>
      <div className="settings-content">
        <div className="settings-title">{pane && project ? `${pane.title} — ${project.scopeName([])}` : category.title}</div>
        <div className="storage-note"><Icon name="externaldrive" size={11} />保存先: {pane && project ? paneStorage(pane.id, project) : category.storage}</div>
        <div className="settings-body" key={pane && project ? `${project.key}.${pane.id}` : category.id}>
          {pane && project ? <ProjectPane pane={pane.id} session={project} /> : (
            <>
              {category.id === 'general' && <GeneralSettings />}
              {category.id === 'appearance' && <AppearanceSettings />}
              {category.id === 'editor' && <EditorSettings />}
              {category.id === 'shortcuts' && <ShortcutSettings />}
              {category.id === 'skills' && <SkillSettings />}
              {category.id === 'api' && <APISettings />}
              {category.id === 'connections' && <ConnectionSettings />}
            </>
          )}
        </div>
      </div>
    </div>
  )
}

function ProjectPane({ pane, session }: { pane: ProjectSettingsPane; session: ProjectSession }) {
  if (ProjectSettingsContent) return <>{ProjectSettingsContent({ pane, session })}</>
  // Until the detail area provides the separate panes, the shared view holds them.
  if (pane !== 'history') return <projectViews.WorkflowSettingsView session={session} />
  return <Empty title={L('実行履歴')} icon="clock.arrow.circlepath" description={L('実行履歴は Workflow の「実行履歴」から見られます。')} />
}

const options = <T extends string>(labels: Record<T, string>) => (Object.keys(labels) as T[]).map((k) => ({ value: k, label: labels[k] }))

function GeneralSettings() {
  return (
    <Form>
      <Section>
        {/* Explanations on hover, not as text under each control. */}
        <Row label={L('言語')} help={L('変えるとページを読み込み直します。プロジェクトの git に書く実行の記録もこの言語になります。')}>
          <Select<Language> value={language()} onChange={(v) => void api('/api/settings', { language: v }).then(() => adoptLanguage(v))} options={[{ value: 'ja', label: '日本語' }, { value: 'en', label: 'English' }]} />
        </Row>
        <Row help={L('別々の Card を同時に実行する上限です（同じ Card は常に直列）。次の実行から使われます。')}>
          <Stepper label={L('並列実行数: {0}', prefs.maxConcurrency)} value={prefs.maxConcurrency} min={1} max={64} onChange={(v) => prefs.set('maxConcurrency', v)} />
        </Row>
        <Row label={L('チャットの送信キー')}>
          <Select<SendKey> value={prefs.sendKey} onChange={(v) => prefs.set('sendKey', v)} options={options(SEND_KEY_LABELS)} />
        </Row>
        <Row label={L('Tsugumi のアニメーション')} help={L('Home と読み込み中の Tsugumi を動かすかどうか。デフォルトは macOS の「視差効果を減らす」（ブラウザの「動きを減らす」）に合わせます。')}>
          <Select<Motion> value={prefs.tsugumiMotion} onChange={(v) => prefs.set('tsugumiMotion', v)} options={options(MOTION_LABELS)} />
        </Row>
      </Section>
    </Form>
  )
}

function AppearanceSettings() {
  return (
    <div className="settings-split">
      <Form style={{ width: 340, flex: 'none' }}>
        <Section>
          <Row label={L('テーマ')}><Select<Theme> value={prefs.theme} onChange={(v) => prefs.set('theme', v)} options={options(THEME_LABELS)} /></Row>
          <Row label={L('新しいプロジェクトの流れの向き')} help={L('新しく作るプロジェクトの Card を並べる向きです。開いているプロジェクトの向きは、設定の「このプロジェクト」→「Canvas」で変えます。')}>
            <Select<FlowDirection> value={prefs.flowDirection} onChange={(v) => prefs.set('flowDirection', v)} options={options(FLOW_LABELS)} />
          </Row>
          <Row label={L('グリッドを表示（移動時に吸着）')}><Toggle checked={prefs.showGrid} onChange={(v) => prefs.set('showGrid', v)} /></Row>
          <Row label={L('グリッド間隔 {0}', prefs.gridSize)}>
            <input type="range" min={10} max={60} step={5} value={prefs.gridSize} onChange={(e) => prefs.set('gridSize', Number(e.target.value))} aria-label={L('グリッド間隔')} />
          </Row>
          <Row label={L('実行中の接続線を動かす')}><Toggle checked={prefs.animateLines} onChange={(v) => prefs.set('animateLines', v)} /></Row>
        </Section>
      </Form>
      <div className="settings-preview">
        <div className="caption secondary">{L('プレビュー（プロジェクトは変わりません）')}</div>
        <CanvasPreview />
      </div>
    </div>
  )
}

/** Two Cards and a running wire, drawn as the canvas draws them. */
function CanvasPreview() {
  const [, tick] = useState(0)
  useEffect(() => {
    if (!prefs.animateLines) return
    const t = setInterval(() => tick((x) => x + 1), 1000 / 30)
    return () => clearInterval(t)
  }, [prefs.animateLines])
  const W = 300, H = 220, g = prefs.gridSize
  const phase = prefs.animateLines ? -((Date.now() / 1000) * 30) % 14 : 0
  const down = prefs.flowDirection === 'topToBottom'
  const a = down ? { x: 40, y: 24 } : { x: 16, y: 40 }
  const b = down ? { x: 160, y: 140 } : { x: 190, y: 130 }
  const cw = 100, ch = 48
  const from = down ? { x: a.x + cw / 2, y: a.y + ch } : { x: a.x + cw, y: a.y + ch / 2 }
  const to = down ? { x: b.x + cw / 2, y: b.y } : { x: b.x, y: b.y + ch / 2 }
  const d = down
    ? `M${from.x},${from.y} C${from.x},${(from.y + to.y) / 2} ${to.x},${(from.y + to.y) / 2} ${to.x},${to.y}`
    : `M${from.x},${from.y} C${(from.x + to.x) / 2},${from.y} ${(from.x + to.x) / 2},${to.y} ${to.x},${to.y}`
  return (
    <svg width={W} height={H} className="canvas-preview" role="img" aria-label={L('Canvas のプレビュー')}>
      <rect width={W} height={H} fill="var(--text-bg)" />
      {prefs.showGrid && Array.from({ length: Math.ceil(W / g) }, (_, i) => Array.from({ length: Math.ceil(H / g) }, (_, j) => (
        <circle key={`${i}-${j}`} cx={i * g + 0.8} cy={j * g + 0.8} r={0.8} fill="var(--grid-dot)" />
      )))}
      {[a, b].map((p, i) => (
        <g key={i}>
          <rect x={p.x} y={p.y} width={cw} height={ch} rx={8} fill="var(--control-bg)" stroke="var(--separator-strong)" />
          <text x={p.x + 10} y={p.y + 20} fontSize={11} fontWeight={600} fill="var(--label)">{i === 0 ? 'load' : 'analyze'}</text>
          <text x={p.x + 10} y={p.y + 36} fontSize={9} fill="var(--secondary)">{i === 0 ? 'Python' : 'C++'}</text>
        </g>
      ))}
      <path d={d} fill="none" stroke="var(--accent)" strokeWidth={1.8} strokeDasharray={prefs.animateLines ? '8 6' : undefined} strokeDashoffset={phase} />
    </svg>
  )
}

const SAMPLES: Record<string, string> = {
  python: `# Collatz steps for one starting value
def steps(n: int, limit: int = 1000) -> list[int]:
    """Returns the sequence down to 1."""
    seen = [n]
    while n != 1 and len(seen) < limit:
        if n % 2 == 0:
            n = n // 2
        else:
            n = 3 * n + 1
        seen.append(n)
    return seen

print("27 ->", len(steps(27)) - 1, "steps")
`,
  cpp: `// Collatz steps for one starting value
#include <cstdint>
#include <vector>

std::vector<uint64_t> steps(uint64_t n, size_t limit = 1000) {
  std::vector<uint64_t> seen{n};
  while (n != 1 && seen.size() < limit) {
    if (n % 2 == 0) {
      n /= 2;
    } else {
      n = 3 * n + 1;  // may overflow for large n
    }
    seen.push_back(n);
  }
  return seen;
}
`,
  nix: `# A Python Card with one extra package
{ pkgs, inputs, params }:
{
  run = inputs.tsugumi.pythonCard {
    src = ./src;
    packages = ps: [ ps.numpy ];
  };
  exports = { scale = x: x * 2; };
}
`,
}

function EditorSettings() {
  const [language, setLanguage] = useState('python')
  const [sample, setSample] = useState(SAMPLES.python)
  return (
    <div className="settings-split">
      <Form style={{ width: 300, flex: 'none' }}>
        <Section>
          <Row label={L('インデント幅')}><Segmented<number> value={prefs.indentWidth} onChange={(v) => prefs.set('indentWidth', v)} options={[2, 4, 8].map((n) => ({ value: n, label: String(n) }))} /></Row>
          <Row label={L('インデントガイドを表示')}><Toggle checked={prefs.showIndentGuides} onChange={(v) => prefs.set('showIndentGuides', v)} /></Row>
          <Row><Stepper label={L('文字サイズ: {0}', prefs.editorFontSize)} value={prefs.editorFontSize} min={9} max={24} onChange={(v) => prefs.set('editorFontSize', v)} /></Row>
          <Row label={L('配色')}><Select<EditorTheme> value={prefs.editorTheme} onChange={(v) => prefs.set('editorTheme', v)} options={options(EDITOR_THEME_LABELS)} /></Row>
        </Section>
      </Form>
      <div className="settings-preview">
        <Segmented<string> value={language} width={240} onChange={(l) => { setLanguage(l); setSample(SAMPLES[l] ?? '') }}
          options={[{ value: 'python', label: 'Python' }, { value: 'cpp', label: 'C++' }, { value: 'nix', label: 'Nix' }]} />
        {/* The same editor as Card settings: what you see here is what you get there. */}
        <CodeEditor key={`${prefs.editorFontSize}-${prefs.indentWidth}-${prefs.showIndentGuides}-${prefs.editorTheme}`} text={sample} onChange={setSample} language={language} style={{ height: 300 }} />
        <div className="caption secondary">{L('サンプルは編集して試せます（保存されません）。Tab はインデント幅の空白を入れます。')}</div>
      </div>
    </div>
  )
}

function ShortcutSettings() {
  const [recording, setRecording] = useState<ShortcutAction>()
  // Captures the next key press while recording a shortcut (the app's own
  // shortcuts pause while body[data-recording] is set).
  useEffect(() => {
    if (!recording) return
    document.body.dataset.recording = '1'
    const key = (e: KeyboardEvent) => {
      if (['Meta', 'Control', 'Alt', 'Shift', 'CapsLock'].includes(e.key) || e.isComposing) return
      e.preventDefault()
      e.stopPropagation()
      const b = eventBinding(e)
      // Canvas keys may be single keys (they never reach a text field); the others need a modifier.
      if (!b.command && !b.control && !b.option) {
        if (e.key === 'Escape') return setRecording(undefined)
        if (!CANVAS_ONLY.has(recording)) return
      }
      prefs.setShortcut(recording, b)
      setRecording(undefined)
    }
    window.addEventListener('keydown', key, true)
    return () => {
      delete document.body.dataset.recording
      window.removeEventListener('keydown', key, true)
    }
  }, [recording])
  return (
    <div className="col gap8 pad16 fill">
      <div className="table shortcut-table flex1" style={{ margin: 0 }}>
        <div className="table-head three"><span>{L('操作')}</span><span>キー</span><span /></div>
        {SHORTCUT_ACTIONS.map((a) => {
          const c = prefs.conflict(prefs.shortcut(a), a)
          return (
            <div key={a} className="table-row three">
              <span>{SHORTCUT_TITLES[a]}{CANVAS_ONLY.has(a) && <span className="shortcut-scope" title={L('Canvas にフォーカスがあるときだけ働きます（文字の入力中は働きません）')}>Canvas</span>}</span>
              <span className="row gap6 center">
                <span className="mono" style={{ color: recording === a ? 'var(--accent)' : undefined }}>{recording === a ? L('キーを押してください…') : bindingLabel(prefs.shortcut(a))}</span>
                {c && <Icon name="exclamationmark.triangle.fill" style={{ color: 'var(--yellow)' }} title={L('「{0}」と重複しています', SHORTCUT_TITLES[c])} />}
              </span>
              <span className="row gap6">
                <Button small onClick={() => setRecording(recording === a ? undefined : a)}>{recording === a ? L('キャンセル') : L('変更')}</Button>
                <Button small disabled={!prefs.isCustom(a)} onClick={() => prefs.setShortcut(a, undefined)}>{L('デフォルト')}</Button>
              </span>
            </div>
          )
        })}
      </div>
      <div className="caption secondary">{L('変更はメニューにすぐ反映されます。ほかに ⌘Z / ⇧⌘Z（取り消す・やり直す）、Delete（選択の削除）、⌘,（アプリ設定）が使えます。ブラウザが予約しているキー（⌘W・⌘T・⌘N・⌘Q など）は割り当てても届きません。')}</div>
    </div>
  )
}

type SkillFolder = { path: string; enabled: boolean }

/** Reloads the Skills of every open chat (after the folders changed). */
function reloadChats() {
  for (const s of app.sessions.values()) void app.chat(s).reloadSkills()
}

function SkillSettings() {
  const [folders, setFolders] = useState<SkillFolder[]>()
  const [exists, setExists] = useState<Record<string, boolean>>({})
  const [query, setQuery] = useState('')
  const [state, setState] = useState<'idle' | 'loading' | 'loaded' | { failed: string }>('idle')
  const [found, setFound] = useState<Skill[]>([])
  const [picking, setPicking] = useState(false)

  /** Reads the folders on the host; a slow or missing folder never blocks the window. */
  const reload = async (list?: SkillFolder[]) => {
    setState('loading')
    try {
      const f = list ?? (await api<{ skillFolders: SkillFolder[] }>('/api/settings')).skillFolders
      setFolders(f)
      const ex = await api<Record<string, boolean>>('/api/settings/skillFolderStatus', { paths: f.map((x) => x.path) })
      setExists(ex)
      setFound(await api<Skill[]>('/api/skills', {}))
      const missing = f.filter((x) => x.enabled && !ex[x.path]).map((x) => x.path)
      setState(missing.length ? { failed: L('見つからないフォルダ: ') + missing.join(', ') } : 'loaded')
      reloadChats()
    } catch (e) {
      setState({ failed: errorText(e) })
    }
  }
  useEffect(() => {
    void reload()
  }, [])
  const save = async (next: SkillFolder[]) => {
    setFolders(next)
    await api('/api/settings', { skillFolders: next })
    await reload(next)
  }
  const q = query.toLowerCase()
  const shown = found.filter((s) => !q || s.name.toLowerCase().includes(q) || s.description.toLowerCase().includes(q))
  return (
    <div className="col gap8 pad16 fill">
      <div className="callout secondary">{L('チャットで `$名前` として使う Skill のフォルダです。各フォルダの `<名前>/SKILL.md` または `<名前>.md` を読み込みます。プロジェクトの `skills/` も常に読み込みます。')}</div>
      <div className="groupbox col gap4">
        <div className="headline">{L('フォルダ')}</div>
        {folders && !folders.length && <div className="secondary">{L('フォルダはありません')}</div>}
        {folders?.map((f) => (
          <div key={f.path} className="skill-folder-row">
            <input type="checkbox" checked={f.enabled} aria-label={L('{0} を使う', f.path)} onChange={(e) => void save(folders.map((x) => (x.path === f.path ? { ...x, enabled: e.target.checked } : x)))} />
            <Icon name={exists[f.path] === false ? 'folder.badge.questionmark' : 'folder'} style={{ color: exists[f.path] === false ? 'var(--orange)' : 'var(--secondary)' }} />
            <span className="ellipsis flex1" title={f.path}>{f.path}</span>
            <Button kind="borderless" iconOnly icon="minus.circle" help={L('一覧から外す')} onClick={() => void save(folders.filter((x) => x.path !== f.path))} />
          </div>
        ))}
        <div className="row gap8">
          <Button onClick={() => setPicking(true)}>{L('フォルダを追加…')}</Button>
          <Button disabled={state === 'loading'} onClick={() => void reload()}>{L('再読込')}</Button>
        </div>
      </div>
      <div className="row gap8 center">
        <TextField value={query} onChange={setQuery} placeholder={L('Skill を検索')} style={{ flex: 1 }} />
        {state === 'loading' && <span className="spinner" style={{ width: 14, height: 14 }} />}
      </div>
      <div className="list bordered flex1">
        {typeof state === 'object' && <div className="list-row" style={{ color: 'var(--red)' }}><Icon name="exclamationmark.triangle.fill" />{state.failed}</div>}
        {state === 'loading' && !found.length ? <div className="list-row secondary">{L('読み込み中…')}</div>
          : !shown.length ? <div className="list-row secondary">{found.length ? L('一致する Skill はありません') : L('Skill はまだありません')}</div>
          : shown.map((sk) => (
            <div key={sk.path} className="list-row col-start">
              <div className="mono callout">${sk.name}</div>
              <div className="caption secondary ellipsis">{sk.description} · {sk.source}</div>
            </div>
          ))}
      </div>
      {picking && <FolderPicker message={L('Skill のフォルダを選んでください')} onClose={() => setPicking(false)} onPick={(p) => {
        setPicking(false)
        if (folders && !folders.some((f) => f.path === p)) void save([...folders, { path: p, enabled: true }])
      }} />}
    </div>
  )
}

type ProviderId = 'anthropic' | 'openai' | 'chatgpt' | 'fixture'
type KeySource = 'env' | 'keychain' | 'file' | null
interface APIState {
  provider: ProviderId
  model: string
  openAIModel: string
  chatGPTModel: string
  fixturePort: number
  models: Partial<Record<'anthropic' | 'openai' | 'chatgpt', string[]>>
  keys: { Anthropic: KeySource; OpenAI: KeySource }
  keyEnv: { Anthropic: string; OpenAI: string }
  allowedPermissions: string[]
  webSearch: boolean
}

/** Re-reads the connection of every open chat (after it changed here);
 *  `saved`: the user's own save of a key or sign-in (the guide may then ask to show the demo). */
function refreshChats(saved = false) {
  for (const s of app.sessions.values()) void app.chat(s).refreshProvider()
  // The guide is there exactly while the chosen provider has a key.
  void (saved ? guide.connectionSaved() : guide.refresh())
}

/** Model connections. API keys stay on this host (the macOS Keychain); they are
 *  never sent to this page, tsugumid or Cards, nor written to project files,
 *  settings or logs. */
function APISettings() {
  const [s, setS] = useState<APIState>()
  const reload = () => api<APIState>('/api/settings').then((v) => {
    setS(v)
    return v
  })
  useEffect(() => {
    void reload()
  }, [])
  const update = (v: Partial<APIState>) => {
    setS((x) => (x ? { ...x, ...v } : x))
    void api('/api/settings', v).then(refreshChats)
  }
  if (!s) return null
  return (
    <Form>
      {/* Every connection can be set up; this picks the one the chat and Tsugumi use (the chat's model menu switches it too). */}
      <Section>
        <Row label={L('使う接続先')}>
          <Select<ProviderId> value={s.provider} onChange={(v) => update({ provider: v })} options={[
            { value: 'anthropic', label: L('Anthropic API（API キー）') }, { value: 'openai', label: L('OpenAI API（API キー）') },
            { value: 'chatgpt', label: L('ChatGPT プラン（Sign in with ChatGPT）') },
            { value: 'fixture', label: L('ローカル fixture（テスト用・通信なし）') },
          ]} />
        </Row>
        {s.provider === 'fixture' && (
          <>
            <Row label={L('fixture のポート')}>
              <input type="number" className="field" value={s.fixturePort} min={1} max={65535} onChange={(e) => update({ fixturePort: Number(e.target.value) })} style={{ width: 100 }} />
            </Row>
            <Row><span className="caption mono selectable">起動: python3 tests/fixtures/model_fixture.py --port {s.fixturePort}</span></Row>
          </>
        )}
        <Row><span className="caption secondary" style={{ whiteSpace: 'normal' }}>{L('API キーと ChatGPT のサインインは、下でそれぞれ同時に設定しておけます。チャットのモデルの一覧からも切り替えられます。')}</span></Row>
      </Section>
      <KeySection id="anthropic" s={s} reload={reload} update={update} />
      <KeySection id="openai" s={s} reload={reload} update={update} />
      <ChatGPTSettings model={s.chatGPTModel} setModel={(m) => update({ chatGPTModel: m })} inUse={s.provider === 'chatgpt'} />
      <Section title={L('Agent が確認なしに Card に与えてよい権限')}>
        {PERMISSIONS.map((p) => (
          <Row key={p.kind} label={<span><Icon name={p.icon} /> {p.label}</span>}>
            <Toggle checked={s.allowedPermissions.includes(p.kind)}
              onChange={(on) => update({ allowedPermissions: on ? [...s.allowedPermissions, p.kind] : s.allowedPermissions.filter((k) => k !== p.kind) })} />
          </Row>
        ))}
        <Row><span className="caption secondary" style={{ whiteSpace: 'normal' }}>{L('オフの権限を Agent が Card に与えようとすると、保存の前に確認します。あなた自身が Card の設定で変えるときは確認しません。')}</span></Row>
      </Section>
      <Section title={L('Web 検索')}>
        <Row label={L('Agent が自分の判断で Web を検索する')}><Toggle checked={s.webSearch} onChange={(v) => update({ webSearch: v })} /></Row>
        <Row><span className="caption secondary" style={{ whiteSpace: 'normal' }}>{s.provider === 'fixture'
          ? L('ローカル fixture には検索がないため、オンでも使えません。')
          : L('オンにすると、検索語が接続先のプロバイダを通して検索サービスへ送られます。Agent は未公開のデータを検索語に入れないよう指示されていますが、確実に防げるものではありません。検索語と出典はチャットに残ります。検索には利用料がかかることがあります。')}</span></Row>
      </Section>
      <GuideSettings />
      <div className="caption secondary">{L('チャットは任意機能です。通常の操作はすべて UI から行えます。')}</div>
    </Form>
  )
}

/** The guide bird: whether it is shown, and the first start's offer again. */
function GuideSettings() {
  useObserve(guide)
  return (
    <Section title={L('ガイドのツグミ')}>
      <Row label={L('状態')}><span className="secondary">{guide.connected ? L('右下に表示中') : L('API キーが未設定のため、決まったデモの案内だけをしています')}</span></Row>
      {/* Pressed, it says what happens next and waits until it has happened. */}
      <Row><Button disabled={guide.offersDemo} onClick={() => void guide.resetIntroduction()}>{L('初回の案内（デモの勧誘）をもう一度表示')}</Button></Row>
      {guide.offersDemo && <Row><span className="label callout" style={{ color: 'var(--green)', whiteSpace: 'normal' }}><Icon name="checkmark.circle.fill" />{L('設定しました。Home に戻ると、ツグミが「何をしてみますか？」と尋ねます。')}</span></Row>}
      <Row><span className="caption secondary" style={{ whiteSpace: 'normal' }}>{L('ガイドは上の接続先とキーを使います。キーがあると質問に答え、ないあいだは決まったデモの案内だけをします（通信はしません）。質問に答えるだけではプロジェクトを変えません。')}</span></Row>
    </Section>
  )
}

interface ChatGPTAccountInfo { clientId: string; email: string | null; signedIn: boolean; planEnabled: boolean; active: boolean }
interface ChatGPTState {
  accounts: ChatGPTAccountInfo[]
  signingIn: boolean
  outcome: { ok: boolean; message: string; welcome?: string } | null
  usageURL: string
}

/** Sign in with ChatGPT: the Agent uses the user's ChatGPT plan. This server
 *  keeps the registrations and tokens in a 0600 file of its data folder; the
 *  page never sees a token. */
function ChatGPTSettings({ model, setModel, inUse }: { model: string; setModel: (m: string) => void; inUse?: boolean }) {
  const [st, setSt] = useState<ChatGPTState>()
  const [message, setMessage] = useState<string>()
  const [models, setModels] = useState<{ id: string; label: string }[]>([])
  const [url, setUrl] = useState<string>()
  const reload = () => api<ChatGPTState>('/api/chatgpt').then((v) => {
    setSt(v)
    return v
  })
  useEffect(() => {
    void reload()
  }, [])
  // While a sign-in runs in the browser tab, its outcome is read back.
  useEffect(() => {
    if (!st?.signingIn) return
    const t = setInterval(() => void reload().then((v) => {
      if (v.signingIn) return
      setUrl(undefined)
      if (v.outcome) setMessage(v.outcome.message)
      refreshChats(!!v.outcome?.ok)
    }), 1000)
    return () => clearInterval(t)
  }, [st?.signingIn])
  if (!st) return null
  const active = st.accounts.find((a) => a.active)
  const run = async (f: () => Promise<string | undefined>) => {
    try {
      setMessage(await f())
    } catch (e) {
      setMessage(errorText(e))
    }
  }
  const signIn = (clientId?: string, consent = false) => {
    // The tab opens within the click (a pop-up blocker allows it), then goes to the sign-in.
    const tab = window.open('about:blank', '_blank')
    void run(async () => {
      let r: { url: string }
      try {
        r = await api<{ url: string }>('/api/chatgpt/signin', { clientId, consent })
      } catch (e) {
        tab?.close()
        throw e
      }
      setUrl(r.url)
      if (tab) {
        tab.opener = null
        tab.location.href = r.url
      }
      await reload()
      return undefined
    })
  }
  const welcome = st.outcome?.welcome
  const welcomed = (go: boolean) => {
    if (go) window.open(st.usageURL, '_blank', 'noopener,noreferrer')
    void api<ChatGPTState>('/api/chatgpt/welcomed', { clientId: welcome }).then(setSt)
  }
  return (
    <>
      <Section title={L('ChatGPT プラン（Sign in with ChatGPT）{0}', inUse ? '（使用中）' : '')}>
        <Row><span className="callout secondary" style={{ whiteSpace: 'normal' }}>{L('ChatGPT の Plus・Pro プランで Agent を使います。リクエストは OpenAI の Responses API に送られ、プランの利用枠から使われます（API キーの課金とは別で、Tsugumi に料金はかかりません）。Plus の 5 時間ごとの利用上限は、ほかのアプリ（オープンソースのツールを含む）と共有です。Pro にはこの上限はありません。')}</span></Row>
        {active && <Row label={L('使用中のアカウント')}><span className="selectable">{active.email ?? active.clientId}</span></Row>}
        {active?.signedIn && !active.planEnabled && (
          <Row><span className="caption" style={{ color: 'var(--orange)', whiteSpace: 'normal' }}>{L('このアカウントでは ChatGPT プランの利用が許可されていないため、Agent は使えません。許可するか、API キーの接続を選んでください。')}</span></Row>
        )}
        {st.accounts.map((a) => (
          <Row key={a.clientId} label={<span><Icon name={a.active ? 'checkmark.circle.fill' : 'circle'} /> {a.email ?? a.clientId}</span>}>
            <div className="row gap8 wrap center">
              <span className="caption secondary">{!a.signedIn ? L('サインアウト済み') : a.planEnabled ? L('サインイン中') : L('サインイン中（プランの利用は未許可）')}</span>
              {a.signedIn && !a.active && <Button small onClick={() => void run(async () => { setSt(await api<ChatGPTState>('/api/chatgpt/select', { clientId: a.clientId })); refreshChats(); return undefined })}>{L('このアカウントを使う')}</Button>}
              {!a.signedIn && <Button small disabled={st.signingIn} onClick={() => signIn(a.clientId)}>Continue with ChatGPT</Button>}
              {a.signedIn && !a.planEnabled && <Button small disabled={st.signingIn} onClick={() => signIn(a.clientId, true)}>{L('プランの利用を許可')}</Button>}
              {a.signedIn && <Button small kind="destructive" onClick={() => void run(async () => {
                const r = await api<ChatGPTState & { message: string }>('/api/chatgpt/signout', { clientId: a.clientId })
                setSt(r)
                refreshChats()
                return r.message
              })}>{L('サインアウト')}</Button>}
            </div>
          </Row>
        ))}
        <Row>
          <div className="row gap8 wrap center">
            {st.signingIn
              ? <>
                  <span className="caption secondary">{L('ブラウザでサインインを続けてください…')}</span>
                  {url && <a className="caption" href={url} target="_blank" rel="noopener noreferrer">{L('サインインのページを開く')}</a>}
                  <Button small onClick={() => void api<ChatGPTState>('/api/chatgpt/cancel', {}).then(setSt)}>{L('中止')}</Button>
                </>
              : <Button kind="prominent" onClick={() => signIn()}>{st.accounts.length ? L('Continue with ChatGPT（別のアカウント）') : 'Continue with ChatGPT'}</Button>}
            <a href={st.usageURL} target="_blank" rel="noopener noreferrer">Manage usage</a>
          </div>
        </Row>
        <Row><span className="caption secondary" style={{ whiteSpace: 'normal' }}>{L('サインインはこのブラウザで行い、Web UI のサーバーが 127.0.0.1 で結果を受け取ります（サーバーと同じマシンのブラウザで開いてください）。トークンはこのサーバーのデータフォルダのファイル（0600）に置かれ、ページ・core・Card には渡されず、プロジェクト・設定・ログにも残りません。macOS アプリのサインインとは別です。')}</span></Row>
      </Section>
      <Section title={L('モデル')}>
        <Row label={L('モデル')}><TextField value={model} onChange={setModel} /></Row>
        <Row>
          <Button disabled={!active?.signedIn} onClick={() => void run(async () => {
            const r = await api<{ models: string[]; modelNames: Record<string, string> }>('/api/settings/models', { provider: 'chatgpt' })
            setModels(r.models.map((id) => ({ id, label: r.modelNames[id] ?? id })))
            return L('{0} 件のモデルを取得しました。', r.models.length)
          })}>{L('モデル一覧を取得')}</Button>
        </Row>
        {models.length > 0 && <Row label={L('一覧から選択')}><Select value={model} onChange={setModel} options={models.map((m) => ({ value: m.id, label: m.label }))} /></Row>}
      </Section>
      {message && <div className="caption selectable">{message}</div>}
      {welcome && (
        <Sheet width={420} onClose={() => welcomed(false)}>
          <div className="sheet-body">
            <b>{L('ChatGPT プランを使っています')}</b>
            <span className="callout secondary" style={{ whiteSpace: 'normal' }}>{L('Agent のリクエストは ChatGPT プランの利用枠から使われます。利用状況は ChatGPT の設定（Usage）で確認・管理できます。')}</span>
            <div className="row gap8" style={{ justifyContent: 'flex-end' }}>
              <Button onClick={() => welcomed(true)}>Manage usage</Button>
              <Button kind="prominent" onClick={() => welcomed(false)}>Got it</Button>
            </div>
          </div>
        </Sheet>
      )}
    </>
  )
}

/** One API key connection: its key (on this host only), its model and the models it offers. */
function KeySection({ id, s, reload, update }: { id: 'anthropic' | 'openai'; s: APIState; reload: () => Promise<APIState>; update: (v: Partial<APIState>) => void }) {
  const [key, setKey] = useState('')
  const [models, setModels] = useState<string[]>(s.models[id] ?? [])
  const [message, setMessage] = useState<string>()
  const name = id === 'openai' ? 'OpenAI' : 'Anthropic'
  const source = s.keys[name]
  const model = id === 'openai' ? s.openAIModel : s.model
  const setModel = (m: string) => update(id === 'openai' ? { openAIModel: m } : { model: m })
  const sourceLabel = source === 'keychain' ? L('macOS Keychain（このホストのみ）') : source === 'env' ? L('環境変数 {0}', s.keyEnv[name]) : source === 'file' ? L('このホストのファイル（0600）') : L('未設定')
  const run = async (f: () => Promise<string>) => {
    try {
      setMessage(await f())
    } catch (e) {
      setMessage(errorText(e))
    }
  }
  return (
    <Section title={L('{0} API キー{1}', name, s.provider === id ? '（使用中）' : '')}>
      <Row label={L('保存先')}><span className="secondary">{sourceLabel}</span></Row>
      <Row label={L('新しい API キー')}>
        <input type="password" className="field" value={key} onChange={(e) => setKey(e.target.value)} autoComplete="off" spellCheck={false} aria-label={L('新しい {0} API キー', name)} />
      </Row>
      <Row>
        <div className="row gap8 wrap">
          <Button disabled={!key} onClick={() => void run(async () => {
            const r = await api<{ message: string }>('/api/settings/key', { provider: id, key })
            setKey('')
            await reload()
            refreshChats(true)
            return r.message
          })}>{L('キーを保存')}</Button>
          <Button kind="destructive" disabled={source !== 'keychain' && source !== 'file'} onClick={() => void run(async () => {
            const r = await api<{ message: string }>('/api/settings/key/delete', { provider: id })
            await reload()
            refreshChats()
            return r.message
          })}>{L('キーを削除')}</Button>
          <Button disabled={!source} onClick={() => void run(async () => {
            const r = await api<{ models: string[] }>('/api/settings/models', { provider: id })
            setModels(r.models)
            return L('{0} 件のモデルを取得しました。', r.models.length)
          })}>{L('モデル一覧を取得')}</Button>
        </div>
      </Row>
      <Row label={L('モデル')}><TextField value={model} onChange={setModel} /></Row>
      {models.length > 0 && (
        <Row label={L('一覧から選択')}><Select value={model} onChange={setModel} options={[...new Set([model, ...models])].map((m) => ({ value: m, label: m }))} /></Row>
      )}
      {message && <Row><span className="caption selectable">{message}</span></Row>}
    </Section>
  )
}

/** RPC endpoints of the environments this server has started (loopback only). */
function ConnectionSettings() {
  const [list, setList] = useState<{ key: string; project: string; name: string; state: { state: string; endpoint?: string; projectId?: string; readyFile?: string; message?: string } }[]>([])
  useEffect(() => {
    void api('/api/env/list').then(setList)
  }, [])
  return (
    <div className="col gap8 pad16 fill">
      <div className="callout secondary">{L('開いているプロジェクトの tsugumid への接続先です。127.0.0.1 にだけ公開しています。外部クライアント（別の UI・Agent）は同じ RPC 契約で接続でき、認証 token は control フォルダの server.ready（0600）にだけ置かれます。')}</div>
      <div className="list bordered flex1">
        {list.map((e) => (
          <div key={e.key} className="list-row col-start gap4">
            <div className="row gap6 center">
              <span style={{ color: e.state.state === 'ready' ? 'var(--green)' : 'var(--secondary)' }}>●</span>
              <b>{e.name}</b>
              <span className="secondary">{e.state.state === 'ready' ? L('接続中') : e.state.state}</span>
            </div>
            {e.state.state === 'ready' && (
              <>
                <div className="row gap6 center"><span className="caption mono selectable">{e.state.endpoint}</span><Button small onClick={() => void copyText(e.state.endpoint!)}>コピー</Button></div>
                <div className="row gap6 center"><span className="caption mono ellipsis selectable">{e.state.readyFile}</span><Button small onClick={() => void copyText(e.state.readyFile!)}>パスをコピー</Button></div>
                <div className="caption2 secondary">project_id {e.state.projectId}</div>
              </>
            )}
          </div>
        ))}
        {!list.length && <Empty small title={L('開いているプロジェクトはありません')} icon="network.slash" />}
      </div>
    </div>
  )
}
