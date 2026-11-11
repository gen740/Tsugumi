// Manual UI fixture using the real canvas and Card settings with deterministic RPC data.
import { createRoot } from 'react-dom/client'
import { useState } from 'react'
import { app } from '../src/model/app'
import { ProjectSession } from '../src/model/session'
import { Workspace, CardEditorCategory } from '../src/model/workspace'
import { parse } from '../src/shared/json'
import { R } from '../src/shared/rpc'
import { GraphCanvas } from '../src/ui/canvas'
import { NixReferenceControls } from '../src/ui/nixReferences'
import { CardSettingsEditor } from '../src/ui/cardSettings'
import '../src/styles.css'
import '../src/styles/canvas.css'
import '../src/styles/detail.css'
import '../src/styles/guide.css'

const session = new ProjectSession('/fixture', 'fixture', 'Nix references')
session.project = parse('{"name":"Nix references","canvas":{"cards":[],"connections":[]}}')
session.layout = parse('{}')
session.scheduleSave = () => {}
session.addCard('python', [], { x: 50, y: 100 }, 'source')
session.addCard('python', [], { x: 450, y: 100 }, 'analysis')
session.addCard('python', [], { x: 800, y: 340 }, 'report')
for (const name of ['source', 'analysis', 'report']) session.resolution.set(name, {
  path: [name], inputs: [], outputs: [], diagnostics: [], nixReferences: name === 'source' ? [] : [
    { target: [name === 'analysis' ? 'source' : 'analysis'], name: name === 'analysis' ? 'source' : 'analysis', local: name === 'report', valid: true }],
})
const workspace = new Workspace()
workspace.open(session)
const tab = workspace.active!
const paths = Array.from({ length: 205 }, (_, i) => '/nix/store/' + 'a'.repeat(32) + '-package-' + String(i).padStart(3, '0'))
let fail = false
const rpc = {
  async history() { return { runs: [{ status: { runId: 'run' } }] } },
  async record() { return { record: { summary: { environmentId: 'env' } } } },
  async call(type: number, request: any) {
    await new Promise(r => setTimeout(r, request.query === 'slow' ? 800 : 30))
    if (fail) throw Error('Fixture connection failure')
    if (type === R.RequestPayload.RunEnvironmentsRequest) return { environments: [{ id: 'env', manifest: paths[0], derivation: paths[0] + '.drv', system: 'aarch64-linux', pathCount: 205n }], nextOffset: null }
    if (type === R.RequestPayload.RunEnvironmentPathsRequest) {
      const found = paths.filter(p => p.includes(request.query ?? ''))
      const offset = Number(request.offset ?? 0)
      return { paths: found.slice(offset, offset + 100), total: BigInt(found.length), nextOffset: offset + 100 < found.length ? BigInt(offset + 100) : null }
    }
    throw Error('Unexpected RPC ' + type)
  },
}
Object.defineProperty(session, 'rpc', { get: () => rpc })
function Fixture() {
  const [editor, setEditor] = useState<{ card: string; category: CardEditorCategory }>()
  app.openWindow = (_s, what) => { if (what.kind === 'card') setEditor({ card: what.card, category: what.category ?? 'general' }) }
  return <>
    <div style={{ position: 'absolute', inset: 0 }}><GraphCanvas workspace={workspace} session={session} tab={tab} /></div>
    <div style={{ position: 'absolute', left: 20, top: 15, display: 'flex', gap: 10 }}>
      <NixReferenceControls session={session} tab={tab} />
      <button onClick={() => setEditor({ card: 'analysis', category: 'packages' })}>Card settings</button>
      <button onClick={() => { session.resolution.get('analysis')!.nixReferences = []; session.changed() }}>Remove reference</button>
      <button onClick={() => { fail = !fail }}>Toggle RPC failure</button>
    </div>
    {editor && <div style={{ position: 'absolute', inset: '80px 60px 30px', background: 'var(--text-bg)', border: '1px solid var(--separator)', borderRadius: 12, overflow: 'hidden' }}>
      <CardSettingsEditor key={`${editor.card}:${editor.category}`} session={session} card={editor.card} initial={editor.category} onClose={() => setEditor(undefined)} />
    </div>}
  </>
}
createRoot(document.getElementById('root')!).render(<Fixture />)
