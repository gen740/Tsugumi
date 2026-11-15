// Recorded host replies keep these tests about gestures, bytes and undo.
// Graph semantics are checked in the host against these same fixtures.
import fs from 'node:fs'
import path from 'node:path'
import { isDeepStrictEqual } from 'node:util'

export async function subgraphReply(projectJson: string, cards: string[], id: string): Promise<{ projectJson: string; externalJson: string; linkJson: string }> {
  const fixture = path.join(process.argv[2], 'tests/fixtures/subgraph-edits.json')
  const fixtures = JSON.parse(fs.readFileSync(fixture, 'utf8'))
  const found = fixtures.find((f: any) => f.id === id && isDeepStrictEqual(f.cards, cards) && isDeepStrictEqual(JSON.parse(f.projectJson), JSON.parse(projectJson)))
  if (found) return found.reply
  throw new Error('Missing recorded host subgraph reply: ' + JSON.stringify({ projectJson, cards, id }))
}
