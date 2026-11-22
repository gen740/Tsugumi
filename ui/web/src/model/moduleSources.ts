/* Viewer assets retained for the existing UI until the HTML host cutover. */
const cache = new Map<string, Promise<string>>()
export function moduleSource(identity: string): Promise<string> {
  if (!cache.has(identity)) {
    const load = async () => {
      if (/^tsugumi:(text|markdown|scalar|table|image|points|line-chart|plotly|vega-lite)$/.test(identity)) {
        const response = await fetch(`/modules/${identity.slice('tsugumi:'.length)}.js`)
        if (!response.ok || !response.headers.get('content-type')?.includes('javascript')) throw new Error('Display module is unavailable')
        return response.text()
      }
      throw new Error('Display module is unavailable')
    }
    const pending = load().catch(error => { cache.delete(identity); throw error })
    // Keep the code cache bounded by entry count; live frames own their loaded code.
    if (cache.size >= 8) cache.delete(cache.keys().next().value!)
    cache.set(identity, pending)
  }
  return cache.get(identity)!
}
export type ModuleBlobs = Record<string, Uint8Array>
