import { agentAppearance } from '../model/agentAppearance'
// App-wide settings; the category on the left switches the content. While a
// project is open its own settings follow the app's (kept in the project).
import { useEffect, useState } from 'react'
import { api, onServerEvent } from '../api'
import { app } from '../model/app'
import { guide } from '../model/guide'
import { Skill } from '../model/chat'
import { useObserve } from '../model/observable'
import {
  bindingLabel, CANVAS_ONLY, EDITOR_THEME_LABELS, EditorTheme, eventBinding, FLOW_LABELS, FlowDirection, Motion, MOTION_LABELS, prefs,
  SEND_KEY_LABELS, SendKey, SHORTCUT_ACTIONS, SHORTCUT_TITLES, ShortcutAction, Theme, THEME_LABELS,
} from '../model/prefs'
import { errorText, ProjectSession } from '../model/session'
import { Button, copyText, Empty, Form, InfoButton, Row, Section, Segmented, Select, Stepper, TextField, Toggle } from './controls'
import { CodeEditor } from './editor'
import { Icon } from './icons'
import { PROJECT_SETTINGS_PANES, type ProjectSettingsPane, ProjectSettingsContent } from './projectViews'
import { FolderPicker } from './sidebar'
import { adoptLanguage, L, language, type Language } from '../i18n'
import { ChatGPTSettings } from './chatgpt'
import { TrustedFoldersSettings } from './trustedFolders'
import { ExecutionServicesSettings } from './executionServices'

type Category = 'general' | 'appearance' | 'editor' | 'shortcuts' | 'skills' | 'api' | 'connections' | 'executionServices'
const CATS: { id: Category; title: string; icon: string; storage: string }[] = [
  { id: 'general', title: L('一般'), icon: 'gearshape', storage: L('このブラウザ（localStorage）') },
  { id: 'appearance', title: L('外観'), icon: 'paintpalette', storage: L('このブラウザの外観設定と、このホストの hostd の Agent 表示設定') },
  { id: 'editor', title: L('エディター'), icon: 'text.cursor', storage: L('このブラウザ（localStorage）') },
  { id: 'shortcuts', title: L('ショートカット'), icon: 'keyboard', storage: L('このブラウザ（localStorage）') },
  { id: 'skills', title: 'Skills', icon: 'wand.and.stars', storage: L('このホスト（Tsugumi web サーバーの設定）') },
  { id: 'api', title: 'API', icon: 'key', storage: L('このホスト（API キーは macOS Keychain または hostd が保管）') },
  { id: 'connections', title: L('外部接続'), icon: 'network', storage: L('表示のみ（開いているプロジェクトの接続先）') },
  { id: 'executionServices', title: L('実行サービス'), icon: 'network', storage: L('このホスト（Tsugumi web サーバーの設定）') },
]

/** The settings of one project: kept in the project (or its environment). */
function paneStorage(p: ProjectSettingsPane, s: ProjectSession): string {
  const name = s.dir.split('/').pop() ?? ''
  switch (p) {
    case 'general': return L('このプロジェクト（{0}/tsugumi.json）', name)
    case 'canvas': return L('このプロジェクト（{0}/layout.json）', name)
    case 'history': return L('このプロジェクトの実行環境（{0}/.tsugumi/runs.sqlite）', name)
    case 'cache': return L('このプロジェクトの実行環境（このホスト）')
  }
}

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

export function ApplicationSettings() {
  useObserve(prefs, app)
  /** "<category>" for the app's panes, "project.<pane>" for the project's. */
  const [stored, setStored] = useState(() => load(STORE.category, 'general'))
  const [projectPath, setProjectPath] = useState(() => load(STORE.project, ''))
  const select = (v: string) => {
    setStored(v)
    store(STORE.category, v)
  }
  const projects = [...app.sessions.values()].sort((a, b) => a.scopeName([]).localeCompare(b.scopeName([])))
  const project = projects.find((s) => s.dir === projectPath) ?? projects[0]
  const pane = stored.startsWith('project.') && project ? PROJECT_SETTINGS_PANES.find((p) => 'project.' + p.id === stored) : undefined
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
            {PROJECT_SETTINGS_PANES.map((p) => (
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
          {pane && project ? <ProjectSettingsContent pane={pane.id} session={project} /> : (
            <>
              {category.id === 'general' && <GeneralSettings />}
              {category.id === 'appearance' && <AppearanceSettings />}
              {category.id === 'editor' && <EditorSettings />}
              {category.id === 'shortcuts' && <ShortcutSettings />}
              {category.id === 'skills' && <SkillSettings />}
              {category.id === 'api' && <APISettings />}
              {category.id === 'connections' && <ConnectionSettings />}
              {category.id === 'executionServices' && <ExecutionServicesSettings changed={() => {
                void Promise.allSettled(projects.map((p) => p.refreshExecutionServices()))
              }} />}
            </>
          )}
        </div>
      </div>
    </div>
  )
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
      <TrustedFoldersSettings />
      <VMResourceSettings />
      <RuntimeResetSettings />
    </Form>
  )
}

interface VMSize { cpus?: number; memoryGib?: number; storeGib?: number }
interface VMRuntime { vmState: string; vmDefaultResources?: VMSize; vmLimits?: VMSize; storeDiskGib?: number }
type VMKey = 'vmCpus' | 'vmMemoryGiB' | 'vmStoreGiB'

/** The shared VM's size, kept by hostd and used from the VM's next start. */
function VMResourceSettings() {
  const [runtime, setRuntime] = useState<VMRuntime>()
  const [settings, setSettings] = useState<Record<VMKey, number> & { revision: Record<string, number> }>()
  const [error, setError] = useState<string>()
  const [saving, setSaving] = useState(false)
  const load = async () => {
    setRuntime(await api<VMRuntime>('/api/runtime'))
    setSettings(await api('/api/settings'))
  }
  useEffect(() => {
    const refresh = () => void load().catch(e => setError(errorText(e)))
    refresh()
    return onServerEvent(e => { if (e.type === 'settingsChanged') refresh() })
  }, [])
  if (!runtime || runtime.vmState === 'unavailable' || !settings) return null
  const defaults = runtime.vmDefaultResources ?? {}, limits = runtime.vmLimits ?? {}
  const save = async (change: Partial<Record<VMKey, number>>) => {
    setSaving(true)
    try {
      await api('/api/settings', { revision: settings.revision, ...change })
      setError(undefined)
    } catch (e) { setError(errorText(e)) }
    finally {
      try { await load() } catch (e) { setError(errorText(e)) }
      setSaving(false)
    }
  }
  // 0 follows this machine's default; choosing the default value stores 0 again.
  const row = (key: VMKey, fallback: number, min: number, max: number, step: number, label: (n: number) => string) => {
    const value = settings[key] || fallback
    return <Row><Stepper label={label(value) + (settings[key] ? '' : L('（既定）'))} value={value} min={min} max={max} step={step}
      onChange={(v) => { if (!saving) void save({ [key]: v === fallback ? 0 : v }) }} /></Row>
  }
  const store = settings.vmStoreGiB || (defaults.storeGib ?? 64)
  const disk = runtime.storeDiskGib ?? 0
  return <Section title={L('VM のサイズ')} info={L('全プロジェクト共通の VM のサイズです。変更は次に VM を起動したときから使われます（実行中の VM は今のサイズのままです）。')}>
    {row('vmCpus', defaults.cpus ?? 2, 1, limits.cpus ?? 8, 1, (n) => L('CPU: {0} 個', n))}
    {row('vmMemoryGiB', defaults.memoryGib ?? 4, 2, limits.memoryGib ?? 16, 1, (n) => L('メモリ: {0} GiB', n))}
    {row('vmStoreGiB', defaults.storeGib ?? 64, 16, limits.storeGib ?? 2048, 16, (n) => L('Nix store: 最大 {0} GiB', n))}
    {disk > store && <Row><span className="caption secondary">{L('store のディスクは今 {0} GiB です。小さくするには「すべての実行環境を初期化…」で store を作り直します（それまでは今の大きさのままです）。', disk)}</span></Row>}
    <Row><Button disabled={saving || (!settings.vmCpus && !settings.vmMemoryGiB && !settings.vmStoreGiB)} onClick={() => void save({ vmCpus: 0, vmMemoryGiB: 0, vmStoreGiB: 0 })}>{L('既定に戻す')}</Button></Row>
    {error && <Row><span className="error">{error}</span></Row>}
  </Section>
}

/** The host supplies the confirmation from the policy it sends to the helper. */
function RuntimeResetSettings() {
  const [working, setWorking] = useState(false)
  const [message, setMessage] = useState('')
  const reset = async () => {
    setWorking(true)
    setMessage('')
    try {
      const state = await api<{ vmState: string; resetConfirmationJa: string; resetConfirmationEn: string }>('/api/runtime')
      if (state.vmState === 'unavailable') return
      const confirmation = language() === 'en' ? state.resetConfirmationEn : state.resetConfirmationJa
      if (!confirmation || !window.confirm(confirmation)) return
      await api('/api/runtime/reset', {})
      setMessage(L('初期化しました。次に開くときに実行環境を作り直します。'))
    } catch (error) { setMessage(errorText(error)) }
    finally { setWorking(false) }
  }
  const verify = async () => {
    if (!window.confirm(L('すべてのプロジェクトの実行環境を停止し、store の内容を検査してキャッシュから修復します。結果と対象パスは VM のログに残ります。'))) return
    setWorking(true); setMessage('')
    try {
      const result = await api<{ message: string }>('/api/runtime/verify-store', {})
      setMessage(L('Nix store の検査・修復が完了しました。') + ' ' + result.message)
    } catch (error) { setMessage(errorText(error)) }
    finally { setWorking(false) }
  }
  const [available, setAvailable] = useState(false)
  useEffect(() => { void api<{ vmState: string }>('/api/runtime').then((s) => setAvailable(s.vmState !== 'unavailable')).catch(() => {}) }, [])
  if (!available) return null
  return <Section title={L('実行環境のデータ')}>
    <Row><Button disabled={working} onClick={() => void verify()}>{L('共有 store を検査・修復…')}</Button></Row>
    <Row><Button disabled={working} onClick={() => void reset()}>{L('すべての実行環境を初期化…')}</Button></Row>
    {message && <Row><span className="caption selectable">{message}</span></Row>}
  </Section>
}

function AppearanceSettings() {
  useObserve(agentAppearance)
  useEffect(() => { void agentAppearance.reload() }, [])
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
          <Row label={L('Agent の作業をアニメーションで表示')} help={L('オフにすると、保存された Card と接続線をすぐに表示します。作業位置の表示と思考の要約、ソースのストリーミングは続きます。設定はこのホストの hostd に保存します。')}>
            <Toggle checked={agentAppearance.animateWork} onChange={(v) => void agentAppearance.setAnimateWork(v)} />
          </Row>
          {agentAppearance.error && <div className="error">{agentAppearance.error}</div>}
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
        <div className="row"><InfoButton>{L('サンプルは編集して試せます（保存されません）。Tab はインデント幅の空白を入れます。')}</InfoButton></div>
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
        <div className="table-head three"><span>{L('操作')}</span><span>{L('キー')}</span><span /></div>
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
      <div className="row"><InfoButton>{L('変更はメニューにすぐ反映されます。ほかに ⌘Z / ⇧⌘Z（取り消す・やり直す）、Delete（選択の削除）、⌘,（アプリ設定）が使えます。ブラウザが予約しているキー（⌘W・⌘T・⌘N・⌘Q など）は割り当てても届きません。')}</InfoButton></div>
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
      <div className="row"><InfoButton>{L('チャットで `$名前` として使う Skill のフォルダです。各フォルダの `<名前>/SKILL.md` または `<名前>.md` を読み込みます。プロジェクトの `skills/` も常に読み込みます。')}</InfoButton></div>
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
type KeySource = 'env' | 'keychain' | 'hostd' | null
interface APIState {
  provider: ProviderId
  model: string
  openAIModel: string
  chatGPTModel: string
  fixturePort: number
  models: Partial<Record<'anthropic' | 'openai' | 'chatgpt', string[]>>
  keys: { Anthropic: KeySource; OpenAI: KeySource }
  keyEnv: { Anthropic: string; OpenAI: string }
  allowUnscopedHelpers: boolean
  webSearch: boolean
}

/** Re-reads the connection of every open chat (after it changed here);
 *  `saved`: the user's own save of a key or sign-in (the guide may then ask to show the demo). */
function refreshChats(saved = false) {
  for (const s of app.sessions.values()) void app.chat(s).refreshProvider()
  // The guide is there exactly while the chosen provider has a key.
  void (saved ? guide.connectionSaved() : guide.refresh())
}

/** Model connections. API keys stay in this host’s platform secret storage; they are
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
      <Section info={<>{L('API キーと ChatGPT のサインインは、下でそれぞれ同時に設定しておけます。チャットのモデルの一覧からも切り替えられます。')}<br />{L('チャットは任意機能です。通常の操作はすべて UI から行えます。')}</>}>
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
      </Section>
      <KeySection id="anthropic" s={s} reload={reload} update={update} />
      <KeySection id="openai" s={s} reload={reload} update={update} />
      <ChatGPTSettings onConnected={() => refreshChats(true)} onChanged={() => refreshChats()} model={s.chatGPTModel} setModel={(m) => update({ chatGPTModel: m })} inUse={s.provider === 'chatgpt'} />
      <ProjectPermissionSettings />
      <Section info={L('オフにすると、各ヘルパーが読める Card と変更・実行できる Card を Agent が指定します。')} title={L('ヘルパー')}>
        <Row label={L('範囲を指定しないヘルパーを許可する')}>
          <Toggle checked={s.allowUnscopedHelpers} onChange={(v) => update({ allowUnscopedHelpers: v })} />
        </Row>
      </Section>
      <Section title={L('Web 検索')} info={s.provider === 'fixture'
          ? L('ローカル fixture には検索がないため、オンでも使えません。')
          : L('オンにすると、検索語が接続先のプロバイダを通して検索サービスへ送られます。Agent は未公開のデータを検索語に入れないよう指示されていますが、確実に防げるものではありません。検索語と出典はチャットに残ります。検索には利用料がかかることがあります。')}>
        <Row label={L('Agent が自分の判断で Web を検索する')}><Toggle checked={s.webSearch} onChange={(v) => update({ webSearch: v })} /></Row>
      </Section>
      <GuideSettings />
    </Form>
  )
}

/** The guide bird: whether it is shown, and the first start's offer again. */
function GuideSettings() {
  useObserve(guide)
  return (
    <Section info={L('ガイドは上の接続先とキーを使います。キーがあると質問に答え、ないあいだは決まったデモの案内だけをします（通信はしません）。質問に答えるだけではプロジェクトを変えません。')} title={L('ガイドのツグミ')}>
      <Row label={L('状態')}><span className="secondary">{guide.connected ? L('右下に表示中') : L('API キーが未設定のため、決まったデモの案内だけをしています')}</span></Row>
      {/* Pressed, it says what happens next and waits until it has happened. */}
      <Row><Button disabled={guide.offersDemo} onClick={() => void guide.resetIntroduction()}>{L('初回の案内（デモの勧誘）をもう一度表示')}</Button></Row>
      {guide.offersDemo && <Row><span className="label callout" style={{ color: 'var(--green)', whiteSpace: 'normal' }}><Icon name="checkmark.circle.fill" />{L('設定しました。Home に戻ると、ツグミが「何をしてみますか？」と尋ねます。')}</span></Row>}
    </Section>
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
  const sourceLabel = source === 'keychain' ? L('macOS Keychain（このデバイスのみ・ロック解除中に使用）') : source === 'hostd' ? L('hostd（このホストの所有者だけが読み書きできます）') : source === 'env' ? L('環境変数 {0}', s.keyEnv[name]) : L('未設定')
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
          <Button kind="destructive" disabled={source !== 'hostd' && source !== 'keychain'} onClick={() => void run(async () => {
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
      <div className="row"><InfoButton>{L('開いているプロジェクトの tsugumid への接続先です。127.0.0.1 にだけ公開しています。外部クライアント（別の UI・Agent）は同じ RPC 契約で接続でき、認証 token は control フォルダの server.ready（0600）にだけ置かれます。')}</InfoButton></div>
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
                <div className="row gap6 center"><span className="caption mono selectable">{e.state.endpoint}</span><Button small onClick={() => void copyText(e.state.endpoint!)}>{L('コピー')}</Button></div>
                <div className="row gap6 center"><span className="caption mono ellipsis selectable">{e.state.readyFile}</span><Button small onClick={() => void copyText(e.state.readyFile!)}>{L('パスをコピー')}</Button></div>
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

interface ProjectPermission { resource_id: string; title: string; path: string; access: string; allowed: boolean }
interface PermissionProject { project_id: string; title: string; path: string; permissions: ProjectPermission[] }
interface ProjectPermissionList { projects: PermissionProject[] }
const permissionLabel = (access: string) => ({ Read: L('内容の読み取り'), Write: L('内容の変更'), Run: L('Card の実行'), Shell: L('シェルコマンドの実行') })[access] ?? access

export function ProjectPermissionSettings() {
  const [projects, setProjects] = useState<PermissionProject[]>([])
  const [error, setError] = useState<string>()
  const [busy, setBusy] = useState(false)
  const request = async (project?: PermissionProject, permission?: ProjectPermission, allow?: boolean) => {
    setBusy(true)
    setError(undefined)
    try {
      const result = await api<ProjectPermissionList>(project ? '/api/project-permissions/set' : '/api/project-permissions', project && permission
        ? { project_id: project.project_id, resource_id: permission.resource_id, access: permission.access, allow } : {})
      setProjects(result.projects ?? [])
    } catch (e) { setError(errorText(e)) }
    finally { setBusy(false) }
  }
  useEffect(() => { void request() }, [])
  return <Section info={L('プロジェクトのすべての会話とヘルパーに適用され、取り消すまで有効です。')} title={L('Agent のプロジェクト権限')}>
    {projects.map(project => <div key={project.project_id}>
      <Row label={project.title || project.path || project.project_id}><span className="caption secondary selectable">{project.path}</span></Row>
      {project.permissions.map(permission => <Row key={`${permission.resource_id}:${permission.access}`}
        label={permission.resource_id === project.project_id ? permissionLabel(permission.access) : `${L('添付フォルダ')} · ${permission.title || permission.path} · ${permissionLabel(permission.access)}`}>
        <Toggle checked={permission.allowed} disabled={busy} onChange={allow => void request(project, permission, allow)} />
      </Row>)}
    </div>)}
    {error && <Row><span className="error">{error}</span></Row>}
    <Row><Button disabled={busy} onClick={() => void request()}>{L('更新')}</Button></Row>
  </Section>
}
