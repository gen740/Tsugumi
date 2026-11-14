import { VersionCommentT } from '../gen/tsugumi/host/version-comment'
// The project's versions as a tree (core/src/versions.cc): each commit with its
// branches, tags, runs (from its note) and changed Cards; hovering one shows
// what it changed and a minimap of its Cards. A version can be named, and the
// project can start again from any of them (a change there opens a branch).
import { useEffect, useRef, useState } from 'react'
import * as R from '../gen/tsugumi/rpc'
import { CardGeometry, CardNodeData } from '../model/canvasKit'
import { useObserve } from '../model/observable'
import { FlowDirection } from '../model/prefs'
import { cardKindNamed, CardKind, commentChange, ProjectSession } from '../model/session'
import { Button, Confirm, Empty, Sheet, TextField } from './controls'
import { Icon } from './icons'
import { L } from '../i18n'

const ROW = 58
const HOVER_H = 200
const LANE = 18

interface Edge { lane: number; toRow: number; toLane: number }
interface Row { lane: number; edges: Edge[] }

/** Lanes of a version graph, newest first: each commit's column and, for each
 *  parent, the column its line runs down before it turns into the parent. */
export function versionLayout(commits: { id: string; parents: string[] }[]): { rows: Row[]; lanes: number } {
  const waiting: (string | null)[] = []
  const rowOf = new Map(commits.map((c, i) => [c.id, i]))
  const laneOf = new Map<string, number>()
  const rows: Row[] = []
  const pending: { row: number; lane: number; parent: string }[] = []
  let lanes = 1
  const free = () => {
    const f = waiting.indexOf(null)
    if (f >= 0) return f
    waiting.push(null)
    return waiting.length - 1
  }
  commits.forEach((c, i) => {
    const w = waiting.indexOf(c.id)
    const lane = w >= 0 ? w : free()
    waiting.forEach((x, j) => { if (j !== lane && x === c.id) waiting[j] = null })
    laneOf.set(c.id, lane)
    rows.push({ lane, edges: [] })
    waiting[lane] = null
    c.parents.forEach((p, k) => {
      if (!rowOf.has(p)) return
      let l: number
      if (k === 0) l = lane
      else {
        const e = waiting.indexOf(p)
        l = e >= 0 ? e : free()
      }
      waiting[l] = p
      pending.push({ row: i, lane: l, parent: p })
    })
    lanes = Math.max(lanes, waiting.length)
  })
  for (const e of pending) rows[e.row].edges.push({ lane: e.lane, toRow: rowOf.get(e.parent)!, toLane: laneOf.get(e.parent)! })
  return { rows, lanes }
}

const LANE_COLORS = ['#3b82f6', '#a855f7', '#14b8a6', '#ec4899', '#6366f1', '#a16207']
const stateColor = (s?: string | null) => (s === 'succeeded' ? 'var(--green)' : s === 'failed' ? 'var(--red)' : 'var(--secondary)')
const stateLabel = (s?: string | null) => (s === 'succeeded' ? L('成功') : s === 'failed' ? L('失敗') : s === 'cancelled' ? L('停止') : s ?? '')
const str = (v: string | Uint8Array | null | undefined) => (v == null ? '' : typeof v === 'string' ? v : new TextDecoder().decode(v))
const list = (v: (string | Uint8Array | null)[] | undefined) => (v ?? []).map(str)

function changedText(changed: string[]) {
  const names = changed.map((k) => k || 'Workflow')
  return L('変更: {0}{1}', names.slice(0, 4).join('、'), names.length > 4 ? L(' ほか {0}', names.length - 4) : '')
}

export interface MiniMapCard { id: string; x: number; y: number; kind: CardKind; fan: number; collapsed: boolean }

/** A Card as far as a version says: its shape and the size the canvas gives it
 *  with one port on each side and no parameters, never more than it really
 *  takes, so what does not overlap on the canvas does not overlap here. */
function miniMapCardData(c: MiniMapCard): CardNodeData {
  const ports = (n: number) => Array.from({ length: n }, (_, i) => ({ name: `p${i}` }))
  return {
    title: c.id, kind: c.kind, inputs: ports(c.kind === 'merge' ? c.fan : 1), outputs: ports(c.kind === 'fork' ? c.fan : 1), params: [],
    invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: c.collapsed, hasDisplay: false,
  }
}

/** Where a version's Cards go in a minimap at most width x height, each with
 *  the outline it has on the canvas: the whole graph fitted and centered
 *  across, the box's height, and whether a Card is wide enough for its name. */
export function miniMapLayout(cards: MiniMapCard[], flow: FlowDirection, width: number, height: number) {
  const PAD = 8
  if (cards.length === 0) return { cards: [], height: 0, named: false }
  const placed = cards.map((c) => ({ c, g: new CardGeometry(miniMapCardData(c), flow) }))
  const left = Math.min(...cards.map((c) => c.x)), top = Math.min(...cards.map((c) => c.y))
  const spanX = Math.max(...placed.map(({ c, g }) => c.x + g.size.width)) - left, spanY = Math.max(...placed.map(({ c, g }) => c.y + g.size.height)) - top
  const k = Math.min((width - PAD * 2) / spanX, (height - PAD * 2) / spanY, 0.3)
  const ox = (width - spanX * k) / 2
  return {
    cards: placed.map(({ c, g }) => {
      const x = ox + (c.x - left) * k, y = PAD + (c.y - top) * k, o = g.outlineRect
      return {
        id: c.id, x, y, w: g.size.width * k, h: g.size.height * k, shape: g.shape, draft: g.draft,
        outline: g.outline({ x: x + o.x * k, y: y + o.y * k, width: o.width * k, height: o.height * k }, k),
      }
    }),
    height: spanY * k + PAD * 2, named: CardGeometry.width * k >= 34,
  }
}

const MAP_W = 284
const MAP_H = 132

/** A version's Cards and how they connect; those it changed stand out. */
function MiniMap({ canvas, changed }: { canvas: R.VersionCanvasT; changed: string[] }) {
  const cards = canvas.cards.filter((c): c is R.VersionCanvasCardT => !!c)
    .map((c) => ({ id: str(c.id), x: c.x, y: c.y, kind: cardKindNamed(str(c.kind)), fan: c.fan, collapsed: c.collapsed }))
  const map = miniMapLayout(cards, str(canvas.flow) === 'topToBottom' ? 'topToBottom' : 'leftToRight', MAP_W, MAP_H)
  const at = new Map(map.cards.map((c) => [c.id, c]))
  const touched = new Set(changed.map((k) => k.split('/')[0]))
  return (
    <svg className="version-map" width={MAP_W} height={map.height}>
      {canvas.wires.map((w, i) => {
        const a = at.get(str(w?.from)), b = at.get(str(w?.to))
        return a && b ? <line key={i} x1={a.x + a.w / 2} y1={a.y + a.h / 2} x2={b.x + b.w / 2} y2={b.y + b.h / 2} stroke="var(--secondary)" strokeWidth={1} /> : null
      })}
      {map.cards.map((c) => (
        <g key={c.id}>
          <path d={c.outline} className="ground" />
          <path d={c.outline} className={touched.has(c.id) ? 'changed' : ''} strokeDasharray={c.draft ? '3 2' : undefined} />
          {map.named && (c.shape === 'card' || c.shape === 'start') && <text x={c.x + c.w / 2} y={c.y + c.h / 2 + 3} textAnchor="middle">{c.id.length * 5 > c.w ? `${c.id.slice(0, Math.max(1, Math.floor(c.w / 5) - 1))}…` : c.id}</text>}
        </g>
      ))}
    </svg>
  )
}

/** What shows while the pointer is on a version: what it changed, in a model's
 *  words, and its Cards. */
function VersionHover({ session, commit, top }: { session: ProjectSession; commit: R.VersionCommitT; top: number }) {
  const id = str(commit.id)
  const canvas = session.versionCanvases.get(id)
  const error = session.versionNoteErrors.get(id)
  return (
    <div className="version-hover" style={{ top }}>
      {session.versionNotes.get(id) ? <div>{session.versionNotes.get(id)}</div>
        : session.versionNoteBusy.has(id) ? <div className="secondary">{L('ノートを読み込んでいます…')}</div>
          : <div>{str(commit.summary)}</div>}
      {error && <div className="caption" style={{ color: 'var(--red)' }}>{L('ノートを読み込めませんでした: {0}', error)}</div>}
      {canvas && canvas.cards.length > 0 && <MiniMap canvas={canvas} changed={list(commit.changed)} />}
    </div>
  )
}

function ago(ms: number) {
  const s = Math.max(0, (Date.now() - ms) / 1000)
  if (s < 90) return L('さっき')
  if (s < 3600) return L('{0} 分前', Math.round(s / 60))
  if (s < 86400 * 2) return L('{0} 時間前', Math.round(s / 3600))
  return L('{0} 日前', Math.round(s / 86400))
}

export function VersionTreeView({ session }: { session: ProjectSession }) {
  useObserve(session)
  const [selected, setSelected] = useState<string>()
  const [error, setError] = useState<string>()
  const [naming, setNaming] = useState(false)
  const [confirmStart, setConfirmStart] = useState<string>()
  // The version under the pointer, and the one whose panel shows (a moment later).
  const [pointed, setPointed] = useState<string>()
  const [hovered, setHovered] = useState<string>()
  const treeRef = useRef<HTMLDivElement>(null)
  useEffect(() => { void session.refreshVersions() }, [session, session.rev])
  useEffect(() => { void session.loadVersionNoteMode().catch((e) => setError(String(e))) }, [session])
  useEffect(() => {
    setHovered(undefined)
    if (!pointed) return
    const t = setTimeout(() => {
      setHovered(pointed)
      void session.loadVersionCanvas(pointed)
      void session.loadVersionNote(pointed)
    }, 250)
    return () => clearTimeout(t)
  }, [session, pointed, session.versionNoteMode])
  useEffect(() => {
    if (selected) void session.loadVersionNote(selected)
  }, [session, selected, session.versionNoteMode])
  // The run's note is written as the run ends: read it a moment later.
  useEffect(() => {
    const t = setTimeout(() => void session.refreshVersions(), 600)
    return () => clearTimeout(t)
  }, [session, session.runActive])
  const act = (f: () => Promise<void>) => void f().catch((e) => setError(String(e?.message ?? e)))
  const g = session.versionGraph
  // What failed is said over any of the views below.
  const failure = error && <Confirm title={L('版を操作できませんでした')} message={error} confirm="OK" onConfirm={() => setError(undefined)} onCancel={() => setError(undefined)} />
  if (!g && session.versionGraphError) {
    return (
      <Empty title={L('版を読み込めませんでした')} description={session.versionGraphError}>
        <Button onClick={() => void session.refreshVersions()}>{L('再試行')}</Button>
      </Empty>
    )
  }
  if (!g) return <Empty title={L('版を読み込んでいます…')} />
  if (!g.enabled) {
    return (
      <Empty title={L('版管理されていません')}
        description={L('このプロジェクトを git のリポジトリにすると、実行するたびにその版が残り、いつでも前の版から始め直せます。')}>
        <Button kind="prominent" onClick={() => act(() => session.makeRepository())}>{L('版管理を始める')}</Button>
        {failure}
      </Empty>
    )
  }
  const commits = g.commits.filter((c): c is R.VersionCommitT => !!c)
  const layout = versionLayout(commits.map((c) => ({ id: str(c.id), parents: list(c.parents) })))
  const head = str(g.head)
  const branch = str(g.branch)
  const x = (lane: number) => 10 + lane * LANE + LANE / 2
  const y = (row: number) => row * ROW + 20
  const sel = commits.find((c) => str(c.id) === selected)
  const rowAt = (clientY: number) => {
    const box = treeRef.current?.getBoundingClientRect()
    const row = box ? Math.floor((clientY - box.top + (treeRef.current?.scrollTop ?? 0)) / ROW) : -1
    return row >= 0 && row < commits.length ? str(commits[row].id) : undefined
  }
  // Under its row, or above it near the bottom of the tree.
  const hoverTop = (commit: string) => {
    const tree = treeRef.current
    const row = commits.findIndex((c) => str(c.id) === commit) * ROW - (tree?.scrollTop ?? 0)
    return row + ROW + HOVER_H > (tree?.clientHeight ?? 0) ? Math.max(4, row - HOVER_H) : row + ROW
  }
  const shown = commits.find((c) => str(c.id) === hovered)
  return (
    <div className="col fill versions">
      <div className="row gap8 versions-header">
        <Icon name="point.3.connected.trianglepath.dotted" />
        <span>{branch ? L('枝: {0}', branch) : L('枝の外（次の変更で新しい枝）')}</span>
        {(g.dirty || session.dirty) && <span className="caption" style={{ color: 'var(--orange)' }}>{L('最後の版から変更あり（次の実行で版になります）')}</span>}
        <span className="flex1" />
        <div className="col gap4">
          <label>{L('版のノート')} <select value={session.versionNoteMode}
            onChange={(e) => act(() => session.setVersionNoteMode(e.target.value as 'mechanical' | 'agent'))}>
            <option value="mechanical">{L('変更の記録')}</option>
            <option value="agent">{L('Agent の要約')}</option>
          </select></label>
          <div className="caption secondary">{L('変更の記録は Card の追加・削除と定義・ソース・パラメータ・接続の変更を無料で記録します。Agent の要約は差分をモデルのプロバイダへ送り、token を使います。この設定は全プロジェクトに適用します。')}</div>
        </div>
        <Button disabled={!session.connected} onClick={() => setNaming(true)}>{L('版として残す…')}</Button>
        <Button kind="borderless" iconOnly icon="arrow.clockwise" help={L('読み直す')} onClick={() => void session.refreshVersions()} />
      </div>
      <div className="row fill" style={{ minHeight: 0 }}>
        <div className="flex1 version-tree" style={{ minWidth: 420 }}>
        <div className="scroll" ref={treeRef} style={{ height: '100%' }} onMouseLeave={() => setPointed(undefined)}
          onMouseMove={(e) => setPointed(rowAt(e.clientY))} onScroll={() => setPointed(undefined)}
          onClick={(e) => { const id = rowAt(e.clientY); if (id) setSelected(id) }}>
          <div className="row" style={{ alignItems: 'flex-start' }}>
            <svg width={layout.lanes * LANE + 12} height={commits.length * ROW} style={{ flex: 'none' }}>
              {layout.rows.map((r, i) => r.edges.map((e, k) => {
                let d = `M${x(r.lane)},${y(i)}`
                if (e.lane !== r.lane) d += ` C${x(r.lane)},${y(i) + ROW * 0.35} ${x(e.lane)},${y(i) + ROW * 0.25} ${x(e.lane)},${y(i) + ROW * 0.6}`
                if (e.lane !== e.toLane) {
                  d += ` L${x(e.lane)},${y(e.toRow) - ROW * 0.6} C${x(e.lane)},${y(e.toRow) - ROW * 0.25} ${x(e.toLane)},${y(e.toRow) - ROW * 0.35} ${x(e.toLane)},${y(e.toRow)}`
                } else d += ` L${x(e.toLane)},${y(e.toRow)}`
                return <path key={`${i}-${k}`} d={d} fill="none" stroke={LANE_COLORS[e.lane % LANE_COLORS.length]} strokeOpacity={0.75} strokeWidth={2} />
              }))}
              {layout.rows.map((r, i) => {
                const c = commits[i]
                const runs = (c.runs ?? []).filter((x): x is R.VersionRunT => !!x)
                const last = runs[runs.length - 1]
                return (
                  <g key={str(c.id)}>
                    {last
                      ? <circle cx={x(r.lane)} cy={y(i)} r={6} fill={stateColor(str(last.state))} />
                      : <circle cx={x(r.lane)} cy={y(i)} r={6} fill="var(--bg, #fff)" stroke={LANE_COLORS[r.lane % LANE_COLORS.length]} strokeWidth={2} />}
                    {str(c.id) === head && <circle cx={x(r.lane)} cy={y(i)} r={10} fill="none" stroke="var(--accent)" strokeWidth={2} />}
                  </g>
                )
              })}
            </svg>
            <div className="col flex1">
              {commits.map((c) => {
                const id = str(c.id)
                const runs = (c.runs ?? []).filter((x): x is R.VersionRunT => !!x)
                const last = runs[runs.length - 1]
                const ok = runs.filter((x) => str(x.state) === 'succeeded').length
                const ng = runs.filter((x) => str(x.state) === 'failed').length
                return (
                  <div key={id} className={`version-row${selected === id ? ' selected' : ''}`} style={{ height: ROW }}>
                    <div className="row gap6">
                      <span className="ellipsis" style={{ fontWeight: id === head ? 600 : 400 }}>{str(c.summary)}</span>
                      {list(c.tags).map((t) => <span key={t} className="version-tag"><Icon name="tag" />{t}</span>)}
                      {list(c.branches).map((b) => <span key={b} className={`version-branch${b === branch ? ' current' : ''}`}>{b}</span>)}
                      <span className="flex1" />
                      <span className="caption secondary">{ago(Number(c.time ?? 0n))}</span>
                    </div>
                    <div className="row gap8 caption">
                      {last && <span style={{ color: stateColor(str(last.state)) }}>{L('実行 {0} 回（成功 {1}・失敗 {2}）· 最後: {3}', runs.length, ok, ng, stateLabel(str(last.state)))}</span>}
                      {list(c.changed).length > 0 && <span className="secondary ellipsis">{changedText(list(c.changed))}</span>}
                    </div>
                  </div>
                )
              })}
            </div>
          </div>
        </div>
        {shown && <VersionHover session={session} commit={shown} top={hoverTop(str(shown.id))} />}
        </div>
        <div className="scroll versions-detail">
          {sel ? (
            <div className="col gap8">
              <div className="title3">{str(sel.summary)}</div>
              {session.versionNotes.get(str(sel.id)) && <div className="selectable">{session.versionNotes.get(str(sel.id))}</div>}
              {session.versionNoteBusy.has(str(sel.id)) && <div className="secondary">{L('ノートを読み込んでいます…')}</div>}
              {session.versionNoteErrors.get(str(sel.id)) && <div className="caption" style={{ color: 'var(--red)' }}>
                {L('ノートを読み込めませんでした: {0}', session.versionNoteErrors.get(str(sel.id))!)}
                <Button onClick={() => void session.loadVersionNote(str(sel.id))}>{L('再試行')}</Button>
              </div>}
              {/* The message's body: what follows its first line (the summary). */}
              {str(sel.message).split('\n').slice(1).join('\n').trim() && (
                <div className="selectable" style={{ whiteSpace: 'pre-wrap' }}>{str(sel.message).split('\n').slice(1).join('\n').trim()}</div>
              )}
              <div className="caption mono">版 {str(sel.id).slice(0, 12)}</div>
              <div className="caption">{new Date(Number(sel.time ?? 0n)).toLocaleString()}</div>
              {list(sel.changed).length > 0 && <div className="caption">{changedText(list(sel.changed))}</div>}
              {(session.versionComments.get(str(sel.id)) ?? []).length > 0 && (
                <>
                  <div style={{ fontWeight: 600 }}>{L('コメント')}</div>
                  {(session.versionComments.get(str(sel.id)) ?? []).filter((x): x is VersionCommentT => !!x).map((c, i) => (
                    <div key={i} className="col caption">
                      <div className="secondary">{str(c.card)} · {commentChange(str(c.change), str(c.by) || undefined)}</div>
                      <div className="selectable" style={{ whiteSpace: 'pre-wrap' }}>{str(c.text)}</div>
                      {str(c.resolution) && <div className="secondary selectable">{str(c.resolution)}</div>}
                      {list(c.cards).length > 0 && <div className="secondary selectable">{L('変更した Card: {0}', list(c.cards).join(L('、')))}</div>}
                    </div>
                  ))}
                </>
              )}
              {(sel.runs ?? []).length > 0 ? (
                <>
                  <div style={{ fontWeight: 600 }}>{L('実行の結果')}</div>
                  {(sel.runs ?? []).filter((x): x is R.VersionRunT => !!x).map((r, i) => (
                    <div key={i} className="col caption">
                      <div className="row gap6">
                        <span style={{ width: 8, height: 8, borderRadius: 4, background: stateColor(str(r.state)) }} />
                        <span>{stateLabel(str(r.state))}</span>
                        <span className="secondary">{r.seconds.toFixed(1)} 秒</span>
                        <span className="flex1" />
                        <span className="secondary mono">{str(r.ended)}</span>
                      </div>
                      {str(r.summary) && <div className="secondary selectable">{str(r.summary)}</div>}
                    </div>
                  ))}
                </>
              ) : <div className="caption secondary">{L('この版での実行はありません。')}</div>}
              <Button disabled={(str(sel.id) === head && !g.dirty) || session.runActive || !session.connected}
                onClick={() => setConfirmStart(str(sel.id))}>{L('この版から始める…')}</Button>
              <div className="caption secondary">{L('git の履歴でも同じ情報を見られます（git log --notes=tsugumi、git tag）。')}</div>
            </div>
          ) : <div className="secondary">{L('版を選ぶと、その実行の結果と変わった Card を表示します。')}</div>}
        </div>
      </div>
      {confirmStart && (
        <Confirm title={L('この版から始めますか？')} confirm={L('この版から始める')}
          message={L('今の定義と追加パスは版として残ってから、この版の内容に戻ります。ここから変更して実行すると、新しい枝になります。')}
          onConfirm={() => { const c = confirmStart; setConfirmStart(undefined); act(() => session.startFrom(c)) }}
          onCancel={() => setConfirmStart(undefined)} />
      )}
      {naming && <NameSheet onClose={() => setNaming(false)} onSave={(n, m) => { setNaming(false); act(() => session.markVersion(n, m)) }} />}
      {failure}
    </div>
  )
}

function NameSheet({ onClose, onSave }: { onClose: () => void; onSave: (name: string, message: string) => void }) {
  const [name, setName] = useState('')
  const [message, setMessage] = useState('')
  return (
    <Sheet onClose={onClose} width={420}>
      <form className="sheet-body" onSubmit={(e) => { e.preventDefault(); if (name.trim()) onSave(name.trim(), message) }}>
        <div className="title2">{L('版として残す')}</div>
        <div className="secondary">{L('今の定義と追加パスの内容に名前を付けて残します（git の tag）。')}</div>
        <TextField value={name} onChange={setName} placeholder={L('名前（例: thin-bands）')} />
        <TextField value={message} onChange={setMessage} placeholder={L('メモ')} />
        <div className="row end gap8">
          <Button onClick={onClose}>{L('キャンセル')}</Button>
          <Button kind="prominent" type="submit" disabled={!name.trim()}>{L('残す')}</Button>
        </div>
      </form>
    </Sheet>
  )
}
