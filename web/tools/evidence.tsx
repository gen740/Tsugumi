// Manual integration fixture: real navigation/Markdown/canvas with a read-only
// RPC replay. It uses the checked-in Cas A recording, not a newly executed run.
import { createRoot } from 'react-dom/client'
import { useState } from 'react'
import { EvidenceContext, evidenceLink } from '../src/ui/evidence'
import { Markdown } from '../src/ui/markdown'
import { GraphCanvas } from '../src/ui/canvas'
import { WindowHost } from '../src/ui/windows'
import { ProjectSession } from '../src/model/session'
import { Workspace } from '../src/model/workspace'
import { useObserve } from '../src/model/observable'
import { parse } from '../src/shared/json'
import { D, R } from '../src/shared/rpc'
import recorded from '../../docs/site/player-data/assets/casa.run.json'
import demo from '../../docs/site/player-data/casa.json'
import project from '../../demos/en/casa/tsugumi.json'
import layout from '../../demos/en/casa/layout.json'
import '../src/styles.css'

const run = recorded.run.id
const session = new ProjectSession('/evidence-replay', 'evidence-replay', 'Cas A evidence replay')
session.project = session.confirmedProject = parse(JSON.stringify(project))
session.layout = session.confirmedLayout = parse(JSON.stringify(layout))
for (const c of demo.cards) {
  session.confirmedDefinitions.set(c.id, parse(c.files.find(f => f.path === '_card.json')!.text))
  session.confirmedDefinitionMeta.set(c.id, parse(JSON.stringify({ description: c.summary })))
  session.resolution.set(c.id, { path: [c.id], inputs: c.inputs.map(p => ({ ...p, kind: D.PortKind.FlatBuffer, optional: false })), outputs: c.outputs.map(p => ({ ...p, kind: D.PortKind.FlatBuffer, optional: false })), diagnostics: [], nixReferences: [] })
}
const status = Object.assign(new R.RunStatusT(), { runId: run, state: D.RunState.succeeded })
session.runStatus = status
const summary = (id: string) => Object.assign(new R.InvocationSummaryT(), { card: Object.assign(new D.CardPathT(), { segments: [id] }), invocation: 1n, state: D.CardState.succeeded })
session.client = {
  call: async (type: R.RequestPayload, request: { runId?: string }) => {
    if (request.runId !== run) throw Error('Unknown run in this project')
    if (type === R.RequestPayload.RunStatusRequest) return status
    if (type === R.RequestPayload.RunLatestInvocationsRequest) return { records: Object.keys(recorded.timeline).map(summary) }
    throw Error('Unsupported replay request')
  },
  record: async (id: string, card: string[], invocation?: bigint) => {
    if (id !== run || card.length !== 1 || !(card[0] in recorded.timeline) || invocation != null && invocation !== 1n) throw Error('Unknown record')
    let display: D.DisplayT | undefined
    if (card[0] === 'bands') {
      const table = Object.assign(new D.DisplayTableT(), { columns: recorded.outputs.bands.columns, rows: recorded.outputs.bands.rows.map(cells => Object.assign(new D.TableRowT(), { cells })) })
      display = Object.assign(new D.DisplayT(), { runId: run, card: summary(card[0]).card, invocation: 1n, revision: 1n, valueType: D.DisplayValue.DisplayTable, value: table })
    } else if (card[0] === 'spectrum') {
      const image = Object.assign(new D.DisplayImageT(), { mediaType: 'image/png', alt: recorded.outputs.spectrum.alt, data: [...new Uint8Array(await (await fetch('/casa-spectrum.png')).arrayBuffer())] })
      display = Object.assign(new D.DisplayT(), { runId: run, card: summary(card[0]).card, invocation: 1n, revision: 1n, valueType: D.DisplayValue.DisplayImage, value: image })
    }
    return { record: { summary: summary(card[0]), inputs: [], outputs: [], display }, status: { state: D.CardState.succeeded }, exists: true }
  },
  logRange: async () => ({ exists: false, sizeBytes: 0n }),
  artifacts: async () => [],
  setVisible: async () => {},
  viewSave: async () => {},
} as never
const workspace = new Workspace()
workspace.open(session)
const tab = workspace.active!
const initial = `[bands](card:bands): red 101187, green 157471, blue 110852 photons. [bands result](run:${run}/bands#1).

[spectrum](card:spectrum) completed successfully. [spectrum result](run:${run}/spectrum#1). [Run record](run:${run}).

Unavailable: [missing Card](card:missing), [missing invocation](run:${run}/spectrum#9), [other project run](run:00000000-0000-0000-0000-000000000000), [missing log](log:${run}/spectrum?invocation=1&stream=stderr&offset=0), [missing artifact](artifact:${run}?id=missing).`
function Fixture() {
  const [answer, setAnswer] = useState(initial)
  useObserve(tab)
  return <><div style={{ height: '100vh', display: 'grid', gridTemplateColumns: '390px 1fr' }}>
    <aside style={{ padding: 16, overflow: 'auto' }}><h2>Cas A evidence replay</h2><p>Recorded run: {run}</p>
      <p>Selected Card: <strong>{[...tab.selection].join(', ') || 'none'}</strong></p>
      <label>Agent answer<textarea aria-label="Agent answer" value={answer} onChange={e => setAnswer(e.target.value)} style={{ width: '100%', height: 100 }} /></label>
      <EvidenceContext.Provider value={{ session, tab }}><Markdown text={answer} renderLink={evidenceLink} /></EvidenceContext.Provider>
    </aside>
    <main style={{ position: 'relative', overflow: 'hidden' }}><GraphCanvas workspace={workspace} session={session} tab={tab} /></main>
  </div><WindowHost /></>
}
createRoot(document.getElementById('root')!).render(<Fixture />)
