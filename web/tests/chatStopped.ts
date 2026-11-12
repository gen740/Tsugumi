// Stopping an answer and sending at once: the stopped answer's end comes after
// the next one began, and that one is not shown as stopped.
import assert from 'node:assert/strict'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { ChatController, CONTEXT_FULL_TEXT } = await import('../src/model/chat')

const chat = () => {
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { items: [], notes: [], choices: [], offered: [], busy: false, stopping: false, turn: 0, transition: Promise.resolve(), session: { dir: '/p' }, currentId: 's', writers: new Map(), team: { endAll() {} } })
  c.changed = () => {}
  c.refreshAfterTurn = () => {}
  return c
}
const STOPPED = '回答を停止しました（Workflow の実行は止めていません）。'

// The context limit: while compacting the chat says so, and a conversation that
// cannot fit says what to do; the next answer has none of it left.
{
  const c = chat()
  c.begin()
  c.handle({ kind: 'compacting' })
  assert.equal(c.status, '会話を圧縮しています…')
  c.reloadCurrent = () => {}
  c.handle({ kind: 'compacted' })
  assert.equal(c.status, undefined)
  c.handle({ kind: 'error', message: CONTEXT_FULL_TEXT })
  assert.equal(c.busy, false)
  assert.equal(c.contextFull, true)
  assert.match(c.status, /新しい会話を始めてください/)
  c.begin()
  assert.equal(c.contextFull, false)
  assert.equal(c.status, undefined)
  c.handle({ kind: 'compacting' })
  c.handle({ kind: 'finished' })
  assert.equal(c.status, undefined)
}
const cancelled = { kind: 'error', message: 'cancelled' }

// The next answer began first.
let c = chat()
c.begin()
c.cancel()
c.begin()
c.handle(cancelled)
assert.equal(c.busy, true)
assert.equal(c.status, undefined)
c.handle({ kind: 'finished' })
assert.equal(c.busy, false)
assert.equal(c.status, undefined)

// Stopped with nothing after it: it says so.
c = chat()
c.begin()
c.cancel()
c.handle(cancelled)
assert.equal(c.busy, false)
assert.equal(c.status, STOPPED)

// Its end came before the next answer: that one can be stopped too.
c.begin()
assert.equal(c.status, undefined)
c.cancel()
c.handle(cancelled)
assert.equal(c.busy, false)
assert.equal(c.status, STOPPED)

// An answer stopped elsewhere (another window) still ends here.
c = chat()
c.begin()
c.handle(cancelled)
assert.equal(c.busy, false)
assert.equal(c.status, STOPPED)

// Only source-saving tool arguments belong on the canvas. A successful save
// changes the label; a rejected or failed save removes the unsaved preview.
{
  const c = chat()
  const delta = JSON.stringify({ changes: [{ value: { path: 'definitions/a/main.py', patches: [{ text: 'print(42)' }] } }] })
  c.noteWriting('', { kind: 'toolInput', name: 'project_file_read', index: 0, delta })
  assert.equal(c.writers.size, 0)
  c.noteWriting('', { kind: 'toolInput', name: 'project_save', index: 1, delta })
  const writer = c.writers.get('')
  assert.equal(writer.at(Date.now() / 1000)?.saved, false)
  assert.equal(writer.at(Date.now() / 1000)?.patch, true)
  c.noteWriting('', { kind: 'toolFinished', name: 'project_save', ok: true })
  assert.equal(writer.at(Date.now() / 1000)?.saved, true)
  c.noteWriting('', { kind: 'toolInput', name: 'project_save', index: 2, delta })
  c.noteWriting('', { kind: 'toolFinished', name: 'project_save', ok: false })
  assert.equal(writer.at(Date.now() / 1000), undefined)
}

// An answer stopped while a file still arrives leaves no unsaved text on the canvas.
{
  const c = chat()
  c.begin()
  c.handle({ kind: 'toolInput', name: 'project_save', index: 0, delta: '{"changes":[{"value":{"path":"definitions/a/main.py","bytes":"half' })
  const writer = c.writers.get('')
  assert.equal(writer.at(Date.now() / 1000)?.text, 'half')
  c.cancel()
  c.handle(cancelled)
  assert.equal(c.status, STOPPED)
  assert.equal(writer.at(Date.now() / 1000), undefined)
}

// A save that waits for an approval is never shown as saved; a refusal takes it away.
{
  const c = chat()
  const delta = JSON.stringify({ changes: [{ value: { path: 'definitions/a/main.py', bytes: 'print(42)' } }] })
  c.begin()
  c.handle({ kind: 'toolInput', name: 'project_save', index: 0, delta })
  c.handle({ kind: 'permission', id: 'p1', asked: 'save', from: '' })
  const writer = c.writers.get('')
  assert.equal(writer.at(Date.now() / 1000)?.saved, false)
  assert.equal(writer.at(Date.now() / 1000 + 5), undefined, 'gone after its hold while the approval is not answered')
  c.handle({ kind: 'permissionAnswered', id: 'p1' })
  c.noteWriting('', { kind: 'toolFinished', name: 'project_save', ok: false })
  assert.equal(writer.at(Date.now() / 1000), undefined)
}

// A helper writing the same file as the Agent has a panel of its own, and its
// failure takes only that one away.
{
  const c = chat()
  const delta = (text: string) => `{"changes":[{"value":{"path":"definitions/a/main.py","bytes":"${text}`
  c.begin()
  c.handle({ kind: 'toolInput', name: 'project_save', index: 0, delta: delta('lead') })
  c.handle({ kind: 'helper', name: 'helper-1', event: { kind: 'toolInput', name: 'project_save', index: 0, delta: delta('helper') } })
  const now = Date.now() / 1000
  assert.deepEqual([...c.writers.keys()], ['', 'helper-1'])
  assert.equal(c.writers.get('').at(now)?.text, 'lead')
  assert.equal(c.writers.get('helper-1').at(now)?.text, 'helper')
  c.noteWriting('helper-1', { kind: 'toolFinished', name: 'project_save', ok: false })
  assert.equal(c.writers.get('helper-1').at(Date.now() / 1000), undefined)
  assert.equal(c.writers.get('').at(Date.now() / 1000)?.text, 'lead')
}

console.log('chat stopped behavior: passed')
