import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { buildDemo, demosDir } from '../../../docs/site/player/build.mjs'

const source = path.join(demosDir(), 'en/report')
const assets = path.join(process.env.TSUGUMI_SITE_PLAYER_DIR, 'player-data', 'assets')
const read = (file) => JSON.parse(fs.readFileSync(file, 'utf8'))
const ports = read(path.join(assets, 'ports.json'))
const player = fileURLToPath(new URL('../../../docs/site/player/build.mjs', import.meta.url))
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'tsg-recording-'))
try {
  fs.cpSync(source, root, { recursive: true })
  for (const entry of fs.readdirSync(root, { recursive: true, withFileTypes: true })) {
    if (entry.isDirectory()) fs.chmodSync(path.join(entry.parentPath, entry.name), 0o755)
  }
  // Use the recording's timeline with the current graph for this fixture.
  // The archived recording keeps the graph it actually ran.
  const recording = {
    ...read(path.join(assets, 'report.run.json')),
    graphId: execFileSync(process.execPath, [player, '--graph-id', root], { encoding: 'utf8' }).trim(),
  }
  const build = () => buildDemo({ id: 'report' }, root, ports, {}, recording)
  assert.equal(build().run?.id, recording.run.id)

  const document = path.join(root, 'tsugumi.json')
  const original = read(document)
  fs.chmodSync(document, 0o644)
  const edited = read(document)
  edited.canvas.cards.reverse()
  edited.canvas.connections.reverse()
  fs.writeFileSync(document, JSON.stringify(edited))
  assert.equal(build().run?.id, recording.run.id, 'reordering the document keeps the same graph')

  const summary = edited.canvas.connections.find(({ to }) => to.card === 'render' && to.port === 'summary')
  summary.to.port = 'different'
  fs.writeFileSync(document, JSON.stringify(edited))
  let preview = build()
  assert.equal(preview.run, undefined, 'the same Cards wired to different ports do not replay the run')

  fs.writeFileSync(document, JSON.stringify(original))
  const child = path.join(root, 'externals', 'archive', 'tsugumi.json')
  fs.chmodSync(child, 0o644)
  const inner = read(child)
  inner.boundary.inputs.find(port => port.name === 'report').port = 'different'
  fs.writeFileSync(child, JSON.stringify(inner))
  preview = build()
  assert.equal(preview.run, undefined, 'a changed child Workflow boundary invalidates the recording')
} finally {
  fs.rmSync(root, { recursive: true, force: true })
}
