// Left column: workflow hierarchy, new Cards (each with a definition folder
// of its own, copied from the projects in externals) and the Cards shown in
// the current scope.
import { useEffect, useState } from 'react'
import { api, FolderListing } from '../api'
import { app } from '../model/app'
import { useObserve } from '../model/observable'
import { CardKey, cardState, definitionKind, errorText, key, KIND_ICON, KIND_LABEL, path, ProjectSession } from '../model/session'
import { canvasFor } from '../model/canvas'
import { Workspace, WorkflowTab } from '../model/workspace'
import { arr, get, str } from '../shared/json'
import { DEFINITION_DRAG, EXTERNAL_DEFINITION_DRAG, EXTERNAL_DRAG, openWorkflowWindow } from './actions'
import { Button, showMenu, Sheet, Spinner, TextField } from './controls'
import { Icon } from './icons'
import { L } from '../i18n'

const kindIcon = (k: string) => KIND_ICON[k as keyof typeof KIND_ICON] ?? 'circle'

/** Where a Card added from the sidebar goes: near the top left of what the canvas shows. */
function dropPoint(session: ProjectSession, tab: WorkflowTab) {
  const cam = session.cameras.get(key(tab.scope)) ?? { x: 40, y: 40, zoom: 1 }
  return { x: (240 - cam.x) / cam.zoom, y: (160 - cam.y) / cam.zoom }
}

export function WorkbenchSidebar({ workspace, session, tab }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab }) {
  useObserve(session, tab)
  const [importing, setImporting] = useState(false)
  const [importError, setImportError] = useState<string>()
  return (
    <div className="sidebar">
      <div className="sidebar-scroll">
        <div className="sidebar-section">
          <div className="sidebar-header">{L('Workflow 階層')}</div>
          <ScopeRow workspace={workspace} session={session} tab={tab} scope={[]} depth={0} />
        </div>
        <div className="sidebar-section">
          <div className="sidebar-header row center">
            <span>{L('Card を追加')}</span>
            <span className="spacer" />
            <Button kind="borderless" small iconOnly icon="square.and.arrow.down.on.square" disabled={!session.isEditable}
              help={L('他のプロジェクトを取り込む（externals にコピーして Card in Card として置きます）')} onClick={() => setImporting(true)} />
          </div>
          <Definitions session={session} tab={tab} />
        </div>
        <div className="sidebar-section">
          <div className="sidebar-header">{L('表示中の Card')}</div>
          <VisibleCards session={session} tab={tab} />
        </div>
      </div>
      <div className="sidebar-footer">
        <Button kind="borderless" iconOnly icon="point.3.connected.trianglepath.dotted" help={L('版の履歴（git）')} guide="versions"
          onClick={() => app.openWindow(session, { kind: 'versions' })} />
        <Button kind="borderless" iconOnly icon="gearshape" help={L('このプロジェクトの設定（設定ウインドウの「このプロジェクト」）')} guide="project_settings"
          onClick={() => app.openWindow(session, { kind: 'workflowSettings' })} />
      </div>
      {importing && (
        <FolderPicker message={L('取り込む Tsugumi のプロジェクト（tsugumi.json のあるフォルダ）を選んでください。externals にコピーされ、元のフォルダとは連動しません。')}
          requireProject confirmLabel={L('取り込む')} onClose={() => setImporting(false)}
          onPick={async (folder) => {
            setImporting(false)
            try {
              tab.setSelection([await session.importProject(folder, tab.scope, dropPoint(session, tab))])
            } catch (e) {
              setImportError(errorText(e))
            }
          }} />
      )}
      {importError && (
        <Sheet onClose={() => setImportError(undefined)} width={420}>
          <div className="confirm">
            <div className="confirm-title">{L('取り込めませんでした')}</div>
            <div className="secondary selectable">{importError}</div>
            <div className="row end"><Button kind="prominent" onClick={() => setImportError(undefined)}>OK</Button></div>
          </div>
        </Sheet>
      )}
    </div>
  )
}

/** Externals of this project: their definitions (copied into the scope as
 *  new Cards) and the external itself (a new Card in Card, a copy of its own);
 *  then the definition folders no Card uses. */
function Definitions({ session, tab }: { session: ProjectSession; tab: WorkflowTab }) {
  const [search, setSearch] = useState('')
  /** Expanded tree nodes; kept across searches and reloads. */
  const [expanded, setExpanded] = useState(new Set<string>())
  const q = search.toLowerCase()
  const has = (s: string) => s.toLowerCase().includes(q)
  const isOpen = (id: string) => expanded.has(id) || !!search
  const toggle = (id: string) => {
    const n = new Set(expanded)
    if (n.has(id)) n.delete(id)
    else n.add(id)
    setExpanded(n)
  }
  const exts = [...session.effectiveExternals.keys()].filter((p) => p.split('/').filter(Boolean).length === 2).sort()
  const unplaced = session.unplacedDefinitions.filter((id) => !search || has(id))
  const at = () => dropPoint(session, tab)
  const placeCopy = async (prefix: string, id: string) => {
    const k = await session.placeDefinitionCopy(prefix, id, tab.scope, at())
    if (k) tab.setSelection([k])
  }
  const placeExternal = async (prefix: string) => {
    const k = await session.placeExternalCopy(prefix, tab.scope, at())
    if (k) tab.setSelection([k])
  }
  const place = (id: string) => {
    if (!session.isEditable || tab.scope.length) return
    tab.setSelection([session.placeDefinition(id, at())])
  }
  return (
    <>
      <div className="sidebar-pad"><TextField small value={search} onChange={setSearch} placeholder={L('検索')} /></div>
      {exts.length === 0 && (
        <div className="sidebar-note caption secondary">
          {L('externals にプロジェクトがありません。取り込みボタンから他のプロジェクトを取り込めます。組み込みの Card は Canvas の右クリックか + から追加します。')}
        </div>
      )}
      {exts.map((prefix) => {
        const name = prefix.split('/').filter(Boolean).pop() ?? prefix
        const e = session.external(prefix)
        const ids = [...(e?.definitions.keys() ?? [])].sort().filter((id) => !search || has(id) || has(name))
        return (
          <div key={prefix}>
            <TreeHead open={isOpen(prefix)} onToggle={() => toggle(prefix)} icon="shippingbox" label={name} />
            {isOpen(prefix) && (
              <>
                <DefinitionRow session={session} icon={KIND_ICON.subgraph} title={L('{0} 全体（Card in Card）', name)} drag={EXTERNAL_DRAG + prefix}
                  help={L('このプロジェクト全体のコピーを Card in Card として置きます')} onAdd={() => void placeExternal(prefix)} />
                {ids.map((id) => {
                  const d = e?.definitions.get(id)
                  return (
                    <DefinitionRow key={id} session={session} icon={d?.link !== undefined ? KIND_ICON.subgraph : kindIcon(definitionKind(d?.card))} title={id}
                      drag={EXTERNAL_DEFINITION_DRAG + prefix + '\n' + id} help={L('定義をこのプロジェクトの definitions にコピーして Card を置きます')}
                      onAdd={() => void placeCopy(prefix, id)} />
                  )
                })}
              </>
            )}
          </div>
        )
      })}
      {unplaced.length > 0 && (
        <div>
          <TreeHead open={isOpen('unplaced')} onToggle={() => toggle('unplaced')} icon="tray" label={L('配置されていない定義')} secondary />
          {isOpen('unplaced') && unplaced.map((id) => (
            <DefinitionRow key={id} session={session} icon={kindIcon(definitionKind(session.definitions.get(id)))} title={id} drag={DEFINITION_DRAG + id}
              help={L('ダブルクリックまたは Canvas へドラッグして配置')} onAdd={() => place(id)}
              menu={[
                { label: L('Canvas に配置'), action: () => place(id), disabled: !session.isEditable },
                { label: L('削除'), destructive: true, disabled: !session.isEditable, action: () => session.edit((x) => x.removeDefinition(id)) },
              ]} />
          ))}
        </div>
      )}
    </>
  )
}

function TreeHead({ open, onToggle, icon, label, secondary }: { open: boolean; onToggle: () => void; icon: string; label: string; secondary?: boolean }) {
  return (
    <div className={`sidebar-row tree-head${secondary ? ' secondary callout' : ''}`} role="button" aria-expanded={open} onClick={onToggle}>
      <span className="disclosure-toggle"><Icon name={open ? 'chevron.down' : 'chevron.right'} size={9} /></span>
      <Icon name={icon} className="secondary" />
      <span className="ellipsis flex1">{label}</span>
    </div>
  )
}

function DefinitionRow({ session, icon, title, drag, help, onAdd, menu }: {
  session: ProjectSession; icon: string; title: string; drag: string; help: string; onAdd: () => void
  menu?: Parameters<typeof showMenu>[1]
}) {
  return (
    <div className="sidebar-row def-row tree-leaf" draggable title={help + L('（ダブルクリックまたは Canvas へドラッグ）')}
      onDragStart={(e) => { e.dataTransfer.setData('text/plain', drag); e.dataTransfer.effectAllowed = 'copy' }}
      onDoubleClick={() => session.isEditable && onAdd()}
      onContextMenu={(e) => showMenu(e, menu ?? [{ label: L('Canvas に追加'), action: onAdd, disabled: !session.isEditable }])}>
      <Icon name={icon} className="secondary" />
      <span className="ellipsis flex1">{title}</span>
    </div>
  )
}

/** One scope in the hierarchy, with its child workflows. */
function ScopeRow({ workspace, session, tab, scope, depth }: { workspace: Workspace; session: ProjectSession; tab: WorkflowTab; scope: string[]; depth: number }) {
  const [expanded, setExpanded] = useState(true)
  const children = session.childScopes(scope)
  const current = key(tab.scope) === key(scope)
  return (
    <>
      <div className={`sidebar-row scope-row${current ? ' current' : ''}`} style={{ paddingLeft: 10 + depth * 14 }}
        onClick={() => tab.enter(scope)}
        onContextMenu={(e) => showMenu(e, [
          { label: L('このタブで開く'), action: () => tab.enter(scope) },
          { label: L('新しいタブで開く'), action: () => workspace.openTab(scope) },
          { label: L('別ウインドウで開く'), action: () => openWorkflowWindow(session, scope) },
        ])}>
        {children.length > 0 ? (
          <button type="button" className="disclosure-toggle" aria-expanded={expanded} onClick={(e) => { e.stopPropagation(); setExpanded(!expanded) }}>
            <Icon name={expanded ? 'chevron.down' : 'chevron.right'} size={9} />
          </button>
        ) : <span className="disclosure-toggle" />}
        <Icon name={scope.length === 0 ? 'square.stack.3d.up.fill' : 'square.stack.3d.up'} style={{ color: current ? 'var(--accent)' : 'var(--secondary)' }} />
        <span className="ellipsis flex1" style={{ fontWeight: current ? 600 : 400 }}>{session.scopeName(scope)}</span>
        {current && <span className="caption2 secondary">{L('表示中')}</span>}
      </div>
      {expanded && children.map((c) => <ScopeRow key={key(c)} workspace={workspace} session={session} tab={tab} scope={c} depth={depth + 1} />)}
    </>
  )
}

/** The Cards of the current scope, under the headings of their visual
 *  groups. A row selects its Card and moves the canvas to it; a composite
 *  Card has an explicit "Open". Collapsing is done on the Card itself. */
function VisibleCards({ session, tab }: { session: ProjectSession; tab: WorkflowTab }) {
  const ids = session.cards(tab.scope).map((c) => str(get(c, 'id'))).filter((x): x is string => !!x)
  const groups = session.groups(tab.scope)
  const members = (g: (typeof groups)[number]) => (arr(get(g, 'cards')) ?? []).map((v) => str(v)).filter((x): x is string => !!x)
  const grouped = new Set(groups.flatMap(members))
  const row = (id: string, indent: boolean) => <VisibleCardRow key={id} session={session} tab={tab} cardKey={key([...tab.scope, id])} indent={indent} />
  return (
    <>
      {ids.length === 0 && <div className="sidebar-row secondary">{L('Card はありません')}</div>}
      {groups.map((g) => {
        const gid = str(get(g, 'id'))
        const list = members(g).filter((m) => ids.includes(m))
        if (!gid || !list.length) return null
        return (
          <div key={gid}>
            <div className="sidebar-row group-head caption headline secondary"><Icon name="rectangle.3.group" size={12} />{str(get(g, 'name')) ?? L('グループ')}</div>
            {list.map((id) => row(id, true))}
          </div>
        )
      })}
      {ids.filter((id) => !grouped.has(id)).map((id) => row(id, groups.length > 0))}
    </>
  )
}

/** A Card of the current scope: name, kind and run state. */
function VisibleCardRow({ session, tab, cardKey, indent }: { session: ProjectSession; tab: WorkflowTab; cardKey: CardKey; indent: boolean }) {
  const st = session.cardStates.get(cardKey)
  const errors = session.resolution.get(cardKey)?.diagnostics.filter((d) => d.error).length ?? 0
  const kind = session.kind(cardKey)
  const selected = tab.selection.has(cardKey)
  const cs = st ? cardState(st.state) : undefined
  return (
    <div className={`sidebar-row${selected ? ' selected' : ''}`} style={{ paddingLeft: indent ? 20 : undefined }} role="option" aria-selected={selected}
      aria-label={L('{0}, {1}, {2}', session.displayName(cardKey), KIND_LABEL[kind], cs ? cs.label : '未実行')}
      onClick={(e) => {
        if (e.metaKey || e.ctrlKey || e.shiftKey) {
          const s = new Set(tab.selection)
          if (s.has(cardKey)) s.delete(cardKey)
          else s.add(cardKey)
          tab.setSelection(s)
          return
        }
        tab.setSelection([cardKey])
        canvasFor(session, tab).reveal(cardKey)
      }}>
      <Icon name={kindIcon(kind)} className={selected ? '' : 'secondary'} size={13} />
      <span className="ellipsis flex1">{session.displayName(cardKey)}</span>
      {errors > 0 && <Icon name="exclamationmark.triangle.fill" style={{ color: 'var(--yellow)' }} title={L('問題 {0} 件', errors)} />}
      {st && cs && (
        <span className="label" title={L('{0}（実行 #{1}）', cs.label, st.invocation)}>
          <Icon name={cs.icon} style={{ color: selected ? undefined : `var(--${cs.color})` }} />
          {st.invocation > 0n && <span className="caption2 mono-digits secondary">#{String(st.invocation)}</span>}
        </span>
      )}
      {kind === 'subgraph' && (
        <Button kind="borderless" small help={L('Card in Card の中を表示（読み取り専用。編集は externals のプロジェクトを単体で開いて行います）')}
          onClick={() => tab.enter(path(cardKey))}>{L('開く')}</Button>
      )}
    </div>
  )
}

/** Browses folders on the host (the web counterpart of the open panel). */
export function FolderPicker({ initial, message, onPick, onPickMultiple, onClose, allowCreate, requireProject, confirmLabel }: {
  initial?: string; message: string; onClose: () => void; allowCreate?: boolean; requireProject?: boolean
  /** The choose button's title (an open panel's prompt). */
  confirmLabel?: string
} & ({ onPick: (path: string) => void; onPickMultiple?: never } | { onPick?: never; onPickMultiple: (paths: string[]) => void })) {
  const [listing, setListing] = useState<FolderListing>()
  const [error, setError] = useState<string>()
  const [typed, setTyped] = useState('')
  const [newFolder, setNewFolder] = useState<string | null>(null)
  const [selected, setSelected] = useState<string[]>([])
  const toggle = (folder: string) => setSelected(current => current.includes(folder) ? current.filter(p => p !== folder) : [...current, folder])
  const load = async (p?: string) => {
    try {
      const l = await api<FolderListing>(`/api/fs${p ? `?path=${encodeURIComponent(p)}` : ''}`)
      setListing(l)
      setTyped(l.path)
      setError(undefined)
    } catch (e) {
      setError(errorText(e))
    }
  }
  useEffect(() => {
    void load(initial)
  }, [initial])
  const can = !!listing && (!requireProject || listing.project)
  return (
    <Sheet onClose={onClose} width={560}>
      <div className="sheet-body">
        <div className="headline">{message}</div>
        <form className="row gap8" onSubmit={(e) => { e.preventDefault(); void load(typed) }}>
          <Button iconOnly icon="chevron.backward" help={L('上のフォルダ')} disabled={!listing?.parent} onClick={() => listing?.parent && void load(listing.parent)} />
          <TextField value={typed} onChange={setTyped} mono style={{ flex: 1 }} />
          <Button type="submit">{L('移動')}</Button>
        </form>
        <div className="list bordered" style={{ height: 300 }}>
          {!listing && !error && <Spinner />}
          {listing?.entries.map((e) => (
            <div key={e.name} className="list-row clickable" onDoubleClick={() => (e.project && requireProject && onPick ? onPick(`${listing.path}/${e.name}`) : void load(`${listing.path}/${e.name}`))}
              onClick={() => void load(`${listing.path}/${e.name}`)}>
              {onPickMultiple && <input type="checkbox" aria-label={`${L('選択')}: ${e.name}`} checked={selected.includes(`${listing.path.replace(/\/$/, '')}/${e.name}`)}
                onClick={event => event.stopPropagation()} onDoubleClick={event => event.stopPropagation()}
                onChange={() => toggle(`${listing.path.replace(/\/$/, '')}/${e.name}`)} />}
              <Icon name={e.project ? 'square.stack.3d.up' : 'folder'} style={{ color: e.project ? 'var(--accent)' : 'var(--secondary)' }} />
              <span className="flex1">{e.name}</span>
              {e.project && <span className="caption2 secondary">{L('プロジェクト')}</span>}
            </div>
          ))}
          {listing && listing.entries.length === 0 && <div className="list-row secondary">{L('サブフォルダはありません')}</div>}
        </div>
        {onPickMultiple && <>
          <Button disabled={!listing} onClick={() => listing && toggle(listing.path)}>{listing && selected.includes(listing.path) ? L('このフォルダの選択を解除') : L('このフォルダを追加')}</Button>
          {selected.length > 0 && <div className="list bordered" style={{ maxHeight: 140, overflowY: 'auto' }}>
            {selected.map(folder => <label key={folder} className="list-row gap8">
              <input type="checkbox" checked onChange={() => toggle(folder)} />
              <span className="selectable" style={{ overflowWrap: 'anywhere' }}>{folder}</span>
            </label>)}
          </div>}
        </>}
        {error && <div style={{ color: 'var(--red)' }} className="callout selectable">{error}</div>}
        {requireProject && listing && !listing.project && <div className="caption secondary">{L('tsugumi.json を含むフォルダを選んでください')}</div>}
        <div className="row gap8">
          {allowCreate && (newFolder === null
            ? <Button onClick={() => setNewFolder('')}>{L('新規フォルダ…')}</Button>
            : (
              <form className="row gap8" onSubmit={async (e) => {
                e.preventDefault()
                if (!newFolder || !listing) return
                try {
                  setListing(await api<FolderListing>('/api/fs/mkdir', { parent: listing.path, name: newFolder }))
                  setNewFolder(null)
                } catch (er) {
                  setError(errorText(er))
                }
              }}>
                <TextField small value={newFolder} onChange={setNewFolder} placeholder={L('フォルダ名')} autoFocus />
                <Button small type="submit" disabled={!newFolder}>{L('作成')}</Button>
              </form>
            ))}
          <span className="spacer" />
          <Button onClick={onClose}>{L('キャンセル')}</Button>
          <Button kind="prominent" disabled={onPickMultiple ? !selected.length : !can} onClick={() => onPickMultiple ? onPickMultiple(selected) : listing && onPick(listing.path)}>{confirmLabel ?? L('選択')}{onPickMultiple && selected.length > 0 ? ` (${selected.length})` : ''}</Button>
        </div>
      </div>
    </Sheet>
  )
}
