import assert from 'node:assert/strict'
import { tablePage } from '../src/model/tablePage'
for (const rows of [0, 1, 200, 201, 2001, 10_000]) {
  const pages = tablePage(rows, 0).count
  const visited = Array.from({ length: pages }, (_, i) => tablePage(rows, i))
    .flatMap((p) => Array.from({ length: p.end - p.start }, (_, i) => p.start + i))
  assert.deepEqual(visited, Array.from({ length: rows }, (_, i) => i))
  const shrunk = tablePage(1, pages - 1)
  assert.equal(shrunk.start, 0); assert.equal(shrunk.end, 1)
}
console.log('table pagination: passed')
