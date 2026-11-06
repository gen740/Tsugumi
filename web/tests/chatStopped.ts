// Stopping an answer and sending at once: the stopped answer's end comes after
// the next one began, and that one is not shown as stopped.
import assert from 'node:assert/strict'

// The session's module reads the page when it loads, and listens to the server.
const page = globalThis as Record<string, unknown>
page.document = { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window = globalThis
page.matchMedia = () => ({ matches: false, addEventListener() {} })
page.EventSource = class { addEventListener() {} close() {} }
const { ChatController } = await import('../src/model/chat')

const chat = () => {
  const c = Object.create(ChatController.prototype) as any
  Object.assign(c, { items: [], notes: [], choices: [], offered: [], busy: false, stopping: false, session: { dir: '/p' }, currentId: 's' })
  c.changed = () => {}
  c.refreshAfterTurn = () => {}
  return c
}
const STOPPED = '回答を停止しました（Workflow の実行は止めていません）。'
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

console.log('chat stopped behavior: passed')
