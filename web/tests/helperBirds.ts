// The helpers of a delegate call on the canvas (as the Mac app's SyncTests and
// PresenceTests): a bird each in its own colour, at its own Card, one scene after
// another, gone a little while after it ended; the chat's events drive them.
import assert from 'node:assert/strict'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { birdColor, BIRD_TIMING: T, flightPoint, helperClear, HelperTeam, leadColor, perchOf, rise } = await import('../src/model/helperBirds')
const { ChatController } = await import('../src/model/chat')

const noWriter = () => undefined

// A bird each, numbered in order, and each comes to its first Card.
{
  const team = new HelperTeam(noWriter)
  team.start([{ name: 'first', cards: ['a'] }, { name: 'second', cards: ['b', 'c'] }, { name: 'none', cards: [] }], 100)
  assert.deepEqual(team.birds.map((b) => [b.name, b.tint, b.card(100)]), [['first', 1, 'a'], ['second', 2, 'b'], ['none', 3, undefined]])
  assert.equal(team.tint('second'), 2)
  assert.equal(team.tint('someone of an earlier call'), 1)
  assert.equal(team.bird('first')!.line(100)?.text, 'a を読んでいます')
  // A new call starts afresh.
  team.start([{ name: 'other', cards: ['z'] }], 101)
  assert.deepEqual(team.birds.map((b) => [b.name, b.tint]), [['other', 1]])
}

// What a helper does moves its own bird only, and scenes do not overlap.
{
  const team = new HelperTeam(noWriter)
  team.start([{ name: 'a', cards: ['one'] }, { name: 'b', cards: ['two'] }], 100)
  const read = (card: string) => ({ kind: 'toolStarted', name: 'project_file_read', arguments: JSON.stringify({ path: `definitions/${card}/m.py` }) })
  team.note('a', read('three'), 100.1)
  assert.equal(team.bird('a')!.card(100.1), 'one', 'its first scene goes on')
  assert.equal(team.bird('a')!.card(100 + T.scene), 'three')
  assert.equal(team.bird('b')!.card(100 + T.scene), 'two')
  assert.equal(team.bird('a')!.line(100 + T.scene)?.text, 'three を読んでいます')
  // Reading again what it reads is no new scene; one far behind the Agent is left out.
  team.note('a', read('three'), 100.2)
  assert.equal(team.bird('a')!.scenes.length, 2)
  for (let i = 0; i < 20; i++) team.note('a', read(`far${i}`), 100.3)
  assert.ok(team.bird('a')!.scenes.length < 8)
  // A save and a run are never left out.
  team.note('a', { kind: 'toolStarted', name: 'run', arguments: JSON.stringify({ card: { segments: ['start'] } }) }, 100.4)
  assert.equal(team.bird('a')!.scenes.at(-1)?.card, 'start')
  // Between scenes it says what it does now; a call with no Card leaves it where it is.
  team.note('b', { kind: 'toolStarted', name: 'project_inspect', arguments: '{}' }, 103)
  assert.equal(team.bird('b')!.line(103)?.text, 'プロジェクトを見ています')
  assert.equal(team.bird('b')!.card(103), 'two')
  team.note('b', { kind: 'text' }, 104)
  assert.equal(team.bird('b')!.line(104)?.text, '考えています')
  // Someone else's event changes nothing.
  team.note('nobody', read('x'), 105)
}

// While a file of its is played, it is at that Card and says so.
{
  let file: { card: string } | undefined
  const team = new HelperTeam((who) => (who === 'a' ? { at: () => file } : undefined))
  team.start([{ name: 'a', cards: ['one'] }, { name: 'b', cards: ['two'] }], 100)
  file = { card: 'written' }
  assert.equal(team.bird('a')!.card(100.5), 'written')
  assert.equal(team.bird('a')!.line(100.5)?.text, 'written を書いています')
  assert.ok(team.bird('a')!.writing(100.5))
  assert.equal(team.bird('b')!.card(100.5), 'two')
  file = undefined
  assert.equal(team.bird('a')!.card(100.5), 'one')
}

// It stays a little while after it ended, says how it ended, and fades away.
{
  const team = new HelperTeam(noWriter)
  team.start([{ name: 'a', cards: ['one'] }, { name: 'b', cards: ['two'] }], 100)
  assert.ok(team.out(100))
  team.note('a', { kind: 'finished' }, 100.5)
  const end = 100 + T.scene
  assert.equal(team.bird('a')!.endedAt, end, 'after the scene that plays')
  assert.equal(team.bird('a')!.line(end + 0.1)?.text, 'できました')
  assert.equal(team.bird('a')!.line(end + T.doneLine + 0.1), undefined)
  assert.equal(team.bird('a')!.fade(end + 1).opacity, 1)
  assert.ok(team.bird('a')!.fade(end + T.linger - 0.5).opacity < 1)
  assert.ok(team.bird('a')!.fade(end + T.linger - 0.5).lift > 0)
  assert.ok(!team.bird('a')!.visible(end + T.linger + 0.1))
  assert.ok(team.bird('b')!.visible(end + T.linger + 0.1), 'the other still works')
  assert.ok(team.out(end + T.linger + 0.1))
  // How the others end.
  team.note('b', { kind: 'error', message: 'cancelled' }, 200)
  assert.equal(team.bird('b')!.line(200.1)?.text, '止めました')
  assert.ok(!team.out(200 + T.linger + 0.1))
  team.start([{ name: 'c', cards: ['x'] }, { name: 'd', cards: ['y'] }], 300)
  team.note('c', { kind: 'error', message: 'broken' }, 301)
  assert.equal(team.bird('c')!.line(301 + T.scene)?.text, 'うまくいきませんでした')
  team.note('d', { kind: 'stepLimit' }, 301)
  assert.equal(team.bird('d')!.line(301 + T.scene)?.text, '途中で止まっています')
  // The main Agent's turn ended while one seems at work (its events were lost): none stays.
  team.start([{ name: 'e', cards: ['x'] }], 400)
  team.endAll('stopped', 401)
  assert.ok(!team.bird('e')!.working)
  assert.ok(!team.out(401 + T.scene + T.linger + 0.1))
}

// It flies between the Cards in an arc, facing the way it goes, and leaves its thread.
{
  const team = new HelperTeam(noWriter)
  team.start([{ name: 'a', cards: ['one'] }], 100)
  const b = team.bird('a')!
  const spots: Record<string, { x: number; y: number } | undefined> = { one: { x: 0, y: 0 }, two: { x: 500, y: 0 }, back: { x: -200, y: 0 } }
  const perch = (c: string) => spots[c]
  // It appears at the first without a flight.
  assert.deepEqual(b.place(100, perch), { x: 0, y: 0, flying: false, facing: 1 })
  b.visit('two', 'two', 100)
  const at = 100 + T.scene
  assert.deepEqual(b.place(at, perch), { x: 0, y: 0, flying: true, facing: 1 })
  const mid = b.place(at + T.flight / 2, perch)!
  assert.ok(Math.abs(mid.x - 250) < 1e-9 && Math.abs(mid.y + 20) < 1e-9 && mid.flying, 'halfway, at the top of the arc')
  assert.ok(new Set(b.trail(at + T.flight / 2, perch).map((p) => Math.round(p.x))).size > 5, 'its thread')
  assert.deepEqual(b.place(at + T.flight, perch), { x: 500, y: 0, flying: false, facing: 1 })
  b.visit('back', 'back', at + T.flight)
  const away = at + T.flight + T.scene
  b.place(at + T.flight + T.scene, perch)
  assert.equal(b.place(away + T.flight / 2, perch)!.facing, -1)
  // Still: at each place at once.
  b.visit('one', 'one', away + 5)
  assert.deepEqual(b.place(away + 5 + T.scene, perch, true), { x: 0, y: 0, flying: false, facing: 1 })
  // A Card out of the scope shown: the bird is not drawn.
  spots.one = undefined
  assert.equal(b.place(away + 5 + T.scene + 2, perch), undefined)
  const landed = flightPoint({ x: 0, y: 0 }, { x: 10, y: 10 }, 1)
  assert.ok(Math.abs(landed.x - 10) < 1e-9 && Math.abs(landed.y - 10) < 1e-9)
  assert.equal(rise(0.5), 0.5)
}

// The places are the Mac app's: on the Card's top edge, its bubble clear of the panel.
{
  const p = perchOf({ x: 100, y: 200 })
  assert.ok(Math.abs(p.x - 125.4) < 1e-9 && Math.abs(p.y - 183.05) < 1e-9)
  assert.ok(Math.abs(helperClear(1) - (16.95 + 54 * 0.8)) < 1e-9)
  assert.ok(Math.abs(helperClear(2) - (2 * 16.95 + 54 * 1.3)) < 1e-9)
}

// One hue per helper in both appearances, the Mac app's values, and gold is none of them.
for (const dark of [false, true]) {
  const colors = [1, 2, 3, 4, 5, 6, 7, 8].map((t) => birdColor(t, dark))
  assert.equal(new Set(colors).size, 8)
  assert.equal(birdColor(9, dark), colors[0], 'it wraps around')
  assert.ok(!colors.includes(leadColor(dark)))
}
assert.equal(birdColor(1, true), 'rgb(242, 186, 133)') // NSColor(hue: 0.08, saturation: 0.45, brightness: 0.95)
assert.equal(birdColor(2, false), 'rgb(40, 101, 158)') // NSColor(hue: 0.58, saturation: 0.75, brightness: 0.62)
assert.equal(leadColor(true), 'rgb(250, 204, 89)')

// The chat's events drive them: delegate brings the birds out, a helper's events move its own.
{
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { items: [], notes: [], choices: [], offered: [], busy: true, session: { dir: '/p' }, currentId: 's', writers: new Map() })
  c.team = new HelperTeam((who: string) => c.writers.get(who))
  c.changed = () => {}
  c.refreshAfterTurn = () => {}
  c.flushNotes = () => {}
  c.handle({ kind: 'toolStarted', name: 'delegate', arguments: JSON.stringify({ tasks: [
    { name: 'knn', cards: ['knn'], instruction: 'x' }, { name: 'mlp', cards: ['mlp'], instruction: 'y' }, { cards: ['nameless'] },
  ] }) })
  assert.deepEqual(c.team.birds.map((b: any) => b.name), ['knn', 'mlp'])
  assert.deepEqual(c.items.filter((i: any) => i.role === 'helper').map((i: any) => i.helper), ['knn', 'mlp'])
  // What a helper writes is its own: its panel, and its bird at that Card.
  const delta = JSON.stringify({ changes: [{ value: { path: 'definitions/knn2/m.py', bytes: 'import os' } }] })
  c.handle({ kind: 'helper', name: 'knn', event: { kind: 'toolInput', index: 0, name: 'project_save', delta } })
  assert.deepEqual([...c.writers.keys()], ['knn'])
  const now = Date.now() / 1000
  assert.equal(c.team.bird('knn').card(now + 0.5), 'knn2')
  assert.equal(c.team.bird('mlp').card(now + 0.5), 'mlp')
  c.handle({ kind: 'helper', name: 'mlp', event: { kind: 'toolStarted', name: 'project_inspect', arguments: '{}' } })
  assert.equal(c.team.bird('mlp').line(now + T.scene + 0.5)?.text, 'プロジェクトを見ています')
  assert.equal(c.items.find((i: any) => i.helper === 'mlp').tool.name, 'project_inspect')
  c.handle({ kind: 'helper', name: 'mlp', event: { kind: 'finished' } })
  assert.ok(!c.team.bird('mlp').working)
  assert.ok(c.team.bird('knn').working)
  // The turn's end leaves no helper at work.
  c.handle({ kind: 'finished' })
  assert.ok(!c.team.bird('knn').working)
}
console.log('helper birds: passed')
