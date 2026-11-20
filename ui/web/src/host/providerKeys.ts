import { L } from '../i18n'

type Call = <T = any>(method: string, fields: Record<string, unknown>) => Promise<T>
export type KeyedProvider = 'anthropic' | 'openai'

/** API keys live in hostd on every platform: the page saves, removes and
 *  asks about them there and never keeps a copy. */
export class ProviderKeys {
  constructor(private readonly call: Call) {}
  async source(id: KeyedProvider, profile: string): Promise<'hostd' | undefined> {
    const status = await this.call<{ source?: 'hostd' }>('ProviderStatusRequest', { profile, id })
    return status.source === 'hostd' ? 'hostd' : undefined
  }
  async save(id: KeyedProvider, raw: string, profile: string): Promise<void> {
    const key = raw.trim()
    if (!/^[\x21-\x7e]+$/.test(key)) throw new Error(L('API キーに使えない文字が含まれています。'))
    await this.call('ProviderSetRequest', { profile, id, provider: { api_key: key } })
  }
  async delete(id: KeyedProvider, profile: string) {
    await this.call('ProviderSetRequest', { profile, id, remove: true })
  }
}
