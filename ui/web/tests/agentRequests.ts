import assert from 'node:assert/strict'
import { cardRequest } from '../shared/agentShared'

for (const kind of ['draftRequest', 'commentRequest'] as const) {
  const request = cardRequest(kind, ['outer', 'inner'], ['prepare', 'render'])
  for (const target of ['outer/inner', 'prepare', 'render']) assert.ok(request.includes(target))
}
