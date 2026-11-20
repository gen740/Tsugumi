// The page's api() over fetch and EventSource, as tests mock them: page models
// are exercised without hostd (src/host/routes.ts is tested on its own).
import { useApiTransport } from '../src/api'

useApiTransport({
  async call(path, body, signal) {
    const r = await fetch(path, body === undefined
      ? { signal, headers: { 'x-tsugumi': '1' } }
      : { signal, method: 'POST', headers: { 'content-type': 'application/json', 'x-tsugumi': '1' }, body: JSON.stringify(body) })
    const v = await r.json().catch(() => ({ error: `HTTP ${r.status}` }))
    if (!r.ok) throw Object.assign(new Error(v.error ?? `HTTP ${r.status}`), { code: v.code })
    return v
  },
  start(emit) {
    const Source = (globalThis as { EventSource?: new (url: string) => { onmessage?: (m: { data: string }) => void } }).EventSource
    if (!Source) return
    const source = new Source('/api/events')
    source.onmessage = (m) => {
      try { emit(JSON.parse(m.data)) } catch { /* not JSON */ }
    }
  },
})
