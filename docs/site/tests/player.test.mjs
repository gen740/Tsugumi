import assert from 'node:assert/strict'
import { test } from 'node:test'
import { fileURLToPath } from 'node:url'
import path from 'node:path'
import { demosDir, readJson, readProject } from '../player/build.mjs'

const root = fileURLToPath(new URL('../../../', import.meta.url))
const ports = readJson(path.join(root, 'docs/site/player-data/assets/ports.json'))

test('player wires reach published ports and Draft Cards', () => {
  const demos = path.join(demosDir(), 'en')
  let attachments = 0
  for (const entry of readJson(path.join(demos, 'index.json')).demos) {
    const { doc, cards, wires } = readProject(path.join(demos, entry.path), ports[entry.id])
    assert.equal(wires.length, doc.canvas.connections.length, entry.id)
    const byId = new Map(cards.map(c => [c.id, c]))
    for (const wire of wires) {
      for (const [id, port, direction] of [[wire.from, wire.fromPort, 'outputs'], [wire.to, wire.toPort, 'inputs']]) {
        const card = byId.get(id)
        assert.ok(card, `${entry.id}: missing Card ${id}`)
        if (card.kind === 'draft') {
          assert.equal(port, undefined, `${entry.id}: ${id} attaches at the Card, with no port`)
          attachments++
        } else {
          assert.ok(card[direction].some(p => p.name === port), `${entry.id}: ${id}.${port}`)
        }
      }
    }
  }
  assert.ok(attachments > 0, 'the real Draft demos exercise Card attachments')
})
