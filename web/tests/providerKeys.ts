import assert from 'node:assert/strict'
import { ProviderKeys } from '../server/providerKeys'

// No real Keychain command or RPC listener: the double checks the same argv,
// stdin and ephemeral flag that protect the production key.
const calls: { method: string; fields: any }[] = []
const commands: { args: string[]; input?: string }[] = []
let attributes = 'first', secret = 'KEYCHAIN_SENTINEL', present = true, refused = false, saveFails = false
const keys = new ProviderKeys(async <T>(method: string, fields: any): Promise<T> => {
  calls.push({ method, fields })
  return { configured: false } as T
}, 'web:keys:fixture', 'darwin', async (command, args, input) => {
  assert.equal(command, '/usr/bin/security')
  commands.push({ args, input })
  if (args[0] === '-i') return { code: saveFails ? 1 : 0, out: '' }
  if (args[0] === 'delete-generic-password') { present = false; return { code: 0, out: '' } }
  return { code: !present || args.includes('-w') && refused ? 1 : 0, out: args.includes('-w') ? secret : attributes }
}, {})
assert.equal(await keys.source('anthropic', 'desktop'), 'keychain')
assert.equal(commands.filter((c) => c.args.includes('-w')).length, 0, 'status never reads a secret')
await keys.provision('anthropic'); await keys.provision('anthropic')
assert.equal(commands.filter((c) => c.args.includes('-w')).length, 1)
assert.ok(calls.every((c) => c.fields.ephemeral === true))
attributes = 'changed'; refused = true
await keys.provision('anthropic'); await keys.provision('anthropic')
assert.equal(commands.filter((c) => c.args.includes('-w')).length, 2, 'a refused read is remembered')
assert.equal(calls.at(-1)!.fields.remove, true)
refused = false
assert.equal(await keys.save('anthropic', secret, 'desktop'), 'keychain')
assert.ok(commands.every((c) => !c.args.some((a) => a.includes(secret))), 'keys never appear in argv')
assert.ok(commands.some((c) => c.input?.includes(secret)))
assert.ok(calls.filter((c) => c.fields.provider).every((c) => c.fields.ephemeral === true))
await keys.delete('anthropic', 'desktop')
assert.equal(calls.at(-1)!.fields.remove, true)
saveFails = true
assert.equal(await keys.save('anthropic', 'FALLBACK_SENTINEL', 'desktop'), 'agentd')
assert.ok(calls.some((c) => c.fields.profile === 'desktop' && !c.fields.ephemeral && c.fields.provider?.api_key === 'FALLBACK_SENTINEL'))
console.log('Platform keys: attribute-only status, cached refusals, stdin, memory provisioning and fallback passed')

const envCalls: any[] = []
const env = new ProviderKeys(async <T>(_method: string, fields: any): Promise<T> => { envCalls.push(fields); return {} as T },
  'web:env:test', 'linux', async () => { throw new Error('non-Mac must not call security') }, { ANTHROPIC_API_KEY: 'ENV_SENTINEL' })
assert.equal(await env.source('anthropic', 'standalone'), 'env')
await env.provision('anthropic')
assert.equal(envCalls.at(-1).provider.api_key, 'ENV_SENTINEL')
assert.equal(await env.save('anthropic', 'DISK_SENTINEL', 'standalone'), 'agentd')
assert.ok(envCalls.some((c) => c.profile === 'standalone' && !c.ephemeral && c.provider?.api_key === 'DISK_SENTINEL'))
await env.delete('anthropic', 'standalone')
assert.equal(envCalls.at(-1).provider.api_key, 'ENV_SENTINEL', 'deleting a stored key preserves ENV precedence')
assert.equal(envCalls.at(-1).ephemeral, true)
