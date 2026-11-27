import { useObserve } from '../model/observable'
import { decodeLog } from '../model/logWindow'
import { runMark } from '../model/runMark'
import { createContext, ReactNode, useContext, useEffect, useState } from 'react'
import { L } from '../i18n'
import { app } from '../model/app'
import { canvasFor } from '../model/canvas'
import { Evidence, evidenceExists, isEvidence, parseEvidence } from '../model/evidence'
import { ProjectSession, cardState, key, runState } from '../model/session'
import { WorkflowTab } from '../model/workspace'
import { D, R, s, segs, sOpt } from '../wire/rpc'
import { Markdown } from './markdown'
import { openWorkflowWindow } from './actions'
import { artifactName, describeNamedValue } from '../wire/flatjson'
import { Button, download } from './controls'

export const EvidenceContext = createContext<{ session: ProjectSession; tab?: WorkflowTab } | undefined>(undefined)
const streamOf = (t: Evidence) => ({ output: D.LogStream.output, build: D.LogStream.build, journal: D.LogStream.journal })[t.stream ?? 'output']
export async function resolveEvidence(session: ProjectSession, target: Evidence) {
  if (target.kind === 'card') return session.resolution.has(key(target.card!))
  const rpc = session.rpc
  if (!rpc) return false
  return evidenceExists(target, {
    cardExists: card => session.resolution.has(key(card)),
    runExists: async run => !!(await rpc.call<R.RunStatusT>(R.RequestPayload.RunStatusRequest, Object.assign(new R.RunStatusRequestT(), { runId: run }))).runId,
    recordExists: async (run, card, invocation) => {
      const r = await rpc.record(run, card, invocation ?? null)
      return invocation === undefined ? true : r.record?.summary?.invocation === invocation
    },
    logExists: async t => { const r = await rpc.logRange(t.run!, t.card ?? null, t.invocation ?? null, streamOf(t), t.offset, false, 4); return r.exists !== false && (t.offset ?? 0n) <= (r.sizeBytes ?? 0n) },
    artifactExists: async t => { const a = await citedArtifact(rpc, t); return !!a && (!t.member || a.members.some(m => s(m.path) === t.member)) },
  })
}
async function citedArtifact(rpc: NonNullable<ProjectSession['rpc']>, t: Evidence): Promise<R.ArtifactInfoT | undefined> {
  const a = (await rpc.artifactsById([t.artifact!])).find(a => s(a.artifactId) === t.artifact)
  return a
}
export function evidenceLink(href: string, label: ReactNode): ReactNode | undefined {
  return isEvidence(href) ? <EvidenceLink href={href}>{label}</EvidenceLink> : undefined
}
function EvidenceLink({ href, children }: { href: string; children: ReactNode }) {
  const context = useContext(EvidenceContext)
  useObserve(context?.session)
  const [valid, setValid] = useState(false)
  const target = parseEvidence(href)
  useEffect(() => {
    let alive = true
    setValid(false)
    if (target && context) void resolveEvidence(context.session, target).then(v => { if (alive) setValid(v) })
    return () => { alive = false }
  }, [href, context?.session, context?.session.rev, context?.session.lastFinished, context?.session.recordingRevision])
  if (!valid || !target || !context) return <span aria-disabled="true" style={{ color: 'var(--secondary)' }} title={L('このプロジェクトに根拠の記録がありません')}>{children}</span>
  return <a href={href} onClick={e => {
    e.preventDefault()
    void resolveEvidence(context.session, target).then(exists => {
      if (!exists) return setValid(false)
      if (target.kind === 'card' && target.file) app.openWindow(context.session, { kind: 'evidence', href })
      else if (target.kind === 'card' && context.tab) {
        context.tab.enter(target.card!.slice(0, -1)); context.tab.setSelection([key(target.card!)]); requestAnimationFrame(() => canvasFor(context.session, context.tab!).reveal(key(target.card!)))
      } else if (target.kind === 'card' && app.cardNavigators.has(context.session)) app.cardNavigators.get(context.session)!(key(target.card!))
      else if (target.kind === 'card') openWorkflowWindow(context.session, target.card!.slice(0, -1), key(target.card!))
      else app.openWindow(context.session, { kind: 'evidence', href })
    })
  }}>{children}</a>
}

function EvidenceArtifactExport({ session, target, artifact, member }: { session: ProjectSession; target: Evidence; artifact: R.ArtifactInfoT; member: string }) {
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string>()
  return <><Button disabled={busy} onClick={() => {
    setBusy(true); setError(undefined)
    void (async () => {
      if (!session.rpc || !await resolveEvidence(session, target)) throw new Error(L('このプロジェクトに根拠の記録がありません'))
      const data = await session.rpc.readArtifact(s(artifact.artifactId), member)
      download(member.split('/').pop() ?? 'artifact', data)
    })().catch(e => setError(String(e.message ?? e))).finally(() => setBusy(false))
  }}>{busy ? L('読み込み中…') : L('書き出す…')}</Button>{error && <p role="alert">{error}</p>}</>
}

/** A cited record opens at its exact run, invocation, stream and byte offset. */
export function EvidenceView({ session, href }: { session: ProjectSession; href: string }) {
  useObserve(session)
  const [body, setBody] = useState<ReactNode>(L('読み込み中…'))
  useEffect(() => {
    let alive = true
    const load = async () => {
      const t = parseEvidence(href), rpc = session.rpc
      if (!t || !rpc || !await resolveEvidence(session, t)) throw new Error(L('このプロジェクトに根拠の記録がありません'))
      if (t.kind === 'card' && t.file) {
        const directory = session.definitionDir(key(t.card!))
        if (!directory) throw new Error(L('このプロジェクトに根拠の記録がありません'))
        const source = await session.sourceText(directory + t.file)
        const lines = source.split('\n')
        return <pre className="selectable wrap-pre">{lines.slice(t.lineStart! - 1, t.lineEnd).map((line, i) => `${t.lineStart! + i}: ${line}`).join('\n')}</pre>
      }
      if (t.kind === 'log') {
        const log = await rpc.logRange(t.run!, t.card ?? null, t.invocation ?? null, streamOf(t), t.offset)
        return <><p>{t.stream} · {L('位置')} {String(log.offset ?? 0n)}</p><pre className="selectable wrap-pre">{decodeLog(log.text)}</pre></>
      }
      if (t.kind === 'artifact') {
        const a = await citedArtifact(rpc, t)
        if (!a) throw new Error(L('このプロジェクトに根拠の記録がありません'))
        const member = t.member ?? (a.members[0] ? s(a.members[0].path) : undefined)
        const bytes = member ? await rpc.call<R.FileContentT>(R.RequestPayload.ArtifactReadRequest, Object.assign(new R.ArtifactReadRequestT(), { artifactId: t.artifact, member: member, limitBytes: 65536 })) : undefined
        let preview: ReactNode = L('バイナリの成果物です')
        if (bytes) {
          const data = Uint8Array.from(bytes.bytes)
          const mime = /\.png$/i.test(member ?? '') ? 'image/png' : /\.jpe?g$/i.test(member ?? '') ? 'image/jpeg' : undefined
          if (mime && bytes.eof === true) preview = <img alt={member} style={{ maxWidth: '100%' }} src={`data:${mime};base64,${btoa(Array.from(data, c => String.fromCharCode(c)).join(''))}`} />
          else { try { preview = <pre className="selectable wrap-pre">{new TextDecoder('utf-8', { fatal: true }).decode(data)}</pre> } catch { /* The binary label remains visible. */ } }
        }
        return <><p>{session.displayPath(segs(a.card))} · {artifactName(s(a.artifactId), sOpt(a.label))} · #{String(a.invocation)}</p><p>{member}</p>{member && <EvidenceArtifactExport session={session} target={t} artifact={a} member={member} />}{preview}{bytes && bytes.eof !== true && <p>{L('先頭 64 KiB を表示')}</p>}</>
      }
      if (t.card) {
        const r = await rpc.record(t.run!, t.card, t.invocation ?? null)
        const record = r.record
        const summary = record?.summary ?? r.status
        return <>
          {runMark(r.run) && <p>{runMark(r.run)}</p>}
          <h3>{session.displayPath(t.card)} · #{String(record?.summary?.invocation ?? t.invocation ?? '')}</h3>
          {summary && <><p>{cardState(summary.state).label}</p><Markdown text={s(summary.failure?.diagnostic)} /></>}
          {record && [[L('入力'), record.inputs], [L('出力'), record.outputs]].map(([label, values], i) => <section key={i}>
            <h4>{label as string}</h4>
            {(values as D.NamedValueT[]).map((v, j) => <pre key={j} className="selectable wrap-pre">{describeNamedValue(v)}</pre>)}
          </section>)}
        </>
      }
      const status = await rpc.call<R.RunStatusT>(R.RequestPayload.RunStatusRequest, Object.assign(new R.RunStatusRequestT(), { runId: t.run }))
      const page = await rpc.call<R.InvocationPageT>(R.RequestPayload.RunLatestInvocationsRequest, Object.assign(new R.RunLatestInvocationsRequestT(), { runId: t.run }))
      return <><p>{runState(status.state).label}</p>{runMark(status) && <p>{runMark(status)}</p>}{page.records.map((r, i) => <p key={i}><EvidenceContext.Provider value={{ session }}><EvidenceLink href={`run:${t.run}/${segs(r.card).join('/')}#${r.invocation}`}>{session.displayPath(segs(r.card))} · #{String(r.invocation)} · {cardState(r.state).label}</EvidenceLink></EvidenceContext.Provider></p>)}</>
    }
    void load().then(v => { if (alive) setBody(v) }).catch(e => { if (alive) setBody(String(e.message ?? e)) })
    return () => { alive = false }
  }, [session, href, session.recordingRevision])
  return <div className="scroll pad16"><h2>{L('根拠の記録')}</h2><p className="caption selectable">{href}</p>{body}</div>
}
