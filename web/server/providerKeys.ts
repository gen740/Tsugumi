import { execFile } from 'node:child_process'
import { keychainReader } from './keychain'
import { L } from './i18n'

type Call = <T = any>(method: string, fields: Record<string, unknown>) => Promise<T>
type Run = (command: string, args: string[], input?: string) => Promise<{ code: number; out: string }>
export type KeyedProvider = 'anthropic' | 'openai'
const providers = { anthropic: { name: 'Anthropic', env: 'ANTHROPIC_API_KEY' }, openai: { name: 'OpenAI', env: 'OPENAI_API_KEY' } }
const service = 'app.tsugumi.agent'
const run: Run = (command, args, input) => new Promise((resolve) => {
  const child = execFile(command, args, (error, stdout) => resolve({ code: error ? (typeof error.code === 'number' ? error.code : 1) : 0, out: stdout.trim() }))
  child.stdin?.on('error', () => {})
  if (input !== undefined) child.stdin?.end(input)
})

/** The platform's small secret adapter. Keys saved in the login Keychain or
 *  supplied by the environment cross Host RPC only with ephemeral:true. */
export class ProviderKeys {
  private readonly keychain
  constructor(private readonly call: Call, readonly memoryProfile: string,
    private readonly platform = process.platform, private readonly execute: Run = run,
    private readonly environment = process.env) {
    this.keychain = keychainReader(execute, service)
  }
  private memory(id: KeyedProvider, key?: string, source?: string) {
    return this.call('ProviderSetRequest', { profile: this.memoryProfile, id, ephemeral: true, source,
      remove: !key, provider: key ? { api_key: key } : undefined })
  }
  async provisionEnvironment() {
    for (const id of ['anthropic', 'openai'] as const) {
      const key = this.environment[providers[id].env]
      if (key) await this.memory(id, key, 'env')
    }
  }
  async provision(id: KeyedProvider): Promise<string> {
    await this.memory(id, this.environment[providers[id].env], 'env')
    if (this.platform === 'darwin') await this.memory(id, await this.keychain.read(providers[id].name), 'keychain')
    return this.memoryProfile
  }
  async source(id: KeyedProvider, profile: string): Promise<'env' | 'keychain' | 'agentd' | undefined> {
    await this.provision(id)
    const status = await this.call<{ source?: 'env' | 'keychain' | 'agentd' }>('ProviderStatusRequest', { profile, id, key_profile: this.memoryProfile })
    return status.source || undefined
  }
  async save(id: KeyedProvider, raw: string, profile: string): Promise<'keychain' | 'agentd'> {
    const key = raw.trim()
    if (!/^[\x21-\x7e]+$/.test(key) || /["\\]/.test(key)) throw new Error(L('API キーに使えない文字が含まれています。'))
    const policy = await this.call<{ storage: string }>('ProviderStatusRequest', { profile, id, key_profile: this.memoryProfile })
    if (policy.storage === 'keychain') {
      // Never put the key in argv, stderr or an exception message.
      const result = await this.execute('/usr/bin/security', ['-i'],
        `add-generic-password -U -s "${service}" -a "${providers[id].name}" -w "${key}"\n`)
      this.keychain.forget(providers[id].name)
      if (result.code === 0 && await this.keychain.read(providers[id].name) === key) {
        await this.call('ProviderSetRequest', { profile, id, remove: true })
        await this.provision(id)
        return 'keychain'
      }
      throw new Error(L('Keychain に API キーを保存できませんでした。'))
    }
    await this.call('ProviderSetRequest', { profile, id, provider: { api_key: key } })
    await this.memory(id, this.environment[providers[id].env], 'env')
    return 'agentd'
  }
  async delete(id: KeyedProvider, profile: string) {
    if (this.platform === 'darwin') {
      await this.execute('/usr/bin/security', ['delete-generic-password', '-s', service, '-a', providers[id].name])
      this.keychain.forget(providers[id].name)
    }
    await this.call('ProviderSetRequest', { profile, id, remove: true })
    await this.memory(id, undefined, 'keychain')
    await this.memory(id, this.environment[providers[id].env], 'env')
  }
}
