// The names and addresses of nixSources, the same on the Mac (tests/fixtures/nix-source-refs.json).
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { shortCommit, sourceName, unlockedRef } from '../src/model/nixSources'

const fixture = path.resolve(import.meta.dirname ?? '.', '../../tests/fixtures/nix-source-refs.json')
const file = fs.existsSync(fixture) ? fixture : path.resolve('../tests/fixtures/nix-source-refs.json')
for (const c of JSON.parse(fs.readFileSync(file, 'utf8')) as { ref: string; name: string; unlocked: string; commit: string | null }[]) {
  assert.equal(sourceName(c.ref), c.name, c.ref)
  assert.equal(unlockedRef(c.ref), c.unlocked, c.ref)
  assert.equal(shortCommit(c.ref) ?? null, c.commit, c.ref)
}
console.log('nix sources: passed')
