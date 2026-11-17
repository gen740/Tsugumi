import { cardIds } from './cardIds'
// Comments on a Card: written, edited, resolved and removed in its _meta.json,
// each saved at once, on any Card, its copy having comments of its own; who did
// what is kept; the canvas shows how many are open and the Agent is asked about those only.
import assert from 'node:assert/strict'
import { get, parse, serialize, str } from '../shared/json'
import { savedComments, toolSummary } from '../shared/agentShared'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { cardComments, commentChange, ProjectSession } = await import('../src/model/session')

const session = () => {
  const s = cardIds(new ProjectSession('/p', 'k', 'p'))
  s.project = parse('{"name":"t","canvas":{"cards":[],"connections":[]}}')
  s.layout = parse('{}')
  let saves = 0
  const operate = s.operateProject
  s.operateProject = async request => { saves++; return operate(request) }
  return { s, saves: () => saves }
}

{
  const { s, saves } = session()
  const k = await s.addCard('python', [], { x: 0, y: 0 })
  assert.deepEqual(s.comments(k), [])
  assert.equal(get(s.cardMeta(k), 'comments'), undefined)

  const before = saves()
  await s.addComment(k, '  y 軸を対数に  ')
  await s.save()
  assert.equal(saves(), before + 1, 'a comment is saved at once')
  await s.addComment(k, '   ')
  assert.equal(s.comments(k).length, 1, 'an empty comment is not written')
  const [first] = s.comments(k)
  assert.equal(first.text, 'y 軸を対数に')
  assert.equal(first.resolved, false)
  assert.equal(s.nodeData(k).openComments, 1)
  assert.deepEqual(s.cardsWithOpenComments([]), [k])
  assert.ok(s.cardDetails(k).some((l) => l.includes('y 軸を対数に')), 'the hover says what is asked')

  await s.addComment(k, '軸に名前を')
  s.edit((x) => x.setCommentText(k, first.id, '両軸を対数に'))
  await s.save()
  assert.deepEqual(s.comments(k).map((c) => c.text), ['両軸を対数に', '軸に名前を'])
  s.edit((x) => x.setCommentText(k, first.id, '  '))
  await s.save()
  assert.equal(s.comments(k)[0].text, '両軸を対数に', 'a comment keeps its text rather than going blank')

  // The Agent's line goes away when the comment is opened again.
  s.updateMeta(k, (m) => {
    const list = get(m, 'comments') as unknown as Map<string, unknown>[]
    list[0].set('resolved', true)
    list[0].set('resolution', '両軸を対数にした')
  })
  await s.save()
  assert.equal(s.comments(k)[0].resolution, '両軸を対数にした')
  assert.equal(s.nodeData(k).openComments, 1)
  s.setCommentResolved(k, first.id, false)
  await s.save()
  assert.equal(s.comments(k)[0].resolved, false)
  assert.equal(s.comments(k)[0].resolution, undefined)
  s.setCommentResolved(k, first.id, true)
  await s.save()
  assert.deepEqual(s.openComments(k).map((c) => c.text), ['軸に名前を'], 'resolved ones stay, not open')

  for (const c of s.comments(k)) { s.removeComment(k, c.id); await s.save() }
  assert.deepEqual(s.comments(k), [], 'no comments remain')
  assert.equal(get(s.cardMeta(k), 'name'), 'card', 'the name stays after the last comment is removed')
  assert.deepEqual(s.cardsWithOpenComments([]), [])
}

{
  // What else the Card's _meta.json says is kept, and undo takes a comment back.
  const { s } = session()
  const k = await s.addCard('python', [], { x: 0, y: 0 })
  s.edit((x) => x.updateMeta(k, (m) => m.set('description', 'Plots')))
  await s.save()
  await s.addComment(k, 'one')
  assert.equal(str(get(s.cardMeta(k), 'description')), 'Plots')
  await s.undo()
  assert.deepEqual(s.comments(k), [])
  assert.equal(str(get(s.cardMeta(k), 'description')), 'Plots')
}

{
  // A Card in Card has a _meta.json too, though its settings are not edited here.
  const { s } = session()
  const k = await s.addCard('python', [], { x: 0, y: 0 })
  s.setCardDefinition(k, { card: parse('{}'), link: parse('{"external":"inner"}') })
  await s.addComment(k, 'split this')
  assert.equal(s.comments(k).length, 1)
  assert.equal(str(get(s.cardDefinitionDraft(k)?.link, 'external')), 'inner', 'it stays a Card in Card')
}


{
  // A Draft takes comments as any Card.
  const { s } = session()
  const k = await s.addCard('draft', [], { x: 0, y: 0 })
  await s.addComment(k, 'think about units')
  assert.deepEqual(s.cardsWithOpenComments([]), [k])
}

{
  // Who did what: the user's marks here, and what the core recorded of the Agent's.
  const { s } = session()
  const k = await s.addCard('python', [], { x: 0, y: 0 })
  await s.addComment(k, 'mine')
  const [c] = s.comments(k)
  assert.equal(c.author, 'user')
  s.edit((x) => x.setCommentText(k, c.id, 'mine, reworded'))
  await s.save()
  assert.equal(s.comments(k)[0].editedBy, 'user')
  s.setCommentResolved(k, c.id, true)
  await s.save()
  assert.equal(s.comments(k)[0].stateBy, 'user')
  const agent = cardComments(parse('{"comments":[{"id":"a","text":"note","author":"agent","resolved":true,"stateBy":"agent","resolution":"did","cards":["p","q"]}]}'))
  assert.deepEqual(agent[0].cards, ['p', 'q'])
  assert.equal(commentChange('resolved', 'agent'), '解決（Agent）')
  assert.equal(commentChange('removed'), '削除')
  s.edit((x) => x.updateMeta(k, (m) => m.set('comments', parse('[{"id":"z","text":"t","resolved":true,"stateBy":"agent","resolution":"did","cards":["p"]}]') as never)))
  await s.save()
  s.setCommentResolved(k, 'z', false)
  await s.save()
  assert.deepEqual(s.comments(k)[0].cards, [], 'opening it again takes back what the resolving save did')
  assert.equal(s.comments(k)[0].stateBy, 'user')
}

{
  // A save's result, whose end the chat row's start would cut off.
  const list = [{ card: 'plot', text: 'log scale', change: 'resolved', by: 'agent', resolution: 'done', cards: ['plot', 'audit'] }]
  const result = JSON.stringify({ reply: 'SaveResult', value: { delta: { big: 'x'.repeat(2000) }, comments: list } })
  const row = 'args\n→ ' + toolSummary(result)
  assert.deepEqual(savedComments(row), list)
  assert.deepEqual(savedComments('args\n→ ' + toolSummary('{"reply":"RunStatus"}')), [])
  assert.equal(toolSummary('x'.repeat(500)).length, 400)
}

console.log('comments: ok')

{
  const { s } = session()
  const k = await s.addCard('python', [], { x: 0, y: 0 })
  await s.addComment(k, 'Read only: [source](card:a?file=src%2Fmain.py&lines=2-4) uses a clock.', true)
  assert.equal(s.comments(k)[0].finding, true)
  assert.equal(s.openComments(k).length, 1, 'Kept observations remain visible as comments')
  assert.deepEqual(s.changeRequests(k), [])
  assert.deepEqual(s.cardsWithOpenComments([]), [], 'A kept finding does not trigger generation')
  await s.addComment(k, 'Change the clock to an explicit input')
  assert.equal(s.changeRequests(k).length, 1)
  assert.deepEqual(s.cardsWithOpenComments([]), [k])
}
