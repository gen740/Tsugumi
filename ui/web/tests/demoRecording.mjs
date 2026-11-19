import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { buildDemo } from '../../../docs/site/player/build.mjs'

const source = fileURLToPath(new URL('../../../demos/en/collatz/', import.meta.url))
const assets = path.join(process.env.TSUGUMI_SITE_PLAYER_DIR, 'player-data', 'assets')
const read = (file) => JSON.parse(fs.readFileSync(file, 'utf8'))
const ports = read(path.join(assets, 'ports.json'))
const recording = read(path.join(assets, 'collatz.run.json'))
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsg-recording-'))
try {
  fs.cpSync(source, root, { recursive: true })
  const build = () => buildDemo({ id: 'collatz' }, root, ports, {}, recording)
  assert.equal(build().run?.id, recording.run.id)
  assert.ok(Object.keys(build().outputs).length)

  const document = path.join(root, 'tsugumi.json')
  const original = read(document)
  const edited = read(document)
  edited.canvas.cards.reverse()
  edited.canvas.connections.reverse()
  fs.writeFileSync(document, JSON.stringify(edited))
  assert.equal(build().run?.id, recording.run.id, 'reordering the document keeps the same graph')

  edited.canvas.connections.find(({ to }) => to.card === 'collatz-cpp').to.port = 'other'
  fs.writeFileSync(document, JSON.stringify(edited))
  let preview = build()
  assert.equal(preview.run, undefined, 'the same Cards wired to different ports do not replay the run')
  assert.deepEqual(preview.outputs, {})

  fs.writeFileSync(document, JSON.stringify(original))
  const child = path.join(root, 'externals', 'collatz-cpp', 'tsugumi.json')
  const inner = read(child)
  inner.boundary.inputs.n.port = 'half'
  fs.writeFileSync(child, JSON.stringify(inner))
  preview = build()
  assert.equal(preview.run, undefined, 'a changed child Workflow boundary invalidates the recording')
  assert.deepEqual(preview.outputs, {})
} finally {
  fs.rmSync(root, { recursive: true, force: true })
}
