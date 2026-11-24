// The Card database and the project's settings panes.
import { splitCardNotes } from '../model/documents'
import { useEffect, useState } from 'react'
import { app } from '../model/app'
import { nixPathLockStatus } from '../model/nixPath'
import { useObserve } from '../model/observable'
import { FLOW_LABELS, FlowDirection } from '../model/prefs'
import { definitionIcon, definitionKind, errorText, formatBytes, key, KIND_LABEL, path, ProjectSession } from '../model/session'
import { arr, bool, clone, get, isObj, J, JNum, numText, obj, parse, serialize, str } from '../../shared/json'
import { R, s } from '../wire/rpc'
import { DEFINITION_DRAG } from './actions'
import { Button, Confirm, Empty, Form, HSplit, InfoButton, MenuButton, Row, Section, Select, Spinner, TextField, Toggle, Value } from './controls'
import { CodeEditor } from './editor'
import { Icon } from './icons'
import { L } from '../i18n'
import { TrustedFoldersSettings } from './trustedFolders'
import { RevisionEdit, RevisionEditNotice } from './revisionEdit'
import { CardSecretsSettings } from './cardSecrets'
import { newCardItems, TemplatePicker } from './cardTemplates'

// ---- Card database -----------------------------------------------------------------

/** Every definition folder of the project (one per Card, and those without
 *  a Card): search, edit its JSON, place an unplaced one on the canvas. */
export function CardDatabaseView({ session, definition }: { session: ProjectSession; definition?: string }) {
  useObserve(session)
  const [search, setSearch] = useState('')
  const [showTemplates, setShowTemplates] = useState(false)
  const at = { x: 40, y: 40 }
  const [selected, setSelected] = useState<string | undefined>(() => (definition && session.definitions.has(definition) ? definition : undefined))
  const [cardText, setCardText] = useState('')
  const [metaText, setMetaText] = useState('')
  const [base, setBase] = useState({ card: '', meta: '' })
  const [revisionEdit] = useState(() => new RevisionEdit())
  useObserve(revisionEdit)
  const [parseError, setParseError] = useState<string>()
  const [message, setMessage] = useState<string>()

  const load = (id = selected) => {
    if (!id) return
    const c = session.definitions.get(id)
    const mt = session.definitionMeta.get(id)
    const ct = c !== undefined ? serialize(c) : '{}'
    const mtt = splitCardNotes(mt).readme ?? ''
    setCardText(ct)
    setMetaText(mtt)
    setBase({ card: ct, meta: mtt })
    revisionEdit.reload(session.rev)
    setParseError(undefined)
    setMessage(undefined)
  }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  useEffect(() => load(), [selected])
  useEffect(() => {
    if (definition && session.definitions.has(definition)) setSelected(definition)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [definition])

  const q = search.toLowerCase()
  const ids = [...session.definitions.keys()].sort().filter((id) =>
    !q || session.displayName(key([id])).toLowerCase().includes(q) || id.toLowerCase().includes(q) || (str(get(session.definitionMeta.get(id), 'description')) ?? '').toLowerCase().includes(q))
  const unplaced = new Set(session.unplacedDefinitions)

  const apply = async (id: string, overwrite = false) => {
    let card: J
    const readme: J | undefined = metaText.trim() ? metaText : undefined
    try {
      card = parse(cardText)
    } catch (e) {
      setParseError(L('JSON として読めません: {0}', errorText(e)))
      return
    }
    setParseError(undefined)
    if (!await revisionEdit.replaceDocuments(session, [
      [`definitions/${id}/_card.json`, card], [`definitions/${id}/README.md`, readme],
    ], overwrite)) return
    setBase({ card: cardText, meta: metaText })
    setMessage(L('適用しました（revision {0}）。', session.rev))
  }

  const detail = (() => {
    const id = selected
    if (!id) return <Empty key="none" title={L('定義を選んでください')} icon="square.stack" description={L('ドラッグして Canvas に配置できます。')} />
    const unapplied = cardText !== base.card || metaText !== base.meta
    const isUnplaced = unplaced.has(id)
    return (
      <div key="detail" className="col fill pad12 gap8">
        <div className="row center">
          <span className="title2 selectable">{session.displayName(key([id]))}</span>
          <span className="spacer" />
          {unapplied && <span className="label" style={{ color: 'var(--orange)' }}><Icon name="pencil.circle.fill" />{L('未適用の変更')}</span>}
        </div>
        <div className="callout secondary">{L('Card「{0}」の定義（definitions/{0}/）です。', id)}</div>
        <div className="headline">{L('_card.json（設定・入力）')}</div>
        <CodeEditor text={cardText} onChange={setCardText} language="json" documentId={`db:${id}/_card.json`} style={{ flex: 2, minHeight: 180 }} />
        <div className="headline">{L('README.md（説明と内容、任意）')}</div>
        <CodeEditor text={metaText} onChange={setMetaText} language="md" documentId={`db:${id}/README.md`} style={{ flex: 1, minHeight: 80, maxHeight: 140 }} />
        <RevisionEditNotice edit={revisionEdit} text={() => `_card.json\n${cardText}\nREADME.md\n${metaText}`}
          overwrite={() => void apply(id, true)} reload={() => load(id)} />
        {parseError && <span className="label selectable" style={{ color: 'var(--red)' }}><Icon name="xmark.octagon" />{parseError}</span>}
        {message && <div className="callout secondary selectable">{message}</div>}
        <div className="row gap8 center">
          <Button disabled={!session.isEditable || !isUnplaced} onClick={() => {
            session.placeDefinition(id, { x: 40, y: 40 })
            setMessage(undefined)
          }}>{L('Canvas に配置')}</Button>
          <Button disabled={isUnplaced} help={L('Card の設定画面で開きます')}
            onClick={() => app.openWindow(session, { kind: 'card', card: key([id]), category: 'source' })}>{L('ソースを開く…')}</Button>
          <Button kind="destructive" disabled={!isUnplaced || !session.isEditable}
            help={isUnplaced ? L('定義とソースを削除') : L('Card を削除すると定義も削除されます')}
            onClick={() => { session.removeDefinition(id); setSelected(undefined) }}>{L('削除')}</Button>
          <span className="spacer" />
          <Button disabled={revisionEdit.applying} onClick={() => load()}>{L('再読込')}</Button>
          <Button kind="prominent" disabled={!unapplied || revisionEdit.applying || !session.isEditable} onClick={() => void apply(id)}>{L('適用')}</Button>
        </div>
      </div>
    )
  })()

  return (
    <HSplit sizes={[280, null]} min={[240, 420]}>
      {[
        <div key="list" className="col fill">
          <div className="row gap6 center pad6">
            <TextField value={search} onChange={setSearch} placeholder={L('検索')} style={{ flex: 1 }} />
            <MenuButton icon="plus" help={L('Card を追加')} disabled={!session.isEditable} items={() => newCardItems((t) => {
              void session.addCard(t, [], at).then((k) => setSelected(path(k)[0])).catch((e) => session.cardEditFailed(e))
            }, () => setShowTemplates(true))} />
            {showTemplates && <TemplatePicker session={session} placement={{ scope: [], at }} onClose={() => setShowTemplates(false)} />}
          </div>
          <div className="list flex1" role="listbox" aria-label={L('定義')}>
            {ids.map((id) => {
              const kind = definitionKind(session.definitions.get(id))
              return (
                <div key={id} role="option" aria-selected={id === selected} className={`list-row clickable def-row${id === selected ? ' selected' : ''}`}
                  draggable onDragStart={(e) => { e.dataTransfer.setData('text/plain', DEFINITION_DRAG + id); e.dataTransfer.effectAllowed = 'copy' }}
                  onClick={() => { if (!revisionEdit.applying) setSelected(id) }}>
                  <Icon name={definitionIcon(session.definitions.get(id))} className="secondary" />
                  <div className="col min0">
                    <span className="ellipsis">{session.displayName(key([id]))}</span>
                    <span className="caption secondary">{KIND_LABEL[kind] + (unplaced.has(id) ? L(' · 未配置') : '')}</span>
                  </div>
                </div>
              )
            })}
          </div>
        </div>,
        detail,
      ]}
    </HSplit>
  )
}

// ---- Project settings --------------------------------------------------------------

export type ProjectSettingsPane = 'general' | 'canvas' | 'history' | 'cache' | 'secrets'
export const PROJECT_SETTINGS_PANES: { id: ProjectSettingsPane; title: string; icon: string }[] = [
  { id: 'general', title: L('名前と実行環境'), icon: 'folder.badge.gearshape' },
  { id: 'canvas', title: 'Canvas', icon: 'rectangle.3.group' },
  { id: 'history', title: L('実行履歴'), icon: 'clock.arrow.circlepath' },
  { id: 'cache', title: L('ビルドキャッシュ'), icon: 'shippingbox' },
  { id: 'secrets', title: L('Card シークレット'), icon: 'key' },
]

/** One of a project's settings panes (also shown in the settings window). */
export function ProjectSettingsContent({ pane, session }: { pane: ProjectSettingsPane; session: ProjectSession }) {
  useObserve(session)
  switch (pane) {
    case 'general': return <GeneralProjectSettings key={session.key} session={session} />
    case 'canvas': return <CanvasProjectSettings session={session} />
    case 'history': return <RunHistorySettings key={session.key} session={session} />
    case 'cache': return <CacheSettings key={session.key} session={session} />
    case 'secrets': return <CardSecretsSettings key={session.key} session={session} />
  }
}

/** The project's settings: its panes beside a sidebar. */
export function WorkflowSettingsView({ session, initial }: { session: ProjectSession; initial?: ProjectSettingsPane }) {
  const [pane, setPane] = useState<ProjectSettingsPane>(initial ?? 'general')
  return (
    <div className="split-view">
      <nav className="split-nav" aria-label={L('設定')}>
        {PROJECT_SETTINGS_PANES.map((p) => (
          <button key={p.id} type="button" className={`nav-row${p.id === pane ? ' selected' : ''}`} aria-current={p.id === pane} onClick={() => setPane(p.id)}>
            <Icon name={p.icon} />{p.title}
          </button>
        ))}
      </nav>
      <div className="split-detail"><ProjectSettingsContent pane={pane} session={session} /></div>
    </div>
  )
}

function NixPathSettings({ session }: { session: ProjectSession }) {
  const [selected, setSelected] = useState('')
  const projects = [...session.externals.keys()].sort()
  const prefix = projects.includes(selected) ? selected : ''
  return (
    <Section title={L('Nix 検索パス')} info={L('Card は <名前/パス> でソースを参照します。「更新」で追加または URL が変わったソースを固定し、削除したソースの固定を外します。')}>
      {projects.length > 0 && <Row label={L('対象プロジェクト')}>
        <Select value={prefix} onChange={setSelected} options={[
          { value: '', label: L('ルートプロジェクト') },
          ...projects.map(value => ({ value, label: value.slice(0, -1) })),
        ]} />
      </Row>}
      <NixPathRows key={prefix} session={session} prefix={prefix} />
    </Section>
  )
}

function NixPathRows({ session, prefix }: { session: ProjectSession; prefix: string }) {
  const file = prefix + 'tsugumi.json'
  const project = prefix ? session.externals.get(prefix)?.project : session.project
  const entries = arr(get(project, 'nixPath')) ?? []
  const lock = session.nixPathLock(prefix)
  const [draft, setDraft] = useState<{ original?: J; editing?: string; name: string; url: string }>()
  const [working, setWorking] = useState(false)
  const [message, setMessage] = useState<string>()
  const [revisionEdit] = useState(() => new RevisionEdit())
  const [pending, setPending] = useState<J[]>()
  useObserve(revisionEdit)

  const disabled = working || revisionEdit.applying || !session.isEditable
  const close = () => {
    setDraft(undefined)
    setPending(undefined)
    revisionEdit.reload(session.rev)
  }
  const start = (entry?: J) => {
    const name = str(get(entry, 'name'))
    const next = { original: clone(get(project, 'nixPath')), editing: name, name: name ?? '', url: str(get(entry, 'url')) ?? '' }
    setDraft(next)
    setPending(undefined)
    revisionEdit.reload(session.rev)
  }
  const save = async (list: J[], original: J | undefined) => {
    setPending(list)
    if (await revisionEdit.apply(session, x => x.appendEdits([x.fieldEdit(file, ['nixPath'], original, list)]))) close()
  }
  const update = async (name?: string) => {
    setWorking(true)
    setMessage(undefined)
    try { await session.updateNixPathLock(prefix, name) }
    catch (e) { setMessage(errorText(e)) }
    finally { setWorking(false) }
  }
  const saveDraft = () => {
    if (!draft) return
    const entry = obj([['name', draft.name], ['url', draft.url.trim()]])
    const original = arr(draft.original) ?? []
    const next = draft.editing === undefined ? [...original, entry]
      : original.map(row => str(get(row, 'name')) === draft.editing ? entry : row)
    void save(next, draft.original)
  }

  return <div data-nix-path>
    {entries.map(entry => {
      const name = str(get(entry, 'name')) ?? ''
      const url = str(get(entry, 'url')) ?? ''
      const status = nixPathLockStatus(lock, name, url)
      const statusText = status.kind === 'unlocked' ? L('未ロック') :
        status.kind === 'urlChanged' ? L('URL が変更されました') :
        status.pin
      return (
        <Row key={name} label={name}>
          <div className="col"><code className="secondary">{url}</code><span className="caption secondary">{statusText}</span></div>
          <Button disabled={disabled} onClick={() => start(entry)}>{L('編集')}</Button>
          <Button disabled={disabled} onClick={() => void update(name)}>{L('更新')}</Button>
          <Button kind="destructive" disabled={disabled} onClick={() => {
            const original = clone(get(project, 'nixPath'))
            close()
            void save(entries.filter(row => str(get(row, 'name')) !== name), original)
          }}>{L('削除')}</Button>
        </Row>
      )
    })}
    {draft && <>
      <Row label={L('名前')}>
        <TextField value={draft.name} onChange={name => setDraft({ ...draft, name })} mono disabled={disabled} />
      </Row>
      <Row label="URL">
        <TextField value={draft.url} onChange={url => setDraft({ ...draft, url })} placeholder="github:owner/repo" mono disabled={disabled} />
      </Row>
      <Row>
        <Button disabled={disabled || !/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(draft.name) || !draft.url.trim() ||
          entries.some(row => str(get(row, 'name')) === draft.name && draft.name !== draft.editing)}
          onClick={saveDraft}>{draft.editing === undefined ? L('追加') : L('保存')}</Button>
        <Button disabled={disabled} onClick={close}>{L('キャンセル')}</Button>
      </Row>
    </>}
    <Row>
      {!draft && <Button disabled={disabled} onClick={() => start()}>{L('追加')}</Button>}
      <Button disabled={disabled} onClick={() => void update()}>{L('すべて更新')}</Button>
      {working && <Spinner small />}
    </Row>
    <RevisionEditNotice edit={revisionEdit} text={() => serialize(pending ?? [])}
      overwrite={() => { if (pending) void save(pending, get(revisionEdit.jsonBase(file, undefined, true), 'nixPath')) }} reload={close} />
    {message && <Row><span className="callout selectable" style={{ color: 'var(--red)' }}>{message}</span></Row>}
  </div>
}

const recordingKeys = ['recordLogs', 'recordIO'] as const
function projectRecording(project: J) {
  return { recordLogs: bool(get(project, 'recordLogs'))!, recordIO: bool(get(project, 'recordIO'))! }
}
const limitFields = [
  ['logLimitBytes', L('呼び出しごとのログ上限（バイト）'), 1073741824n],
  ['outputLimitBytes', L('出力の保存上限（バイト）'), 18446744073709551615n],
  ['outputLimitEntries', L('出力の保存件数上限'), 18446744073709551615n],
] as const
type LimitKey = typeof limitFields[number][0]
const projectLimits = (project: J) => Object.fromEntries(
  limitFields.map(([key]) => [key, numText(get(project, key)) ?? ''])) as Record<LimitKey, string>

function GeneralProjectSettings({ session }: { session: ProjectSession }) {
  const [selectedProject, setSelectedProject] = useState(() => clone(session.project))
  const [name, setName] = useState(str(get(session.project, 'name')) ?? '')
  const savedPaths = (arr(get(session.project, 'trackedPaths')) ?? []).map(x => str(x) ?? '').join('\n')
  const [trackedPaths, setTrackedPaths] = useState(savedPaths)
  const excludeText = (project: J) => (arr(get(project, 'agentExclude')) ?? []).map(x => str(x) ?? '').join('\n')
  const [agentExclude, setAgentExclude] = useState(() => excludeText(session.project))
  const [recording, setRecording] = useState(() => projectRecording(session.project))
  const [limits, setLimits] = useState(() => projectLimits(session.project))
  const savedRecording = projectRecording(selectedProject)
  const changedRecording = recordingKeys.filter(k => recording[k] !== savedRecording[k])
  const savedLimits = projectLimits(selectedProject)
  const changedLimits = limitFields.filter(([key]) => limits[key] !== savedLimits[key])
  const validLimits = limitFields.every(([key, , max]) => /^\d+$/.test(limits[key]) && BigInt(limits[key]) <= max)
  const [message, setMessage] = useState<string>()
  const [revisionEdit] = useState(() => { const e = new RevisionEdit(); e.reload(session.rev); return e })
  useObserve(revisionEdit)
  const initialPaths = (arr(get(selectedProject, 'trackedPaths')) ?? []).map(x => str(x) ?? '').join('\n')
  const changed = name !== (str(get(selectedProject, 'name')) ?? '') || trackedPaths !== initialPaths || agentExclude !== excludeText(selectedProject) || changedRecording.length > 0 || changedLimits.length > 0
  const load = () => {
    setSelectedProject(clone(session.project))
    setName(str(get(session.project, 'name')) ?? '')
    setTrackedPaths(savedPaths)
    setAgentExclude(excludeText(session.project))
    setRecording(projectRecording(session.project))
    setLimits(projectLimits(session.project))
    revisionEdit.reload(session.rev)
  }
  const save = async (overwrite = false) => {
    if (!validLimits) return
    const paths = trackedPaths.split(/\r?\n/).filter(x => x.length > 0)
    const before = revisionEdit.jsonBase('tsugumi.json', selectedProject, overwrite)
    const after = clone(before)
    if (!isObj(after)) return
    if (name !== (str(get(selectedProject, 'name')) ?? '')) after.set('name', name)
    if (trackedPaths !== initialPaths) after.set('trackedPaths', paths)
    if (agentExclude !== excludeText(selectedProject)) after.set('agentExclude', agentExclude.split(/\r?\n/).filter(x => x.length > 0))
    for (const k of changedRecording) after.set(k, recording[k])
    for (const [key] of changedLimits) after.set(key, new JNum(String(BigInt(limits[key]))))
    if (!await revisionEdit.apply(session, x => x.appendEdits([x.jsonEdit('tsugumi.json', before, after)]))) return
    setSelectedProject(after)
    const saved = projectRecording(after)
    setRecording(current => {
      const next = { ...current }
      for (const k of recordingKeys) if (current[k] === recording[k]) next[k] = saved[k]
      return next
    })
    const savedLimits = projectLimits(after)
    setLimits(current => {
      const next = { ...current }
      for (const [key] of limitFields) if (current[key] === limits[key]) next[key] = savedLimits[key]
      return next
    })
    setTrackedPaths(current => current === trackedPaths ? paths.join('\n') : current)
    setMessage(L('保存しました。'))
  }
  return (
    <div className="col fill" onKeyDown={(e) => {
      // Return saves (the default action).
      if (e.key === 'Enter' && !e.nativeEvent.isComposing && changed && session.isEditable && (e.target as HTMLElement).tagName === 'INPUT' && !(e.target as HTMLElement).closest('[data-nix-path]')) void save()
    }}>
      <div className="scroll flex1">
        <Form>
          <Section>
            <Row label={L('プロジェクト名')}><TextField value={name} onChange={setName} style={{ width: 260 }} /></Row>
          </Section>
          <Section info={L('Tsugumi のプロジェクトの版に追加で残すファイルやフォルダを、プロジェクトからの相対パスで 1 行に 1 つ指定します。Git の追跡には影響しません。ワイルドカード、.tsugumi/、.git/、シンボリックリンクは指定できません。')}>
            <Row label={L('Tsugumi の版に含める追加パス')}><TextField value={trackedPaths} onChange={setTrackedPaths} multiline rows={5} style={{ width: 320 }} /></Row>
          </Section>
          <Section info={L('Agent が参照できないパスを 1 行に 1 つ指定します。プロジェクトからの相対パス、または絶対パスで、ファイルと配下の全体を対象にします。人の画面には表示されます。')}>
            <Row label={L('Agent から除外するパス')}><TextField value={agentExclude} onChange={setAgentExclude} multiline rows={5} style={{ width: 320 }} /></Row>
          </Section>
          <Section info={L('信頼すると、このプロジェクトの Skill が自動で適用され、組み込み Skill を上書きできます。信頼しなくても、プロジェクトを開いて読むことや、名前を指定して Skill を使うことはできます。')}>
            <Row label={L('このプロジェクトを信頼する')}><Toggle checked={session.projectTrust === 1} disabled={!!session.trustedFolder} onChange={(trusted) => void session.setProjectTrust(trusted)} /></Row>
          </Section>
          <Section title={L('記録の既定値')}>
            <Row label={L('標準出力と標準エラーを記録')}><Toggle checked={recording.recordLogs} onChange={recordLogs => setRecording({ ...recording, recordLogs })} /></Row>
            <Row label={L('入出力の値を記録')}><Toggle checked={recording.recordIO} onChange={recordIO => setRecording({ ...recording, recordIO })} /></Row>
            {limitFields.map(([key, label]) => <Row key={key} label={label}
              help={key === 'logLimitBytes' ? L('0〜1073741824。0 の場合もログは実行中に表示されます。') : undefined}>
              <TextField value={limits[key]} onChange={value => setLimits(current => ({ ...current, [key]: value }))} mono />
            </Row>)}
          </Section>
          <TrustedFoldersSettings />
          <NixPathSettings session={session} />
          <Section>
            <Value label={L('場所')} value={session.dir} />
            <Value label={L('ファイル形式')} value={numText(get(session.project, 'formatVersion')) ?? ''} />
          </Section>
        </Form>
      </div>
      <RevisionEditNotice edit={revisionEdit} text={() => [`name: ${name}`, `trackedPaths:\n${trackedPaths}`, `agentExclude:\n${agentExclude}`,
        ...recordingKeys.map(k => `${k}: ${recording[k]}`), ...limitFields.map(([key]) => `${key}: ${limits[key]}`)].join('\n')}
        overwrite={() => void save(true)} reload={load} />
      <div className="detail-foot">
        {message && <span className="callout secondary selectable">{message}</span>}
        <span className="spacer" />
        <Button kind="prominent" disabled={!changed || !validLimits || revisionEdit.applying || !session.isEditable} onClick={() => void save()}>{L('保存')}</Button>
      </div>
    </div>
  )
}

/** How the project's Cards are laid out (layout.bin, applied at once). */
function CanvasProjectSettings({ session }: { session: ProjectSession }) {
  return (
    <div className="scroll fill">
      <Form>
        <Section footer={<span className="label" style={{ color: 'var(--orange)' }}><Icon name="exclamationmark.triangle" />{L('変えると Card の配置と線が合わなくなることがあります（「整列」で並べ直せます）。')}</span>}>
          <Row label={L('流れの向き')} help={L('このプロジェクトの Card を並べた向きです（layout.bin に保存され、すぐ反映されます）。線の引き方と「整列」がこの向きに合わせます。')}>
            <Select<FlowDirection> value={session.flow} onChange={(f) => session.setFlow(f)}
              options={(Object.keys(FLOW_LABELS) as FlowDirection[]).map((f) => ({ value: f, label: FLOW_LABELS[f] }))} />
          </Row>
        </Section>
      </Form>
    </div>
  )
}

/** A date as the value of a datetime-local field (local time). */
const localInput = (d: Date) => {
  const p = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}`
}

function RunHistorySettings({ session }: { session: ProjectSession }) {
  const [scope, setScope] = useState<'before' | 'all'>('before')
  const [before, setBefore] = useState(() => localInput(new Date(Date.now() - 30 * 24 * 3600 * 1000)))
  const [working, setWorking] = useState(false)
  const [message, setMessage] = useState<string>()
  const [failed, setFailed] = useState(false)
  const [confirm, setConfirm] = useState(false)
  const del = async () => {
    setWorking(true)
    setMessage(undefined)
    try {
      const r = await session.deleteRunHistory(scope === 'all' ? null : new Date(before))
      setFailed(false)
      setMessage(L('{0} 件の実行を削除し、{1} を解放しました。', r.deletedRuns ?? 0n, formatBytes(r.freedBytes ?? 0n)))
    } catch (e) {
      setFailed(true)
      setMessage(L('削除できませんでした: {0}', errorText(e)))
    }
    setWorking(false)
  }
  return (
    <div className="col fill">
      <div className="scroll flex1">
        <Form>
          <Section title={L('削除')} info={L('実行記録、保存した値、ログ、実行が所有する成果物（.tsugumi/artifacts/<run>）と scratch を削除します。実行中は削除できません。')}>
            <Row label={L('対象')}>
              <Select<'before' | 'all'> value={scope} onChange={setScope}
                options={[{ value: 'before', label: L('指定した日時より前に終わった実行') }, { value: 'all', label: L('すべての実行') }]} />
            </Row>
            {scope === 'before' && (
              <Row label={L('日時')}>
                <input type="datetime-local" className="field" value={before} onChange={(e) => setBefore(e.target.value)} />
              </Row>
            )}
          </Section>
        </Form>
      </div>
      <div className="detail-foot">
        {message && <span className="callout selectable" style={{ color: failed ? 'var(--red)' : 'var(--secondary)' }}>{message}</span>}
        <span className="spacer" />
        {working && <Spinner small />}
        <Button kind="destructive" disabled={working || session.runActive || !session.connected || (scope === 'before' && !before)} onClick={() => setConfirm(true)}>{L('削除…')}</Button>
      </div>
      {confirm && <Confirm title={scope === 'all' ? L('すべての実行履歴を削除しますか？') : L('この日時より前に終わった実行を削除しますか？')} confirm={L('削除')}
        onConfirm={() => void del()} onCancel={() => setConfirm(false)} />}
    </div>
  )
}

/** The same row controls and selected total used by the cache pane. */
export function ScratchRows({ candidates, selected, onSelect, disabled = false, onDelete }: { candidates: R.RunScratchUsageT[]; selected: Set<string>; onSelect: (path: string, on: boolean) => void; disabled?: boolean; onDelete: (paths: string[]) => void }) {
  const selection = candidates.filter(p => selected.has(s(p.path)))
  return <>
    {candidates.map(p => <div className="row gap8" key={s(p.path)}>
      <Toggle kind="checkbox" label={s(p.path)} checked={selected.has(s(p.path))} onChange={on => onSelect(s(p.path), on)} />
      <span>{p.sizeUnknown ? L('不明') : formatBytes(p.bytes)}</span>
    </div>)}
    <Value label={L('削除対象の合計')} value={formatBytes(selection.filter(p => !p.sizeUnknown).reduce((n, p) => n + p.bytes, 0n))} />
    {selection.some(p => p.sizeUnknown) && <div className="caption secondary">{L('合計は容量不明のフォルダを除きます。')}</div>}
    <Button kind="destructive" disabled={disabled || !selection.length} onClick={() => onDelete(selection.map(p => s(p.path)))}>{L('選択した scratch を削除…')}</Button>
  </>
}

function CacheSettings({ session }: { session: ProjectSession }) {
  useObserve(session)
  const [selectedScratch, setSelectedScratch] = useState<Set<string>>(() => new Set())
  const [scratchSelection, setScratchSelection] = useState<string[]>()
  const [scratchResult, setScratchResult] = useState<R.RunScratchDeleteResultT>()
  useEffect(() => { void session.refreshCacheUsage() }, [session])
  const scratch = session.cacheUsage?.projects.find(p => s(p.project) === s(session.cacheUsage?.currentProject))?.runScratch ?? []
  const candidates = scratch.filter(p => s(p.path) !== `runs/${s(session.cacheUsage?.protectedRun)}`)
  const deleteScratch = async () => {
    setWorking(true)
    setMessage(undefined)
    const paths = scratchSelection ?? []
    setScratchSelection(undefined)
    try { setScratchResult(await session.deleteRunScratch(paths)); setSelectedScratch(new Set()) }
    catch (e) { setMessage(L('削除できませんでした: {0}', errorText(e))) }
    setWorking(false)
  }
  const [clearTarget, setClearTarget] = useState(session)
  const [result, setResult] = useState<R.ProjectCacheClearResultT>()
  const [working, setWorking] = useState(false)
  const [message, setMessage] = useState<string>()
  const [confirm, setConfirm] = useState(false)
  const clear = async () => {
    setWorking(true)
    setMessage(undefined)
    try {
      setResult(await clearTarget.clearBuildCache())
      await session.refreshCacheUsage()
    } catch (e) {
      setMessage(L('削除できませんでした: {0}', errorText(e)))
    }
    setWorking(false)
  }
  return (
    <div className="col fill pad16 gap12">
      <div className="row"><InfoButton>{L('このプロジェクトの Card のビルド結果の登録（Nix の GC root）と、作業用のビルドファイルを削除します。共有 Nix store にある実体は消さず、アプリ設定の「共有 store を掃除」で登録のないものとして削除されます。文書・実行履歴・保持した成果物は削除しません。次回の実行では評価し直し、store に残っていればビルドせずに使います。')} {L('実行中は削除できません。')}</InfoButton></div>
      <div className="groupbox col gap8" style={{ overflow: 'auto' }}>
        <div className="headline">{L('キャッシュ使用量')}</div>
        <Value label={L('合計（共有パスは一度だけ計上）')} value={session.cacheUsage?.buildBytes != null ? formatBytes(session.cacheUsage.buildBytes) : L('計測中または取得できません')} />
        {session.cacheUsage?.storeAvailableBytes != null && <Value label={L('store ディスクの空き')} value={formatBytes(session.cacheUsage.storeAvailableBytes)} />}
        <div className="row"><InfoButton>{L('ビルドキャッシュは NAR サイズの概算、scratch はリンクをたどらずに合計したファイルサイズです。共有する依存関係は各プロジェクトにも含まれるため、ビルドの内訳の和と合計は異なります。設定を開いたときに計測します。')}</InfoButton></div>
        {session.cacheUsage?.measuredAt != null && <Value label={L('計測日時')} value={new Date(Number(session.cacheUsage.measuredAt)).toLocaleString()} />}
        {session.cacheUsage?.error && <div className="caption selectable">{s(session.cacheUsage.error)}</div>}
        {session.cacheUsage?.projects.map(p => {
          const target = s(p.project) === s(session.cacheUsage?.currentProject) ? session : app.sessions.get(s(p.project))
          return <div className="col gap4" key={s(p.project)} style={{ borderTop: '1px solid var(--separator)', paddingTop: 8 }}>
            <div className="row gap8"><span className="headline flex1">{s(p.name)}</span><Button kind="destructive" disabled={!target?.connected || working || !!target?.runActive}
              onClick={() => { if (target) { setClearTarget(target); setConfirm(true) } }}>{L('削除')}</Button></div>
            <Value label={L('ビルドキャッシュ')} value={p.buildBytes !== null ? formatBytes(p.buildBytes) : L('取得できません')} />
            <Value label={L('実行の scratch（合計）')} value={p.scratchBytes !== null ? formatBytes(p.scratchBytes) : L('取得できません')} />
            <Value label={L('root の最終使用')} value={p.lastUsed !== null ? new Date(Number(p.lastUsed)).toLocaleString() : L('不明')} />
            {!target && <div className="caption secondary">{L('削除するにはこのプロジェクトを開いてください。')}</div>}
            {p.error && <div className="caption selectable">{s(p.error)}</div>}
          </div>
        })}
      </div>
      <div className="row"><InfoButton>{L('過去の scratch は自動では削除しません。削除するフォルダと容量を確認してください。最新の実行の scratch は再実行のため残します。保持した記録・成果物・最後の入力は別に残ります。')}</InfoButton></div>
      <div className="groupbox col gap4">
        <div className="headline">{L('このプロジェクトの過去の scratch')}</div>
        <ScratchRows candidates={candidates} selected={selectedScratch} disabled={working || session.runActive || !session.connected} onDelete={setScratchSelection} onSelect={(path, on) => setSelectedScratch(before => {
          const next = new Set(before); if (on) next.add(path); else next.delete(path); return next
        })} />
        {scratchResult && <Value label={L('解放')} value={formatBytes(scratchResult.freedBytes)} />}
        {!!scratchResult?.retained.length && <div>{L('削除できず残ったフォルダ')}: {scratchResult.retained.map(s).join(', ')}</div>}
      </div>
      {scratchSelection && <Confirm title={L('選択した過去の scratch を削除しますか？')} confirm={L('削除')}
        onConfirm={() => void deleteScratch()} onCancel={() => setScratchSelection(undefined)} />}
      {result && (
        <div className="groupbox col gap4">
          <div className="headline">{L('結果')}</div>
          <Value label={L('削除')} value={L('{0} 件', result.deleted.length)} />
          <Value label={L('保持')} value={L('{0} 件', result.retained.length)} />
          {result.freedBytes !== null && <Value label={L('解放')} value={formatBytes(result.freedBytes)} />}
        </div>
      )}
      {message && <div className="selectable" style={{ color: 'var(--red)' }}>{message}</div>}
      <span className="flex1" />
      <div className="row end gap8">
        {working && <Spinner small />}
        <Button kind="destructive" disabled={working || session.runActive || !session.connected} onClick={() => { setClearTarget(session); setConfirm(true) }}>{L('削除')}</Button>
      </div>
      {confirm && <Confirm title={L('「{0}」のビルドキャッシュを削除しますか？', clearTarget.scopeName([]))} confirm={L('削除')} onConfirm={() => void clear()} onCancel={() => setConfirm(false)} />}
    </div>
  )
}
