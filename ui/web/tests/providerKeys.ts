import assert from 'node:assert/strict'
import { ProviderKeys } from '../src/host/providerKeys'

// API keys live in hostd: the page sends them there and keeps no copy.
const calls: { method: string; fields: any }[] = []
let stored = false
const keys = new ProviderKeys(async <T>(method: string, fields: any): Promise<T> => {
  calls.push({ method, fields })
  if (method === 'ProviderSetRequest') stored = !fields.remove
  return { configured: stored, source: stored ? 'hostd' : undefined } as T
})
assert.equal(await keys.source('anthropic', 'desktop'), undefined)
await keys.save('anthropic', '  KEY_SENTINEL \n', 'desktop')
assert.deepEqual(calls.at(-1), { method: 'ProviderSetRequest', fields: { profile: 'desktop', id: 'anthropic', provider: { api_key: 'KEY_SENTINEL' } } })
assert.equal(await keys.source('anthropic', 'desktop'), 'hostd')
await assert.rejects(() => keys.save('anthropic', 'bad key', 'desktop'))
await keys.delete('anthropic', 'desktop')
assert.deepEqual(calls.at(-1)!.fields, { profile: 'desktop', id: 'anthropic', remove: true })
assert.equal(await keys.source('anthropic', 'desktop'), undefined)
console.log('Provider keys: saved, checked and removed in hostd passed')
