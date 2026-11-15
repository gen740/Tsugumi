import assert from 'node:assert/strict'
import { test } from 'node:test'
import { fileURLToPath } from 'node:url'
import path from 'node:path'
import { readJson, readProject } from '../player/build.mjs'

const root = fileURLToPath(new URL('../../../', import.meta.url))
const ports = readJson(path.join(root, 'docs/site/player-data/assets/ports.json'))

for (const language of ['ja', 'en']) {
  test(`${language} player data preserves real ports and Draft Card attachments`, () => {
    const demos = path.join(root, 'demos', language)
    let attachments = 0
    for (const entry of readJson(path.join(demos, 'index.json')).demos) {
      const { doc, cards, wires } = readProject(path.join(demos, entry.path), ports[entry.id])
      assert.deepEqual(wires, doc.canvas.connections.map(c => ({
        from: c.from.card, fromPort: c.from.port, to: c.to.card, toPort: c.to.port,
      })), `${entry.id}: every source wire is preserved`)
      const byId = new Map(cards.map(c => [c.id, c]))
      for (const card of cards.filter(c => c.kind === 'draft')) {
        assert.deepEqual(card.inputs, [])
        assert.deepEqual(card.outputs, [])
      }
      for (const wire of wires) {
        for (const [id, port, direction] of [[wire.from, wire.fromPort, 'outputs'], [wire.to, wire.toPort, 'inputs']]) {
          const card = byId.get(id)
          assert.ok(card, `${entry.id}: missing Card ${id}`)
          if (card.kind === 'draft') {
            assert.equal(port, '', `${entry.id}: ${id} attaches at the Card`)
            attachments++
          } else {
            assert.ok(port && card[direction].some(p => p.name === port), `${entry.id}: ${id}.${port}`)
          }
        }
      }
    }
    assert.ok(attachments > 0, 'the real Draft demos exercise Card attachments')
  })
}
