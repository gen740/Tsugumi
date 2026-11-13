import { RPCClient, R } from '../shared/rpc'

/** Immutable code is shared across displays, never attached to run data. */
const clients: WeakRef<RPCClient>[] = []
const cache = new Map<string, Promise<string>>()
export function registerModuleClient(client: RPCClient) { clients.push(new WeakRef(client)) }
export function moduleSource(identity: string): Promise<string> {
  if (!cache.has(identity)) {
    const load = async () => {
      let error: unknown = new Error('No module connection')
      for (const weak of [...clients].reverse()) {
        const client = weak.deref()
        if (!client) continue
        try {
          const parts: Uint8Array[] = []
          let offset = 0n, size = 0
          for (;;) {
            const page = await client.call<R.FileContentT>(R.RequestPayload.ModuleReadRequest,
              Object.assign(new R.ModuleReadRequestT(), { module: identity, offset, limitBytes: 1024 * 1024 }))
            const bytes = Uint8Array.from(page.bytes)
            size += bytes.length
            if (size > 32 * 1024 * 1024 || page.offset !== offset) throw new Error('Invalid module page')
            parts.push(bytes)
            if (page.eof) break
            if (page.nextOffset === null || page.nextOffset <= offset) throw new Error('Invalid module offset')
            offset = page.nextOffset
          }
          const result = new Uint8Array(size)
          let cursor = 0
          for (const part of parts) { result.set(part, cursor); cursor += part.length }
          return new TextDecoder('utf-8', { fatal: true }).decode(result)
        } catch (e) { error = e }
      }
      throw error
    }
    const pending = load().catch(error => { cache.delete(identity); throw error })
    // Keep the code cache bounded by entry count; live frames own their loaded code.
    if (cache.size >= 8) cache.delete(cache.keys().next().value!)
    cache.set(identity, pending)
  }
  return cache.get(identity)!
}
export type ModuleBlobs = Record<string, Uint8Array>
