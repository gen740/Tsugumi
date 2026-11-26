// The helpers of a delegate call on the canvas (as the Mac app's SyncTests and
// PresenceTests): chat events move each helper to its own Card and end its work.
import assert from 'node:assert/strict'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { BIRD_TIMING: T, HelperTeam } = await import('../src/model/helperBirds')
const { ChatController } = await import('../src/model/chat')

const noWriter = () => undefined

// A bird each, numbered in order, and each comes to its first Card.
{
  const team = new HelperTeam(noWriter)
  team.start([{ name: 'first', cards: ['a'] }, { name: 'second', cards: ['b', 'c'] }, { name: 'none', cards: [] }], 100)
  assert.deepEqual(team.birds.map((b) => [b.name, b.tint, b.card(100)]), [['first', 1, 'a'], ['second', 2, 'b'], ['none', 3, undefined]])
  assert.equal(team.tint('second'), 2)
  assert.equal(team.tint('someone of an earlier call'), 1)
  assert.ok(team.bird('first')!.line(100)?.text)
  // A new call starts afresh.
  team.start([{ name: 'other', cards: ['z'] }], 101)
  assert.deepEqual(team.birds.map((b) => [b.name, b.tint]), [['other', 1]])
}

// New work replaces the previous scene even within the same update.
{
  const team = new HelperTeam(noWriter)
  team.start([{ name: 'a', cards: ['one'] }, { name: 'b', cards: ['two'] }], 100)
  const read = (card: string) => ({ kind: 'toolStarted', name: 'project_file_read', arguments: JSON.stringify({ path: `definitions/${card}/m.py` }) })
  team.note('a', read('three'), 100)
  team.note('a', read('four'), 100)
  const a = team.bird('a')!
  assert.deepEqual(a.scenes.map(scene => scene.card), ['four'])
  assert.equal(a.card(100), 'four')
  const perch = (card: string) => ({ x: card === 'four' ? 1 : 2, y: 0 })
  a.place(100, perch)
  team.note('a', { kind: 'toolStarted', name: 'run', arguments: '{"card":{"segments":["start"]}}' }, 100)
  assert.equal(a.card(100), 'start')
  assert.equal(a.place(100, perch)?.flying, false, 'running takes precedence over flight')
  const b = team.bird('b')!
  team.note('b', { kind: 'toolStarted', name: 'project_inspect', arguments: '{}' }, 101)
  assert.equal(b.card(101), 'two')
  const inspecting = b.line(101)?.text
  assert.ok(inspecting)
  team.note('b', { kind: 'text' }, 102)
  assert.ok(b.line(102)?.text)
  assert.notEqual(b.line(102)?.text, inspecting)
  team.note('nobody', read('x'), 102)
  assert.equal(team.birds.length, 2)
}

// While a file of its is played, it is at that Card and says so.
{
  let file: { card: string } | undefined
  const team = new HelperTeam((who) => (who === 'a' ? { at: () => file } : undefined))
  team.start([{ name: 'a', cards: ['one'] }, { name: 'b', cards: ['two'] }], 100)
  file = { card: 'written' }
  assert.equal(team.bird('a')!.card(100.5), 'written')
  assert.ok(team.bird('a')!.line(100.5)?.text)
  assert.ok(team.bird('a')!.writing(100.5))
  assert.equal(team.bird('b')!.card(100.5), 'two')
  // A file of a Card not on the canvas yet: it says so from where it stands.
  const perch = (c: string) => (c === 'one' ? { x: 1, y: 2 } : undefined)
  assert.equal(team.bird('a')!.place(100.5, perch)?.x, 1)
  file = undefined
  assert.equal(team.bird('a')!.card(100.5), 'one')
}

// Each terminal event ends work, leaves a completion bubble and lets the bird disappear.
{
  const team = new HelperTeam(noWriter)
  const outcomes = ['finished', 'cancelled', 'error']
  team.start(outcomes.map(name => ({ name, cards: ['one'] })), 100)
  const completion = new Set<string>()
  const later = 101 + T.linger + T.doneLine
  for (const kind of outcomes) {
    const bird = team.bird(kind)!
    assert.equal(bird.visible(later), true, 'an active helper remains visible')
    team.note(kind, { kind }, 101)
    assert.equal(bird.working, false)
    assert.equal(bird.visible(101), true)
    assert.equal(bird.line(101)?.done, true)
    completion.add(bird.line(101)!.text)
    assert.equal(bird.line(later), undefined)
    assert.equal(bird.visible(later), false)
  }
  assert.equal(completion.size, outcomes.length, 'each outcome has a distinct completion message')
  assert.equal(team.out(later), false)
}

// A helper takes off and lands, or moves directly when animation is off.
{
  const team = new HelperTeam(noWriter)
  team.start([{ name: 'a', cards: ['one'] }], 100)
  const bird = team.bird('a')!
  const points = new Map([['one', { x: 500, y: 0 }], ['two', { x: 0, y: 0 }]])
  const perch = (card: string) => points.get(card)
  assert.equal(bird.place(100, perch)?.flying, false)
  bird.visit('two', 'reading', 101)
  assert.equal(bird.place(101, perch)?.flying, true)
  const inFlight = 101 + T.flight / 2
  assert.equal(bird.place(inFlight, perch)?.facing, -1)
  assert.ok(bird.trail(inFlight, perch).some(point => point.x < points.get('one')!.x))
  assert.equal(bird.place(102 + T.flight, perch)?.flying, false)
  assert.equal(bird.place(102 + T.flight, perch)?.x, points.get('two')!.x)
  bird.visit('one', 'reading', 103 + T.flight)
  assert.equal(bird.place(103 + T.flight, perch, true)?.flying, false)
  assert.equal(bird.place(103 + T.flight, perch, true)?.x, points.get('one')!.x)
  points.delete('one')
  assert.equal(bird.place(103 + T.flight, perch), undefined)
}

// The chat's events drive them: delegate brings the birds out, a helper's events move its own.
{
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { items: [], notes: [], choices: [], offered: [], busy: true, session: { dir: '/p' }, currentId: 's', contentRevision: 0, writers: new Map() })
  c.team = new HelperTeam((who: string) => c.writers.get(who))
  c.changed = () => {}
  c.refreshAfterTurn = () => {}
  c.flushNotes = () => {}
  c.handle({ kind: 'toolStarted', name: 'delegate', arguments: JSON.stringify({ tasks: [
    { name: 'knn', cards: ['knn'], instruction: 'x' }, { name: 'mlp', cards: ['mlp'], instruction: 'y' }, { cards: ['nameless'] },
  ] }) })
  assert.deepEqual(c.team.birds.map((b: any) => b.name), ['knn', 'mlp'])
  const task = (name: string, state = 'running', children: any[] = []) => ({ id: name, kind: 'helper', name, state, children })
  c.handle({ kind: 'presentation', count: 2, reset: true, seq: 1n, rows: [task('knn'), task('mlp')].map((row, index) => ({ row, index })) })
  assert.deepEqual(c.items.filter((i: any) => i.role === 'helper').map((i: any) => i.helper), ['knn', 'mlp'])
  // What a helper writes is its own: its panel, and its bird at that Card.
  const delta = JSON.stringify({ changes: [{ value: { path: 'definitions/knn2/m.py', bytes: 'import os' } }] })
  c.handle({ kind: 'helper', name: 'knn', event: { kind: 'toolInput', index: 0, name: 'project_save', delta } })
  assert.deepEqual([...c.writers.keys()], ['knn'])
  const now = Date.now() / 1000
  assert.equal(c.team.bird('knn').card(now), 'knn2')
  assert.equal(c.team.bird('mlp').card(now), 'mlp')
  c.handle({ kind: 'helper', name: 'mlp', event: { kind: 'toolStarted', name: 'project_inspect', arguments: '{}' } })
  assert.ok(c.team.bird('mlp').line(Date.now() / 1000)?.text)
  c.handle({ kind: 'presentation', count: 2, reset: false, seq: 2n, rows: [{ index: 1, row: task('mlp', 'running', [{ id: 'inspect', kind: 'step', children: [{ id: 'call', kind: 'tool', name: 'project_inspect', state: 'running' }] }]) }] })
  assert.equal(c.items.find((i: any) => i.helper === 'mlp').presentation.children[0].children[0].name, 'project_inspect')
  c.handle({ kind: 'helper', name: 'mlp', event: { kind: 'finished' } })
  assert.ok(!c.team.bird('mlp').working)
  assert.ok(c.team.bird('knn').working)
  // The turn's end leaves no helper at work.
  c.handle({ kind: 'finished' })
  assert.ok(!c.team.bird('knn').working)
}
console.log('helper birds: passed')

// Presentation off: existing queues release the marker; future work is immediate.
{
  let animate = true
  const team = new HelperTeam(noWriter, () => animate)
  team.start([{ name: 'helper', cards: ['a'] }], 100)
  team.bird('helper')!.visit('b', 'saving', 100.1, true)
  assert.equal(team.bird('helper')!.card(100.1), 'b')
  animate = false
  assert.equal(team.bird('helper')!.card(100.1), 'b')
  team.bird('helper')!.visit('c', 'checking', 100.2, true)
  assert.equal(team.bird('helper')!.card(100.2), 'c')
  assert.equal(team.bird('helper')!.line(100.2)?.text, 'checking')
}
