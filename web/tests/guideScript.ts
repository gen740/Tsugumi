// Tsugumi without a model connection (agent/Shared/guide-script.json): the
// script is whole (every choice and every run's end leads to one of its
// steps, every Card and setting it points at is in its demo), Home's first
// question picks a tour and its choices go on, each tour goes from its start
// to its farewell by choices and runs' ends, anything typed gets the reply
// that points to Settings and $demo, and nothing is sent to a model. The
// macOS app's GuideTests check the same script.
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
const { noConnection, scriptDemoFor, scriptDemos, scriptStep } = await import('../src/model/guideScript')
const { D } = await import('../src/shared/rpc')
const root = process.argv[2]
const demos = JSON.parse(fs.readFileSync(path.join(root, 'demos/index.json'), 'utf8')).demos as { id: string; title: string; path: string }[]

// The script is whole.
{
  type Raw = { show: { actions?: { kind: string; demo?: string }[]; open?: string; highlight?: string }; next?: Record<string, string>; ran?: Record<string, string> }
  const tours = script.tours as unknown as Record<string, { keywords: string[]; home: Record<string, Raw>; steps: Record<string, Raw> }>
  assert.deepEqual(scriptDemos, ['report', 'exoplanet-transit', 'pharmacokinetics'])
  for (const [demo, t] of Object.entries(tours)) {
    const entry = demos.find((d) => d.id === demo)!
    const dir = path.join(root, 'demos', entry.path)
    const cards = new Set(JSON.parse(fs.readFileSync(path.join(dir, 'tsugumi.json'), 'utf8')).canvas.cards.map((c: { id: string }) => c.id))
    for (const id of ['start', 'resume', 'run', 'ranOk', 'ranFailed', 'change', 'runAgain', 'ranAgain', 'next', 'farewell']) assert.ok(t.steps[id], `${demo}/${id}`)
    for (const id of ['pick', 'about']) assert.ok(t.home[id], `${demo}/${id}`)
    for (const [id, raw] of [...Object.entries(t.home), ...Object.entries(t.steps)]) {
      const name = `${demo}/${id}`
      const s = scriptStep(name)!
      assert.deepEqual(Object.keys(raw.next ?? {}).sort(), [...s.show.choices].sort(), `${name}: every choice leads somewhere`)
      for (const n of [...Object.values(s.next), s.ran?.ok, s.ran?.else]) if (n) assert.ok(scriptStep(n), `${name} → ${n}`)
      assert.equal(s.show.actions.length, raw.show.actions?.length ?? 0, `${name}: every button is one the app has`)
      for (const a of s.show.actions) if (a.demo) assert.ok(scriptDemos.includes(a.demo), `${name}: ${a.demo} has a tour`)
      const h = s.show.highlight
      if (h?.startsWith('card:')) assert.ok(cards.has(h.slice(5).split('/')[0]), `${name}: ${h} is in the demo`)
      if (h?.startsWith('demo:')) assert.equal(h, `demo:${demo}`)
      if (h?.startsWith('param:')) {
        // The setting is one of the Card the step opens.
        const card = JSON.parse(fs.readFileSync(path.join(dir, 'definitions', s.show.open!.slice(5), '_card.json'), 'utf8'))
        assert.ok(card.params.some((p: { name: string }) => p.name === h.slice(6)), `${name}: ${h}`)
      }
    }
    assert.equal(scriptStep(`${demo}/farewell`)?.show.endTour, true)
    assert.ok(scriptStep(`${demo}/farewell`)?.show.say?.includes('$demo'))
  }
  assert.deepEqual(noConnection().show.actions.map((a) => a.kind), ['open_settings', 'open_demo'])
  assert.ok(noConnection().show.say?.includes('$demo'))
  assert.deepEqual(scriptStep('otherProject')!.show.actions.map((a) => a.demo), scriptDemos)
  // Home's suggestions each find their tour.
  assert.equal(scriptDemoFor('データの集計とレポート'), 'report')
  assert.equal(scriptDemoFor('天文・地震・タンパク質のデータ'), 'exoplanet-transit')
  assert.equal(scriptDemoFor('ライブ表示'), 'pharmacokinetics')
  assert.equal(scriptDemoFor('C++ と Python'), 'report')
  assert.equal(scriptDemoFor('なんでも'), 'report')
}

const calls: string[] = []
const realFetch = globalThis.fetch
globalThis.fetch = (async (url: string) => {
  calls.push(String(url))
  return { ok: true, status: 200, json: async () => ({ available: false, setupDone: true, demoAnswered: true, tour: null, busy: false, conversation: null }) }
}) as typeof fetch
;(app as any).status = { demos: demos.map((d) => ({ ...d, description: '', tags: [] })) }

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

// Home's first question: the bird flies to the demo that fits; ほかのデモ and
// もっと詳しく go on from there, and a question keeps those choices.
{
  guide.answerInterest('天文・地震・タンパク質のデータ')
  assert.equal(guide.bubble?.target, 'demo:exoplanet-transit')
  assert.deepEqual(guide.bubble?.choices, ['ほかのデモ', 'もっと詳しく'])
  guide.choose('もっと詳しく')
  assert.equal(guide.items[guide.items.length - 1].text, scriptStep('exoplanet-transit/about')!.text)
  guide.choose('ほかのデモ')
  assert.equal(guide.bubble?.target, 'demo:pharmacokinetics')
  guide.choose('どれがおすすめ？')
  assert.equal(guide.bubble?.say, noConnection().show.say)
  assert.deepEqual(guide.bubble?.choices, ['ほかのデモ', 'もっと詳しく'])
  guide.choose('ほかのデモ')
  assert.equal(guide.bubble?.target, 'demo:report')
}

// Each tour in its demo's copy: its chat shows each step, もっと詳しく says more
// and goes on, a typed question keeps the step's choices, runs' ends are
// answered, and the farewell ends the tour.
for (const demo of scriptDemos) {
  const entry = demos.find((d) => d.id === demo)!
  const dir = `/copy-${demo}`
  const shown: { asked?: string; reply: string; choices: string[]; actions: string[] }[] = []
  const s = { dir, connected: true, runActive: false, runStatus: undefined as { state: number } | undefined, currentRunId: undefined as string | undefined, scopeName: () => entry.title }
  const chat = { session: s, showScripted: (asked: string | undefined, reply: string, show: { choices: string[]; actions: { kind: string }[] }) => shown.push({ asked, reply, choices: show.choices, actions: show.actions.map((a) => a.kind) }) }
  ;(app as any).chat = () => chat
  guide.startTour(entry, dir)
  guide.observe(s as any)
  assert.equal(shown[0].asked, `デモ「${entry.title}」の案内を始めます`)
  assert.equal(guide.bubble?.target, 'card:start')
  assert.equal(guide.speakingChat, chat as any)
  guide.answerScripted('もっと詳しく', chat as any)
  assert.equal(shown[shown.length - 1].reply, scriptStep(`${demo}/startMore`)!.text)
  guide.answerScripted('これは何？', chat as any)
  assert.equal(shown[shown.length - 1].reply, noConnection().text)
  assert.deepEqual(shown[shown.length - 1].choices, ['次へ'])
  assert.deepEqual(shown[shown.length - 1].actions, ['open_settings'], 'no second tour from inside the tour')
  assert.equal(guide.bubble?.target, 'card:start', 'the bird stays where it was')
  for (let i = 0; i < 20 && guide.bubble?.target !== 'run'; i++) guide.answerScripted('次へ', chat as any)
  assert.deepEqual(guide.bubble?.actions.map((a) => a.kind), ['run'])
  const ran = (state: number, run: string) => {
    s.runActive = true
    s.currentRunId = run
    s.runStatus = { state: D.RunState.running }
    guide.observe(s as any)
    s.runActive = false
    s.runStatus = { state }
    guide.observe(s as any)
  }
  guide.answerScripted('実行した！', chat as any)
  ran(D.RunState.failed, 'r1')
  assert.equal(guide.bubble?.target, 'logs')
  guide.answerScripted('もう一度実行する', chat as any)
  ran(D.RunState.succeeded, 'r2')
  assert.equal(shown[shown.length - 1].asked, '実行が終わりました（成功）')
  assert.equal(shown[shown.length - 1].reply, scriptStep(`${demo}/ranOk`)!.text)
  guide.answerScripted('次へ', chat as any)
  assert.ok(guide.bubble?.target?.startsWith('param:'))
  guide.answerScripted('変更して保存した！', chat as any)
  ran(D.RunState.succeeded, 'r3')
  assert.equal(shown[shown.length - 1].reply, scriptStep(`${demo}/ranAgain`)!.text)
  guide.answerScripted('次へ', chat as any)
  guide.answerScripted('終える', chat as any)
  assert.equal(guide.bubble?.farewell, true)
  assert.equal(guide.tour, null)
}

// A demo without a tour here: its copy says which demos have one.
{
  const shown: string[] = []
  const s = { dir: '/copy-newton', connected: true, runActive: false, runStatus: undefined, currentRunId: undefined, scopeName: () => 'Newton' }
  ;(app as any).chat = () => ({ session: s, showScripted: (_: string | undefined, reply: string) => shown.push(reply) })
  guide.startTour({ id: 'newton', title: 'Newton' }, '/copy-newton')
  guide.observe(s as any)
  assert.deepEqual(shown, [scriptStep('otherProject')!.text])
  guide.endTour()
}

// $demo in a project's chat: in a copy of a toured demo the tour begins there;
// elsewhere the reply says where it can.
{
  const shown: { asked?: string; reply: string }[] = []
  const chatIn = (name: string, dir: string) => ({
    session: { dir, connected: true, runActive: false, runStatus: undefined, currentRunId: undefined, scopeName: () => name },
    showScripted: (asked: string | undefined, reply: string) => shown.push({ asked, reply }),
  }) as any
  guide.answerScripted('$demo', chatIn('My analysis', '/mine'))
  assert.equal(shown[shown.length - 1].reply, scriptStep('otherProject')!.text)
  assert.equal(guide.tour, null)
  const title = demos.find((d) => d.id === 'pharmacokinetics')!.title
  const copy = chatIn(title, '/copy')
  guide.answerScripted('$demo', copy)
  assert.equal(shown[shown.length - 1].asked, '$demo')
  assert.equal(shown[shown.length - 1].reply, scriptStep('pharmacokinetics/start')!.text)
  assert.equal(guide.tour?.path, '/copy')
  guide.answerScripted('次へ', copy)
  assert.equal(guide.bubble?.target, 'card:settings')
}

assert.deepEqual(calls.filter((c) => c.includes('/api/guide/send') || c.includes('/api/chat/send')), [], 'nothing is sent to a model')
globalThis.fetch = realFetch
console.log('guide script: passed')
