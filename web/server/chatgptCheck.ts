// Sign in with ChatGPT in the web server, against the local model fixture
// (tests/fixtures/model_fixture.py is also the OAuth issuer): the sign-in
// with its loopback callback and ID token check, the 0600 record, the web
// Agent on the plan (the fixture refuses requests that break the plan's
// rules), refreshes of one-second access tokens, the usage limit and the
// sign-out. The same flow as macOS/CoreTests (chatGPTPlanSignInRunRefreshLimitSignOut).
// Run by `node scripts/contract.mjs chatgpt` (npm run check:chatgpt).
import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { BFBS } from '../src/shared/bfbs'
import { Agent, AgentEvent, listModels, ProviderConfig, ToolCatalog } from './agent'
import { ChatGPTAuth, ChatGPTError, endpointsFor, USAGE_LIMIT_TEXT } from './chatgpt'

const [root] = process.argv.slice(2)
const fixture = spawn('python3', [path.join(root, 'tests/fixtures/model_fixture.py'), '--port', '0', '--chatgpt-expires', '1'], { stdio: ['ignore', 'pipe', 'inherit'] })
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-chatgpt-'))
try {
  const issuer = await new Promise<string>((resolve, reject) => {
    fixture.stdout!.on('data', (d: Buffer) => {
      const m = /http:\/\/127\.0\.0\.1:\d+/.exec(String(d))
      if (m) resolve(m[0])
    })
    fixture.once('exit', () => reject(new Error('the fixture ended')))
  })
  const stats = async () => (await (await fetch(`${issuer}/fixture/chatgpt`)).json()) as { refreshes: number; revoked: number; violations: string[]; plan_requests: number; authorize: Record<string, unknown>[] }
  const file = path.join(dir, 'chatgpt.json')
  const auth = new ChatGPTAuth(file, endpointsFor(issuer, 'https://api.openai.com/v1'))

  // The browser's part: follow the authorize redirect to the loopback callback.
  const { url, done } = await auth.begin()
  assert.equal((await fetch(url)).status, 200)
  const r = await done
  assert.ok(r.welcome && r.account.planEnabled && r.account.email === 'fixture@example.com', JSON.stringify(r))
  assert.equal(fs.statSync(file).mode & 0o777, 0o600)
  const first = (await stats()).authorize[0]
  assert.equal(first.client_id, 'dynamic_agent_client')
  assert.equal(first.agent_name_hint, 'Tsugumi')
  assert.match(String(first.ext_agent_host_id), /^urn:uuid:/)
  await auth.markWelcomed(r.account.clientId)

  const provider: ProviderConfig = { name: 'ChatGPT', baseURL: issuer, model: 'fixture', api: 'openAIResponses', auth: 'chatgpt',
    token: (stale) => auth.accessToken(stale), maxTokens: 32768, contextLimit: 180_000 }
  assert.deepEqual(await listModels(provider), [{ id: 'fixture', label: 'Fixture model' }])

  const schema = new BFBS(new Uint8Array(fs.readFileSync(path.join(root, 'web/src/gen/tsugumi.bfbs'))))
  // No tsugumid: every tool call fails, so each step sends the history back with its calls.
  const agent = new Agent(new ToolCatalog(schema), async () => { throw new Error('no rpc in this check') }, provider, path.join(dir, 'c.json'), 'c')
  const turn = (prompt: string) => new Promise<AgentEvent[]>((resolve) => {
    const seen: AgentEvent[] = []
    agent.send(prompt, 'check', (e) => {
      seen.push(e)
      if (e.kind === 'finished' || e.kind === 'error' || e.kind === 'stepLimit') resolve(seen)
    })
  })
  // A tool call and its failure, then the answer: the second request repeats the call in its namespace.
  const steps = await turn('set src.text to "x" and run, then report out')
  assert.ok(steps.some((e) => e.kind === 'toolFinished') && steps.some((e) => e.kind === 'finished'), JSON.stringify(steps))
  let s = await stats()
  assert.deepEqual(s.violations, [])
  assert.ok(s.plan_requests >= 2 && s.refreshes >= 2, JSON.stringify(s))

  // Web search on the plan: the hosted tool beside the namespace keeps the plan's rules.
  const searching = new Agent(agent.catalog, async () => { throw new Error('no rpc in this check') }, { ...provider, webSearch: true }, path.join(dir, 's.json'), 's')
  const found = await new Promise<AgentEvent[]>((resolve) => {
    const seen: AgentEvent[] = []
    searching.send('search the web for tsugumi', 'check', (e) => {
      seen.push(e)
      if (e.kind === 'finished' || e.kind === 'error' || e.kind === 'stepLimit') resolve(seen)
    })
  })
  assert.ok(found.some((e) => e.kind === 'toolFinished' && e.name === 'web_search') && found.at(-1)?.kind === 'finished', JSON.stringify(found))
  assert.deepEqual((await stats()).violations, [])

  const limited = await turn('now exceed the usage limit')
  assert.ok(limited.some((e) => e.kind === 'error' && e.message === USAGE_LIMIT_TEXT), JSON.stringify(limited))
  assert.ok(!limited.some((e) => e.kind === 'retrying'))

  // Signing in again to the saved registration keeps its client and host.
  const again = await auth.begin(r.account.clientId)
  await fetch(again.url)
  await again.done
  const last = (await stats()).authorize.at(-1)!
  assert.ok(last.client_id === r.account.clientId && last.agent_name_hint === undefined && last.id_token_hint === true, JSON.stringify(last))
  assert.equal(last.ext_agent_host_id, first.ext_agent_host_id)

  assert.equal(await auth.signOut(r.account.clientId), true)
  assert.deepEqual(auth.accounts().map((a) => a.signedIn), [false])
  await assert.rejects(auth.accessToken(), ChatGPTError)
  const record = fs.readFileSync(file, 'utf8')
  assert.ok(!record.includes('chatgpt-access-') && !record.includes('chatgpt-refresh-'))
  s = await stats()
  assert.equal(s.revoked, 1)
  console.log(`chatgpt: signed in, ${s.plan_requests} plan requests, ${s.refreshes} refreshes, usage limit, signed out`)
} finally {
  fixture.kill()
  fs.rmSync(dir, { recursive: true, force: true })
}
