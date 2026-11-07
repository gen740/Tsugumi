// Tsugumi without a model connection (agent/Shared/guide-script.json): the
// script is whole (every choice and every run's end leads to one of its
// steps, every Card it points at is in the demo), anything typed gets the
// reply that points to Settings, and nothing is sent to a model. The macOS
// app's GuideTests check the same script.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import script from '../../agent/Shared/guide-script.json'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { guide } = await import('../src/model/guide')
const { app } = await import('../src/model/app')
const { noConnection, scriptStep } = await import('../src/model/guideScript')
const { D } = await import('../src/shared/rpc')
const root = process.argv[2]

// The script is whole.
{
  const steps = script.steps as Record<string, { show: { actions?: unknown[] }; next?: Record<string, string>; ran?: Record<string, string> }>
  const demo = JSON.parse(fs.readFileSync(path.join(root, 'demos/report/tsugumi.json'), 'utf8'))
  const cards = new Set(demo.canvas.cards.map((c: { id: string }) => c.id))
  for (const id of ['interest', 'start', 'resume']) assert.ok(steps[id], id)
  for (const [id, raw] of Object.entries(steps)) {
    const s = scriptStep(id)!
    assert.deepEqual(Object.keys(raw.next ?? {}).sort(), [...s.show.choices].sort(), `${id}: every choice leads somewhere`)
    for (const n of [...Object.values(raw.next ?? {}), ...Object.values(raw.ran ?? {})]) assert.ok(steps[n], `${id} → ${n}`)
    assert.equal(s.show.actions.length, raw.show.actions?.length ?? 0, `${id}: every button is one the app has`)
    const h = s.show.highlight
    if (h?.startsWith('card:')) assert.ok(cards.has(h.slice(5).split('/')[0]), `${id}: ${h} is in the demo`)
  }
  assert.equal(scriptStep('farewell')?.show.endTour, true)
  assert.deepEqual(noConnection().show.actions.map((a) => a.kind), ['open_settings', 'open_demo'])
  assert.ok(noConnection().show.say?.includes('$demo'))
}

const calls: string[] = []
const realFetch = globalThis.fetch
globalThis.fetch = (async (url: string) => {
  calls.push(String(url))
  return { ok: true, status: 200, json: async () => ({ available: false, setupDone: true, demoAnswered: true, tour: null, busy: false, conversation: null }) }
}) as typeof fetch

// No key: the bird is there and answers a question with the fixed reply.
{
  await guide.refresh()
  assert.ok(guide.available && guide.scripted && !guide.connected)
  guide.ask('Tsugumi で何ができる？', 'home')
  const [asked, reply] = guide.items.slice(-2)
  assert.deepEqual([asked.role, asked.text], ['user', 'Tsugumi で何ができる？'])
  assert.equal(reply.text, noConnection().text)
  assert.deepEqual(reply.actions?.map((a) => a.kind), ['open_settings', 'open_demo'])
  assert.equal(guide.bubble?.say, noConnection().show.say)
  assert.equal(guide.busy, false)
}

// The tour in the demo's copy: its chat shows each step, a choice leads to the
// next, a typed question keeps the step's choices, and a run's end is answered.
{
  const shown: { asked?: string; reply: string; choices: string[]; actions: string[] }[] = []
  const s = { dir: '/tour', connected: true, runActive: false, runStatus: undefined as { state: number } | undefined, currentRunId: undefined as string | undefined }
  const chat = { session: s, showScripted: (asked: string | undefined, reply: string, show: { choices: string[]; actions: { kind: string }[] }) => shown.push({ asked, reply, choices: show.choices, actions: show.actions.map((a) => a.kind) }) }
  ;(app as any).chat = () => chat
  guide.startTour({ id: 'report', title: 'Data to report' }, '/tour')
  guide.observe(s as any)
  assert.equal(shown[0].asked, 'デモ「Data to report」の案内を始めます')
  assert.equal(guide.bubble?.target, 'card:start')
  assert.equal(guide.speakingChat, chat as any)
  guide.answerScripted('次へ', chat as any)
  assert.equal(guide.bubble?.target, 'card:settings')
  guide.answerScripted('これは何？', chat as any)
  assert.equal(shown[shown.length - 1].reply, noConnection().text)
  assert.deepEqual(shown[shown.length - 1].choices, ['次へ'])
  assert.deepEqual(shown[shown.length - 1].actions, ['open_settings'], 'no second tour from inside the tour')
  assert.equal(guide.bubble?.target, 'card:settings', 'the bird stays where it was')
  for (const t of ['share', 'generate', 'aggregate', 'render', 'archive', 'run']) {
    guide.answerScripted('次へ', chat as any)
    assert.equal(guide.bubble?.target, t === 'run' ? 'run' : `card:${t}`)
  }
  assert.deepEqual(guide.bubble?.actions.map((a) => a.kind), ['run'])
  s.runActive = true
  s.currentRunId = 'r1'
  s.runStatus = { state: D.RunState.running }
  guide.observe(s as any)
  s.runActive = false
  s.runStatus = { state: D.RunState.succeeded }
  guide.observe(s as any)
  assert.equal(shown[shown.length - 1].asked, '実行が終わりました（成功）')
  assert.equal(guide.bubble?.target, 'card:render')
}

// $demo in a project's chat: in a copy of the demo the tour begins there;
// elsewhere the reply says where it can.
{
  guide.endTour()
  ;(app as any).status = { demos: [{ id: 'report', title: 'Data to report', description: '', tags: [] }] }
  const shown: { asked?: string; reply: string }[] = []
  const chatIn = (name: string, dir: string) => ({
    session: { dir, connected: true, runActive: false, runStatus: undefined, currentRunId: undefined, scopeName: () => name },
    showScripted: (asked: string | undefined, reply: string) => shown.push({ asked, reply }),
  }) as any
  guide.answerScripted('$demo', chatIn('My analysis', '/mine'))
  assert.equal(shown[shown.length - 1].reply, scriptStep('otherProject')!.text)
  assert.equal(guide.tour, null)
  const copy = chatIn('Data to report', '/copy')
  guide.answerScripted('$demo', copy)
  assert.equal(shown[shown.length - 1].asked, '$demo')
  assert.equal(shown[shown.length - 1].reply, scriptStep('start')!.text)
  assert.equal(guide.tour?.path, '/copy')
  assert.equal(guide.bubble?.target, 'card:start')
  guide.answerScripted('次へ', copy)
  assert.equal(guide.bubble?.target, 'card:settings')
}

assert.deepEqual(calls.filter((c) => c.includes('/api/guide/send') || c.includes('/api/chat/send')), [], 'nothing is sent to a model')
globalThis.fetch = realFetch
console.log('guide script: passed')
