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
import { AGENT, commentRequest, draftRequest, modelChoices, modelOptions } from '../src/shared/agentShared'
import os from 'node:os'
import { ChildProcess, spawn } from 'node:child_process'
import { Agent, AgentEvent, approvalRequired, CONTEXT_FULL_TEXT, DEMO_SKILL, delegateTasks, helperOutside, messagesBody, ProviderConfig, readsLatestInstead, referenceDemos, responsesBody, sourcesText, ToolCatalog } from './agent'

type Json = null | boolean | number | string | Json[] | { [k: string]: Json }
interface Fixture {
  flatjson: { type: string; json: { [k: string]: Json }; expect?: { [k: string]: Json }; error?: boolean }[]
  draftRequest: { scope: string[]; cards: { key: string; definition: string; description: string; content: string; context: string }[]; expect: string }[]
  commentRequest: { scope: string[]; cards: { key: string; definition: string; comments: { id: string; text: string }[] }[]; expect: string }[]
  approvalRequired: { json: string; expect: string[] | null }[]
  modelOptions: { connection: string; model: string; effort: string; fastModel: string
    expect: { efforts: string[]; default: string | null; fastPrice: number | null; present: Record<string, unknown>; absent: string[] } }[]
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
for (const c of fixture.commentRequest) assert.equal(commentRequest(c.scope, c.cards), c.expect)
for (const c of fixture.approvalRequired) assert.deepEqual(approvalRequired(c.json) ?? null, c.expect, c.json)
// What the model menu offers, and what its choices put into a request.
for (const c of fixture.modelOptions) {
  const label = `${c.connection} ${c.model} ${JSON.stringify([c.effort, c.fastModel])}`
  const o = modelOptions(c.connection, c.model)
  assert.deepEqual([o.efforts, o.defaultEffort ?? null, o.fastPrice ?? null], [c.expect.efforts, c.expect.default, c.expect.fastPrice], label)
  const wire = c.connection === 'openai' || c.connection === 'chatgpt' ? 'openAIResponses' as const : 'anthropicMessages' as const
  const config: ProviderConfig = { name: 't', baseURL: 'http://127.0.0.1:1', model: c.model, api: wire, auth: c.connection === 'chatgpt' ? 'chatgpt' : c.connection === 'fixture' ? 'none' : 'apiKey',
    maxTokens: 32768, contextLimit: 180_000, ...modelChoices(c.connection, c.model, c.effort, c.fastModel) }
  const body = (wire === 'openAIResponses' ? responsesBody : messagesBody)(config, 'rules', [], [])
  for (const [k, v] of Object.entries(c.expect.present)) assert.deepEqual(body[k], v, `${label}: ${k}`)
  for (const k of c.expect.absent) assert.ok(!(k in body), `${label}: ${k} must not be sent`)
}
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
  // How the helpers meet: each by default, together when asked, nothing else.
  const tasks = '"tasks":[{"name":"a","cards":["a"],"instruction":"x"},{"name":"b","cards":["b"],"instruction":"y"}]'
  const met = (json: string) => { const r = delegateTasks(json); return typeof r === 'string' ? r : r.map((t) => t.runsTogether) }
  assert.deepEqual(met(`{${tasks}}`), [false, false])
  assert.deepEqual(met(`{"runs":"each",${tasks}}`), [false, false])
  assert.deepEqual(met(`{"runs":"together",${tasks}}`), [true, true])
  assert.equal(typeof met(`{"runs":"sometimes",${tasks}}`), 'string')
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
    const requests = async () => (await (await fetch(`${base}/fixture/requests`)).json()) as { path: string; tools: string[]; include: string[] | null; names_web_search: boolean; effort: string | null; speed: string | null; output_limit: number }[]
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

    // The chat's choices go with every step of its Agent; a compaction's summary carries neither.
    for (const api of ['anthropicMessages', 'openAIResponses'] as const) {
      const chosen = agentWith({ ...provider(false, api), effort: 'max', fast: true })
      for (const said of ['one', 'two', 'three']) finished(await turn(chosen, said))
      const asked = (await requests()).at(-1)
      assert.deepEqual([asked?.effort, asked?.speed, asked?.output_limit], ['max', 'fast', 64000], JSON.stringify(asked))
      await new Promise<void>((resolve) => chosen.compact((e) => { if (e.kind === 'compacted' || e.kind === 'finished' || e.kind === 'error') resolve() }))
      const summary = (await requests()).at(-1)
      assert.deepEqual([summary?.effort, summary?.speed, summary?.output_limit], [null, null, 32768], JSON.stringify(summary))
    }

    // Sources come from the web: only http(s) links, and a title cannot break out of its link.
    const text = sourcesText([{ url: 'javascript:alert(1)', title: 'x' }, { url: 'https://a.example/b)c', title: 'a]b\nc' }, { url: 'https://a.example/b)c' }])
    assert.ok(!text.includes('javascript:'), text)
    assert.equal(text.match(/https:/g)?.length, 1, text)
    assert.ok(text.includes('[a b c](https://a.example/b%29c)'), text)
    assert.equal(sourcesText([{ url: 'file:///etc/passwd' }]), '')

    // A reply with nothing in it (declined before any output, or only thinking,
    // which is not kept): the turn says so, and the conversation can go on. The
    // fixture refuses empty content as the Messages API does. The same cases as
    // macOS/CoreTests (EmptyReplyTests).
    const saysEmpty = (events: AgentEvent[], stop: string) => {
      const last = events[events.length - 1]
      assert.ok(last.kind === 'error' && last.message.includes('empty reply') && last.message.includes(stop), JSON.stringify(last))
    }
    for (const [asked, stop] of [['refuse this', 'refusal'], ['think only', 'end_turn']]) {
      const a = agentWith(provider(false))
      saysEmpty(await turn(a, asked), stop)
      assert.ok(a.conversation.messages.every((m) => m.content.length > 0), JSON.stringify(a.conversation.messages))
      // Nothing is left to resume: opening the conversation asks nothing again.
      assert.equal(a.conversation.messages.at(-1)?.role, 'assistant')
      // The next request is not refused for the empty reply.
      events = await turn(a, 'thanks')
      finished(events)
      assert.match(says(events), /set <card>/)
    }
    // Retry asks the same again; it may be answered the same, and says so again.
    const refused = agentWith(provider(false))
    saysEmpty(await turn(refused, 'refuse this'), 'refusal')
    saysEmpty(await new Promise<AgentEvent[]>((resolve) => {
      const seen: AgentEvent[] = []
      refused.retry((e) => {
        seen.push(e)
        if (e.kind === 'finished' || e.kind === 'error') resolve(seen)
      })
    }), 'refusal')
    // A reply over the Responses API whose only output is a refusal has no text either.
    const declined = agentWith(provider(false, 'openAIResponses'))
    saysEmpty(await turn(declined, 'refuse this'), 'end_turn')
    finished(await turn(declined, 'thanks'))
  } finally {
    fixture.kill()
    fs.rmSync(tmp, { recursive: true, force: true })
  }
}

// The context limit, as in macOS/CoreTests (ContextLimitTests): the fixture refuses
// a request over it as the APIs do. Whichever way the context runs out, the
// conversation goes on, or the Agent says what to do.
{
  const start = async (...args: string[]) => {
    const p = spawn('python3', [path.join(root, 'tests/fixtures/model_fixture.py'), '--port', '0', ...args], { stdio: ['ignore', 'pipe', 'inherit'] })
    const base = await new Promise<string>((resolve, reject) => {
      p.stdout!.on('data', (d: Buffer) => {
        const m = /http:\/\/127\.0\.0\.1:\d+/.exec(String(d))
        if (m) resolve(m[0])
      })
      p.once('exit', () => reject(new Error('the fixture ended')))
    })
    return { p, base }
  }
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'tsugumi-limit-'))
  const big = path.join(tmp, 'big.txt')
  const running: ChildProcess[] = []
  let serial = 0
  const catalog = new ToolCatalog(schema)
  const agentAt = (base: string, api: ProviderConfig['api'] = 'anthropicMessages', file?: string) => new Agent(catalog, async () => { throw new Error('no rpc in this check') },
    { name: 'Test', baseURL: base, model: 'fixture', api, auth: 'apiKey', apiKey: 'fixture-key', maxTokens: 32768, contextLimit: 6000 },
    file ?? path.join(tmp, `${++serial}.json`), `limit${serial}`, { 'big.txt': big })
  const turn = (a: Agent, text: string) => new Promise<AgentEvent[]>((resolve) => {
    const events: AgentEvent[] = []
    a.send(text, text, (e) => {
      events.push(e)
      if (e.kind === 'finished' || e.kind === 'error' || e.kind === 'stepLimit') resolve(events)
    })
  })
  const says = (events: AgentEvent[]) => events.flatMap((e) => (e.kind === 'text' ? [e.text] : [])).join('')
  const ended = (events: AgentEvent[]) => assert.equal(events[events.length - 1].kind, 'finished', JSON.stringify(events[events.length - 1]))
  const fixtureAt = async (...args: string[]) => {
    const { p, base } = await start(...args)
    running.push(p)
    return base
  }
  const earlier = (usage: number, first = 'question') => {
    const file = path.join(tmp, `${++serial}.json`)
    const messages = []
    for (let i = 0; i < 4; i++) messages.push({ role: 'user', content: [{ type: 'text', text: i ? `question ${i}` : first }] }, { role: 'assistant', content: [{ type: 'text', text: `answer ${i}` }] })
    fs.writeFileSync(file, JSON.stringify({ id: `limit${serial}`, updatedAt: 0, messages, archive: [], tools: [], usage: { inputTokens: usage, outputTokens: 0 } }))
    return file
  }
  try {
    fs.writeFileSync(big, 'x'.repeat(20_000))
    const limited = await fixtureAt('--context-limit', '6000')

    // Ten reads in one reply are more than the next request can take, and nothing
    // can be summarized yet: the results are cut, the whole stays in the records.
    let a = agentAt(limited)
    let events = await turn(a, 'read big.txt 10 times')
    ended(events)
    assert.match(says(events), /Read 10 results, 10 cut\./)
    assert.ok(a.conversation.tools.every((t) => t.result?.length === 20_000))
    ended(await turn(a, 'and now?'))

    // Earlier turns to summarize are not enough: the reads stay in what is kept.
    a = agentAt(limited)
    for (const t of ['one', 'two', 'three']) await turn(a, t)
    events = await turn(a, 'read big.txt 10 times')
    ended(events)
    assert.ok(events.some((e) => e.kind === 'compacted'))
    assert.ok(a.conversation.summary && a.conversation.archive.length)
    ended(await turn(a, 'and now?'))

    // The same over the Responses API, where the refusal comes in the stream.
    a = agentAt(limited, 'openAIResponses')
    events = await turn(a, 'read big.txt 10 times')
    ended(events)
    assert.match(says(events), /Read 10 results, 10 cut\./)

    // A message that cannot fit says what to do, and a new conversation works.
    a = agentAt(limited)
    events = await turn(a, 'y'.repeat(100_000))
    assert.deepEqual(events.at(-1), { kind: 'error', message: CONTEXT_FULL_TEXT })
    ended(await turn(agentAt(limited), 'hello'))

    // The request for the summary fits the context too.
    a = agentAt(limited, 'anthropicMessages', earlier(5000, 'z'.repeat(60_000)))
    ended(await turn(a, 'hello'))
    assert.ok(a.conversation.summary)

    // What compaction keeps is under the threshold, so it does not start again at each step.
    const free = await fixtureAt()
    fs.writeFileSync(big, 'x'.repeat(15_000))
    a = agentAt(free)
    events = await turn(a, 'read big.txt 2 times in 3 rounds')
    ended(events)
    assert.ok(events.some((e) => e.kind === 'compacted'))
    ended(await turn(a, 'and now?'))
    assert.ok(a.conversation.usage.inputTokens <= 4500, String(a.conversation.usage.inputTokens))

    // A reply ended at the context window (HTTP 200, its own stop reason) is not an answer: the work goes on.
    events = await turn(agentAt(await fixtureAt('--window-cut-once')), 'read big.txt 1 times')
    ended(events)
    assert.match(says(events), /Read 1 results/, JSON.stringify(events))

    // A summary that fails over the threshold does not end the turn: the model still takes the request.
    a = agentAt(await fixtureAt('--fail-summaries', '99'), 'anthropicMessages', earlier(5000))
    events = await turn(a, 'hello')
    ended(events)
    assert.ok(events.some((e) => e.kind === 'retrying' && e.message.includes('summar')), JSON.stringify(events))
  } finally {
    for (const p of running) p.kill()
    fs.rmSync(tmp, { recursive: true, force: true })
  }
}

console.log(`contract: ${tools.length} tools, ${fixture.flatjson.length} FlatJSON cases, ${fixture.draftRequest.length} Draft requests`)
