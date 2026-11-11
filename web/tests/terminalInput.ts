// What is typed reaches the terminal in order, whatever the requests take.
import assert from 'node:assert/strict'
import { TerminalInput } from '../src/model/terminalInput'

const sent: string[] = []
const finish: (() => void)[] = []
const failures: unknown[] = []
const input = new TerminalInput((data) => {
  sent.push(data)
  return new Promise<void>((resolve, reject) => {
    finish.push(() => (data === 'bad' ? reject(new Error('gone')) : resolve()))
  })
}, (e) => failures.push(e))
const settle = () => new Promise((r) => setTimeout(r))

input.write('a')
input.write('b')
input.write('c')
assert.deepEqual(sent, ['a'], 'one request at a time')
finish.shift()!()
await settle()
assert.deepEqual(sent, ['a', 'bc'], 'what was typed meanwhile goes together, in order')
finish.shift()!()
await settle()

// A failure is said, and what is typed next still goes.
input.write('bad')
finish.shift()!()
await settle()
assert.equal(failures.length, 1)
input.write('d')
assert.deepEqual(sent, ['a', 'bc', 'bad', 'd'])
finish.shift()!()
await settle()
console.log('terminal input: passed')
