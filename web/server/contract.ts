// The web Agent's half of the Swift/web contract (tests/fixtures/): the tool
// catalog equals the snapshot both Agents are checked against, and FlatJSON
// and the Draft request give the shared cases' results. Run by
// scripts/contract.mjs; `--update` writes the tool snapshot instead.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { Builder, ByteBuffer } from 'flatbuffers'
import { BFBS } from '../src/shared/bfbs'
import { FlatJSON, InvalidArguments } from '../src/shared/flatjson'
import { AGENT, draftRequest } from '../src/shared/agentShared'
import os from 'node:os'
import { spawn } from 'node:child_process'
import { Agent, AgentEvent, approvalRequired, DEMO_SKILL, delegateTasks, helperOutside, messagesBody, ProviderConfig, readsLatestInstead, referenceDemos, responsesBody, sourcesText, ToolCatalog } from './agent'

type Json = null | boolean | number | string | Json[] | { [k: string]: Json }
interface Fixture {
  flatjson: { type: string; json: { [k: string]: Json }; expect?: { [k: string]: Json }; error?: boolean }[]
  draftRequest: { scope: string[]; cards: { key: string; definition: string; description: string; content: string; context: string }[]; expect: string }[]
  approvalRequired: { json: string; expect: string[] | null }[]
}

const [root, mode] = process.argv.slice(2)
const schema = new BFBS(new Uint8Array(fs.readFileSync(path.join(root, 'web/src/gen/tsugumi.bfbs'))))

const tools = new ToolCatalog(schema).tools.map((t) => ({
  name: t.name, payloadType: t.payloadType, description: t.description, inputSchema: t.inputSchema, mutating: t.mutating,
}))
const snapshot = path.join(root, 'tests/fixtures/agent-tools.json')
if (mode === '--update') {
  fs.writeFileSync(snapshot, JSON.stringify(tools, null, 1) + '\n')
  console.log(`wrote ${snapshot}`)
} else {
  assert.deepEqual(tools, JSON.parse(fs.readFileSync(snapshot, 'utf8')),
    'the tool catalog differs from tests/fixtures/agent-tools.json (run `npm run contract -- --update` after an RPC change)')
}

const fixture = JSON.parse(fs.readFileSync(path.join(root, 'tests/fixtures/agent-contract.json'), 'utf8')) as Fixture
const codec = new FlatJSON(schema)
for (const c of fixture.flatjson) {
  const object = schema.object(c.type)
  const b = new Builder(256)
  const label = `${c.type} ${JSON.stringify(c.json)}`
  if (c.error) {
    assert.throws(() => codec.build(object, c.json, b), InvalidArguments, label)
    continue
  }
  b.finish(codec.build(object, c.json, b))
  const bb = new ByteBuffer(b.asUint8Array())
  const out = codec.json(object, bb, bb.readInt32(bb.position()) + bb.position())
  for (const [k, v] of Object.entries(c.expect ?? {})) assert.deepEqual(out[k], v, `${label}: ${k} is ${JSON.stringify(out[k])}`)
}
for (const c of fixture.draftRequest) assert.equal(draftRequest(c.scope, c.cards), c.expect)
for (const c of fixture.approvalRequired) assert.deepEqual(approvalRequired(c.json) ?? null, c.expect, c.json)
// The Draft demos are tasks and the finished demo of each is its answer:
// reference_read neither lists nor reads them (as the Swift Agent).
{
  const refs = referenceDemos(path.join(root, 'demos'))
  const listed = Object.keys(refs).sort()
  for (const example of ['demos/collatz', 'demos/live-stream', 'demos/plots', 'demos/report']) assert.ok(listed.includes(example), example)
  for (const task of ['demos/casa', 'demos/casa-draft', 'demos/gw150914', 'demos/dynamics-draft', 'demos/mnist-classifiers', 'demos/mnist-classifiers-draft']) assert.ok(!listed.includes(task), task)
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-ref-'))
  const agent = new Agent(new ToolCatalog(schema), async () => { throw new Error('no rpc') },
    { name: 'none', baseURL: 'http://127.0.0.1:1', model: 'none', auth: 'none' } as never, path.join(tmp, 'c.json'), 'c', refs)
  assert.deepEqual(agent.readReference('demos'), [listed.map((p) => p.slice('demos/'.length) + '/').join('\n'), false])
  assert.equal(agent.readReference('demos/report/tsugumi.json')[1], false)
  for (const hidden of ['demos/casa', 'demos/casa/definitions/bands/src/main.py', 'demos/casa-draft', 'demos/../core',
    'demos/mnist-classifiers', 'demos/mnist-classifiers/definitions/cnn/src/main.py',
    'demos/mnist-classifiers-draft', 'demos/mnist-classifiers-draft/definitions/load/src/main.py']) {
    assert.equal(agent.readReference(hidden)[1], true, `${hidden} is readable`)
  }
  fs.rmSync(tmp, { recursive: true, force: true })
}
// delegate: the tasks are checked before any helper starts, and a helper
// saves only its own Cards (as the Swift Agent).
{
  const ok = delegateTasks('{"tasks":[{"name":"a","cards":["a"],"instruction":"x"},{"name":"b","cards":["b","c"],"instruction":"y"}]}')
  assert.ok(Array.isArray(ok) && ok.length === 2)
  for (const bad of ['{"tasks":[{"name":"a","cards":["a"],"instruction":"x"}]}',
    '{"tasks":[{"name":"a","cards":["a"],"instruction":"x"},{"name":"b","cards":["a"],"instruction":"y"}]}',
    '{"tasks":[{"name":"a/..","cards":["a"],"instruction":"x"},{"name":"b","cards":["b"],"instruction":"y"}]}',
    '{"tasks":[{"name":"a","cards":["x/y"],"instruction":"x"},{"name":"b","cards":["b"],"instruction":"y"}]}']) {
    assert.equal(typeof delegateTasks(bad), 'string', bad)
  }
  const h = { name: 'lorenz', cards: ['lorenz'], instruction: '' }
  assert.equal(helperOutside(h, [{ value_type: 'PutDefinition', value: { id: 'lorenz' } },
    { value_type: 'PutSource', value: { path: 'definitions/lorenz/src/main.py' } }]), undefined)
  assert.equal(helperOutside(h, [{ value_type: 'PutDefinition', value: { id: 'life' } }]), 'the definition life')
  assert.equal(helperOutside(h, [{ value_type: 'SetProject', value: { json: '{}' } }]), 'SetProject')
  assert.ok(helperOutside(h, [{ value_type: 'PutSource', value: { path: 'definitions/lorenz/../life/x' } }]))
}
// The guide only reads and offers buttons (as the Swift Agent's Role.guide).
{
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-guide-'))
  const catalog = new ToolCatalog(schema)
  const g = new Agent(catalog, async () => { throw new Error('no rpc') },
    { name: 'none', baseURL: 'http://127.0.0.1:1', model: 'none', needsKey: false } as never, path.join(tmp, 'g.json'), 'g', {}, undefined, 'guide')
  const offered = g.tools.map((t) => t.name)
  assert.deepEqual(offered, [...AGENT.guide.tools, 'guide_show'])
  for (const t of AGENT.guide.tools) assert.equal(catalog.tool(t)?.mutating, false, `${t} changes the project`)
  assert.ok(g.system.startsWith('You are Tsugumi, the guide bird'))
  // The chat's Agent is the same Tsugumi: it can fly and offer choices too.
  const chat = new Agent(catalog, async () => { throw new Error('no rpc') },
    { name: 'none', baseURL: 'http://127.0.0.1:1', model: 'none', needsKey: false } as never, path.join(tmp, 'c.json'), 'c', {})
  assert.ok(chat.tools.some((t) => t.name === 'guide_show') && chat.system.includes('only while the demo skill'))
  // Its guiding is the built-in skill $demo, started by a tour or the user.
  assert.ok(DEMO_SKILL.startsWith('---\nname: demo\n') && DEMO_SKILL.includes('"endTour": true'))
  fs.rmSync(tmp, { recursive: true, force: true })
}
// A read refused for naming a revision gone by goes again at the latest; only a read, and only once.
{
  const args: Record<string, unknown> = { at_rev: '0' }
  assert.equal(readsLatestInstead(true, 'revision_conflict', { ...args }), false, 'a save keeps its revision')
  assert.equal(readsLatestInstead(false, 'busy', { ...args }), false)
  assert.equal(readsLatestInstead(false, 'revision_conflict', args), true)
  assert.deepEqual(args, {})
  assert.equal(readsLatestInstead(false, 'revision_conflict', args), false, 'nothing more to take away')
}

// Web search is the provider's own tool: off, the model never hears of it; on,
// a search shows like any tool, its sources can be followed, and its blocks go
// back to the API as they came (the fixture refuses them otherwise). The same
// cases as macOS/CoreTests (WebSearchTests).
{
  const fixture = spawn('python3', [path.join(root, 'tests/fixtures/model_fixture.py'), '--port', '0'], { stdio: ['ignore', 'pipe', 'inherit'] })
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-search-'))
  try {
    const base = await new Promise<string>((resolve, reject) => {
      fixture.stdout!.on('data', (d: Buffer) => {
        const m = /http:\/\/127\.0\.0\.1:\d+/.exec(String(d))
        if (m) resolve(m[0])
      })
      fixture.once('exit', () => reject(new Error('the fixture ended')))
    })
    const requests = async () => (await (await fetch(`${base}/fixture/requests`)).json()) as { path: string; tools: string[]; include: string[] | null; names_web_search: boolean }[]
    const catalog = new ToolCatalog(schema)
    const noRpc = async () => { throw new Error('no rpc in this check') }
    const provider = (webSearch: boolean, api: ProviderConfig['api'] = 'anthropicMessages'): ProviderConfig => ({
      name: 'Test', baseURL: base, model: 'fixture', api, auth: 'apiKey', apiKey: 'fixture-key', maxTokens: 32768, contextLimit: 180_000, webSearch })
    let serial = 0
    const agentWith = (p: ProviderConfig) => { serial++; return new Agent(catalog, noRpc, p, path.join(tmp, `${serial}.json`), `c${serial}`) }
    const turn = (a: Agent, text: string) => new Promise<AgentEvent[]>((resolve) => {
      const events: AgentEvent[] = []
      a.send(text, text, (e) => {
        events.push(e)
        if (e.kind === 'finished' || e.kind === 'error' || e.kind === 'stepLimit') resolve(events)
      })
    })
    const says = (events: AgentEvent[]) => events.flatMap((e) => (e.kind === 'text' ? [e.text] : [])).join('')
    const offered = (a: Agent) => a.tools.map((t) => t.name)
    const finished = (events: AgentEvent[]) => assert.equal(events[events.length - 1].kind, 'finished', JSON.stringify(events[events.length - 1]))

    // Off (the default): no tool, not a word about it in the request.
    const off = agentWith({ ...provider(false), webSearch: undefined })
    assert.ok(!offered(off).includes('web_search'))
    assert.ok(!off.system.includes('web_search'))
    let events = await turn(off, 'search the web for tsugumi')
    finished(events)
    assert.match(says(events), /no way to search/)
    assert.equal((await requests()).at(-1)?.names_web_search, false, 'off, the request names web_search')

    // On, but not for the local fixture (it has no search), the guide or a helper.
    const none: ProviderConfig = { name: 'Local fixture', baseURL: base, model: 'fixture', api: 'anthropicMessages', auth: 'none', maxTokens: 32768, contextLimit: 6000, webSearch: true }
    assert.ok(!offered(agentWith(none)).includes('web_search'))
    assert.ok(!offered(new Agent(catalog, noRpc, provider(true), path.join(tmp, 'g.json'), 'g', {}, undefined, 'guide')).includes('web_search'))
    assert.ok(!offered(new Agent(catalog, noRpc, provider(true), path.join(tmp, 'h.json'), 'h', {}, { name: 'h', cards: ['h'], instruction: '' })).includes('web_search'))

    // On: the tool list changes, and the search is the provider's.
    const on = agentWith(provider(true))
    assert.equal(offered(on)[0], 'web_search')
    assert.equal(offered(on).length, offered(off).length + 1)
    assert.ok(on.system.includes('web_search'), 'the rules say what a query may not hold')
    const body = messagesBody(provider(true), on.system, [{ role: 'user', content: [{ type: 'text', text: 'hi' }] }], on.tools) as { tools: Record<string, unknown>[] }
    assert.deepEqual(body.tools[0], { type: AGENT.webSearch.anthropicType, name: 'web_search', max_uses: AGENT.webSearch.maxUses })
    assert.ok(body.tools[body.tools.length - 1].cache_control, 'the last tool ends the cached prefix')

    events = await turn(on, 'search the web for tsugumi')
    finished(events)
    assert.deepEqual((await requests()).at(-1)?.tools[0], 'web_search')
    const started = events.find((e) => e.kind === 'toolStarted')
    assert.ok(started?.kind === 'toolStarted' && started.name === 'web_search' && JSON.parse(started.arguments).query === 'tsugumi', JSON.stringify(started))
    const done = events.find((e) => e.kind === 'toolFinished')
    assert.ok(done?.kind === 'toolFinished' && done.ok && done.summary.includes('https://example.com/1'), JSON.stringify(done))
    assert.match(says(events), /\[Result 1 on tsugumi\]\(https:\/\/example\.com\/1/, 'the cited source is a link')
    const record = on.conversation.tools.find((t) => t.name === 'web_search')
    assert.ok(record?.state === 'completed' && record.result?.includes('https://example.com/2') && JSON.parse(record.arguments).query === 'tsugumi', JSON.stringify(record))
    const reply = on.conversation.messages[1]
    assert.ok(reply.content.some((b) => b.type === 'web_search' && b.sources.length === 2), JSON.stringify(reply.content))
    assert.ok(JSON.stringify(reply.providerContent).includes('enc-1'), 'the reply is kept as the provider sent it')
    // The next request sends it back (the fixture refuses it changed or left out).
    finished(await turn(on, 'thanks'))
    const again = messagesBody(provider(true), on.system, on.conversation.messages, on.tools) as { messages: { content: Record<string, unknown>[] }[] }
    assert.ok(JSON.stringify(again.messages[1].content).includes('encrypted_index'), JSON.stringify(again.messages[1]))
    // A saved conversation reads back with it.
    assert.deepEqual(new Agent(catalog, noRpc, provider(true), on.storage, on.id).conversation.messages[1], reply)

    // Turned off later: the tool and its blocks leave the request, the fact of the search stays.
    on.setProvider(provider(false))
    assert.ok(!offered(on).includes('web_search'))
    finished(await turn(on, 'thanks again'))
    assert.equal((await requests()).at(-1)?.names_web_search, false, 'turned off, the request names web_search')
    const later = messagesBody(provider(false), on.system, on.conversation.messages, on.tools) as { messages: { content: { text?: string }[] }[] }
    assert.ok(later.messages[1].content.some((b) => b.text?.includes('[web search: tsugumi')), JSON.stringify(later.messages[1]))

    // pause_turn: the paused reply goes back unchanged, with no message added, and the turn goes on.
    const paused = agentWith(provider(true))
    events = await turn(paused, 'search the web for tsugumi with a pause')
    finished(events)
    assert.equal(paused.conversation.messages.filter((m) => m.role === 'user').length, 1)
    assert.ok(events.some((e) => e.kind === 'toolFinished' && e.name === 'web_search' && e.ok), JSON.stringify(events))
    finished(await turn(paused, 'thanks'))

    // A search called beside a tool of ours runs once that tool's result is back.
    const mixed = agentWith(provider(true))
    events = await turn(mixed, 'search the web for tsugumi alongside inspect')
    finished(events)
    assert.ok(mixed.conversation.messages.flatMap((m) => m.content).some((b) => b.type === 'web_search' && b.sources.length === 2), JSON.stringify(mixed.conversation.messages))
    assert.ok(mixed.conversation.tools.some((t) => t.name === 'web_search' && t.state === 'completed'))
    finished(await turn(mixed, 'thanks'))

    // A search left open by a stopped turn cannot go back as it is (the API refuses it): it goes in words.
    const stopped = agentWith(provider(true))
    stopped.conversation.messages.push(
      { role: 'user', content: [{ type: 'text', text: 'go' }] },
      { role: 'assistant', content: [{ type: 'web_search', id: 'srvtoolu_open', query: 'open', sources: [] }],
        providerContent: [{ type: 'server_tool_use', id: 'srvtoolu_open', name: 'web_search', input: { query: 'open' } }] })
    finished(await turn(stopped, 'thanks'))

    // The Responses API: its hosted tool, with the sources asked for.
    const responses = agentWith(provider(true, 'openAIResponses'))
    assert.ok(offered(responses).includes('web_search'))
    events = await turn(responses, 'search the web for tsugumi')
    finished(events)
    const seen = (await requests()).at(-1)
    assert.ok(seen?.tools.includes('web_search') && seen.include?.includes('web_search_call.action.sources'), JSON.stringify(seen))
    assert.ok(events.some((e) => e.kind === 'toolFinished' && e.name === 'web_search' && e.summary.includes('https://example.com/2')), JSON.stringify(events))
    assert.match(says(events), /\[Result 1 on tsugumi\]\(https:\/\/example\.com\/1/)
    // Its history carries the search in words (no call item: nothing of the results could come back).
    finished(await turn(responses, 'thanks'))
    const input = responsesBody(provider(true, 'openAIResponses'), responses.system, responses.conversation.messages, responses.tools).input as unknown[]
    assert.ok(JSON.stringify(input).includes('[web search: tsugumi') && !JSON.stringify(input).includes('web_search_call'), JSON.stringify(input))
    finished(await turn(agentWith(provider(false, 'openAIResponses')), 'search the web for tsugumi'))
    assert.equal((await requests()).at(-1)?.names_web_search, false)

    // The ChatGPT plan: the hosted tool beside the namespace, and no include (not known to be accepted).
    const plan: ProviderConfig = { ...provider(true, 'openAIResponses'), auth: 'chatgpt', apiKey: undefined, token: async () => 'token' }
    const planBody = responsesBody(plan, 'rules', [], agentWith(plan).tools)
    assert.deepEqual((planBody.tools as { type: string }[]).map((t) => t.type), ['namespace', 'web_search'])
    assert.equal(planBody.include, undefined)
    assert.ok(!((responsesBody(provider(false, 'openAIResponses'), 'rules', [], agentWith(provider(false, 'openAIResponses')).tools).tools as { type: string }[]).some((t) => t.type === 'web_search')))

    // Sources come from the web: only http(s) links, and a title cannot break out of its link.
    const text = sourcesText([{ url: 'javascript:alert(1)', title: 'x' }, { url: 'https://a.example/b)c', title: 'a]b\nc' }, { url: 'https://a.example/b)c' }])
    assert.ok(!text.includes('javascript:'), text)
    assert.equal(text.match(/https:/g)?.length, 1, text)
    assert.ok(text.includes('[a b c](https://a.example/b%29c)'), text)
    assert.equal(sourcesText([{ url: 'file:///etc/passwd' }]), '')
  } finally {
    fixture.kill()
    fs.rmSync(tmp, { recursive: true, force: true })
  }
}

console.log(`contract: ${tools.length} tools, ${fixture.flatjson.length} FlatJSON cases, ${fixture.draftRequest.length} Draft requests`)
