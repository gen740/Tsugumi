import assert from 'node:assert/strict'
import { cardRequest } from '../src/shared/agentShared'

for (const kind of ['draftRequest', 'commentRequest'] as const) {
  const text = cardRequest(kind, ['outer'], ['outer/work', 'outer/render'])
  assert.ok(text.includes('the child workflow outer'))
  assert.ok(text.includes('- outer/work\n- outer/render'))
  assert.ok(text.includes('project_validate') && text.includes('run_record'))
  assert.ok(!text.includes('{where}') && !text.includes('{list}') && !text.includes('delegate'))
}
assert.ok(cardRequest('draftRequest', [], ['work']).includes('the root workflow'))
console.log('shared generation requests: passed')
