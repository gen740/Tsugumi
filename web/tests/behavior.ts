import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { merge3 } from '../src/model/merge'
import { parse, serialize } from '../src/shared/json'
const root = process.argv[2]
for (const c of JSON.parse(fs.readFileSync(path.join(root, 'tests/fixtures/merge-contract.json'), 'utf8'))) {
  const result = merge3(parse(JSON.stringify(c.base)), parse(JSON.stringify(c.ours)), parse(JSON.stringify(c.theirs)))
  assert.deepEqual(JSON.parse(serialize(result!)), c.expect, c.name)
}
console.log('merge behavior: passed')

await import("./recovery")

await import("./approval")

await import("./compaction")

await import("./storage")

await import("./table")

await import("./draft")

await import("./modelMenu")
