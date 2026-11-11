// Manual browser fixture: real history components against deterministic paginated RPC responses.
import { createRoot } from 'react-dom/client'
import { useState } from 'react'
import { EnvironmentHistory } from '../src/ui/environmentHistory'
import { EnvironmentView } from '../src/ui/environments'
import { ProjectSession } from '../src/model/session'
import { R } from '../src/shared/rpc'
import '../src/styles.css'

const store = (name: string) => '/nix/store/' + 'a'.repeat(32) + '-' + name
const paths = Array.from({ length: 205 }, (_, i) => store(`package-${String(i).padStart(3, '0')}`))
let fail = false
const rpc = {
  async history() { return { runs: ['current', 'previous'].map((runId, i) => ({ status: { runId, acceptedAt: 1700000000000n + BigInt(i * 1000) } })), nextCursor: null } },
  async call(type: number, request: any) {
    await new Promise(r => setTimeout(r, request.query === 'slow' ? 800 : 30))
    if (fail) throw Error('Fixture connection failure')
    if (type === R.RequestPayload.RunLatestInvocationsRequest) return { records: [
      { card: { segments: ['spectrum'] }, environmentId: request.runId === 'current' ? 'new' : 'old' },
      { card: { segments: ['bands'] }, environmentId: 'new' }, { card: { segments: ['trigger'] }, environmentId: null },
    ] }
    if (type === R.RequestPayload.RunEnvironmentsRequest) return { environments: ['new', 'old'].map(id => ({ id, manifest: store(id + '-environment'), derivation: store(id + '.drv'), system: 'aarch64-linux', pathCount: 205n })), nextOffset: null }
    if (type === R.RequestPayload.RunEnvironmentPathsRequest) {
      let rows = request.excludeEnvironmentId ? (request.environmentId === request.excludeEnvironmentId ? [] : [store(request.environmentId + '-unique')]) : paths
      rows = rows.filter(p => p.toLowerCase().includes((request.query ?? '').toLowerCase()))
      const offset = Number(request.offset ?? 0), end = Math.min(offset + request.limit, rows.length)
      return { paths: rows.slice(offset, end), total: BigInt(rows.length), nextOffset: end < rows.length ? BigInt(end) : null }
    }
    throw Error('Unexpected RPC ' + type)
  },
}
const session = { rpc } as unknown as ProjectSession
function Fixture() {
  const [tab, setTab] = useState('detail')
  return <main style={{ width: 900, height: '100vh', margin: 'auto', overflow: 'auto', background: 'var(--control-bg)' }}>
    <h1>Environment UI fixture</h1>
    <button onClick={() => setTab('detail')}>Invocation</button><button onClick={() => setTab('run')}>Run overview</button>
    <button onClick={() => setTab('missing')}>Missing environment</button><label><input type="checkbox" onChange={e => { fail = e.target.checked }} />Fail requests</label>
    {tab === 'run' ? <EnvironmentHistory session={session} runId="current" /> : <EnvironmentView key={tab} session={session} runId="current" environmentId={tab === 'missing' ? '' : 'new'} />}
  </main>
}
createRoot(document.getElementById('root')!).render(<Fixture />)
