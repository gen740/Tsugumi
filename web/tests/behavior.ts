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

// Agent recovery, images, references, approvals and compaction run through
// protocol/agent.fbs in tests/core/agent_rpc_test.cc. These are UI behaviors.

await import("./chatStopped")
await import("./folderAttachments")
await import("./switchCard")

await import("./table")
await import("./artifactHTML")

await import("./draft")
await import("./comments")
await import("./draftWires")
await import("./freedName")
await import("./summary")
await import("./versions")
await import("./wires")
await import("./wireDemos")

await import("./modelMenu")

await import("./chatFollow")
await import("./terminalText")

await import("./projectActions")

await import("./thumbnails")

await import("./liveCode")

await import("./helperBirds")
await import("./leaveProject")
await import("./terminalInput")

await import("./portGrid")

await import("./marquee")

await import("./arrange")

await import("./freeSpot")

await import("./deleteCard")

await import("./saveFailure")

await import("./draftDisplays")

await import("./nixSources")

await import("./sessionRestore")
await import("./sessionConnect")

await import("./perchScale")
await import("./guideScript")
await import("./demoReplay")
await import("./evidence")

await import('./nixReferences')
await import('./nixReferenceOverlay')
