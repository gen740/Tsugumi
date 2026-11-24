import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { buildDemo } from '../../../docs/site/player/build.mjs'

const source = fileURLToPath(new URL('../../../demos/en/report/', import.meta.url))
const assets = path.join(process.env.TSUGUMI_SITE_PLAYER_DIR, 'player-data', 'assets')
const read = (file) => JSON.parse(fs.readFileSync(file, 'utf8'))
const ports = read(path.join(assets, 'ports.json'))
const recording = read(path.join(assets, 'report.run.json'))
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsg-recording-'))
try {
  fs.cpSync(source, root, { recursive: true })
  const build = () => buildDemo({ id: 'report' }, root, ports, {}, recording)
  assert.equal(build().run?.id, recording.run.id)

  const document = path.join(root, 'tsugumi.json')
  const original = read(document)
  const edited = read(document)
  edited.canvas.cards.reverse()
  edited.canvas.connections.reverse()
  fs.writeFileSync(document, JSON.stringify(edited))
  assert.equal(build().run?.id, recording.run.id, 'reordering the document keeps the same graph')

  const summary = edited.canvas.connections.find(({ to }) => to.card === 'render' && to.port === 'summary')
  const config = edited.canvas.connections.find(({ to }) => to.card === 'render' && to.port === 'config')
  summary.to.port = 'config'
  config.to.port = 'summary'
  fs.writeFileSync(document, JSON.stringify(edited))
  let preview = build()
  assert.equal(preview.run, undefined, 'the same Cards wired to different ports do not replay the run')

  fs.writeFileSync(document, JSON.stringify(original))
  const child = path.join(root, 'externals', 'archive', 'tsugumi.json')
  const inner = read(child)
  inner.boundary.inputs.find(port => port.name === 'report').port = 'different'
  fs.writeFileSync(child, JSON.stringify(inner))
  preview = build()
  assert.equal(preview.run, undefined, 'a changed child Workflow boundary invalidates the recording')
} finally {
  fs.rmSync(root, { recursive: true, force: true })
}
