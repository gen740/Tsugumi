// What is typed reaches the terminal in order, whatever the requests take.
import assert from 'node:assert/strict'
import { TerminalInput } from '../src/model/terminalInput'

const sent: string[] = []
const finish: (() => void)[] = []
const failures: unknown[] = []
const combined = Promise.withResolvers<void>()
const failed = Promise.withResolvers<void>()
let onBad = () => {}
const input = new TerminalInput((data) => {
  sent.push(data)
  if (data === 'bc') combined.resolve()
  if (data === 'bad') onBad()
  return new Promise<void>((resolve, reject) => {
    finish.push(() => (data === 'bad' ? reject(new Error('gone')) : resolve()))
  })
}, (e) => { failures.push(e); failed.resolve() })

input.write('a')
input.write('b')
input.write('c')
assert.deepEqual(sent, ['a'], 'one request at a time')
finish.shift()!()
await combined.promise
assert.deepEqual(sent, ['a', 'bc'], 'what was typed meanwhile goes together, in order')
// Queue the next write while the second request is in flight.
input.write('bad')
const sendingBad = Promise.withResolvers<void>()
onBad = sendingBad.resolve
finish.shift()!()
await sendingBad.promise
// A failure is said, and what is typed next still goes.
finish.shift()!()
await failed.promise
assert.equal(failures.length, 1)
input.write('d')
assert.deepEqual(sent, ['a', 'bc', 'bad', 'd'])
finish.shift()!()
console.log('terminal input: passed')
