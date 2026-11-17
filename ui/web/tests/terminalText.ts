// A log's carriage returns write over their line, as in a terminal.
import assert from 'node:assert/strict'
import { terminalText } from '../src/model/terminalText'

assert.equal(terminalText(' 10% 1.0M\r 55% 5.5M\r100%  10M\ndone\n'), '100%  10M\ndone\n')
// A shorter redraw keeps the rest of the line.
assert.equal(terminalText('abcdef\rXY'), 'XYcdef')
// A carriage return at the end keeps the line until something writes over it.
assert.equal(terminalText('50%\r'), '50%')
assert.equal(terminalText('50%\r60%\r'), '60%')
assert.equal(terminalText('a\r\nb\r\n'), 'a\nb\n')
const plain = '日本語 · é\nline\n'
assert.equal(terminalText(plain), plain)

console.log('terminal text: passed')
