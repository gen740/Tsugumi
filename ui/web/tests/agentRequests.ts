import assert from 'node:assert/strict'
import { AGENT, cardRequest } from '../shared/agentShared'

for (const kind of ['draftRequest', 'commentRequest'] as const) {
  for (const scope of [[], ['outer'], ['outer', 'inner']]) {
    const where = scope.length ? AGENT.draftRequest.child.replace('{scope}', scope.join('/')) : AGENT.draftRequest.root
    const expected = AGENT[kind].text.replace('{where}', where).replace('{list}', '- outer/work\n- outer/render')
    assert.equal(cardRequest(kind, scope, ['outer/work', 'outer/render']), expected)
  }
}
console.log('shared request substitution: passed')
