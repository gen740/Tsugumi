import vegaSource from './vega.umd'
import liteSource from './vega-lite.umd'
import embedSource from './vega-embed.umd'
for (const source of [vegaSource, liteSource, embedSource]) (0, eval)(source)
const vega = globalThis.vega, embed = globalThis.vegaEmbed
let root, result, geometry
export async function initialise(element, data, size) { root = element; geometry = size; await update(data) }
export async function update({ config }) {
  dispose()
  const spec = structuredClone(config)
  if (!['hconcat', 'vconcat', 'concat', 'facet', 'repeat'].some(k => k in spec)) {
    spec.width = Math.max(1, geometry.width - 40); spec.height = Math.max(1, geometry.height - 40)
    spec.autosize = { type: 'fit', contains: 'padding' }
  }
  const loader = vega.loader()
  loader.load = loader.sanitize = async () => { throw new Error('Put data in the display; network is disabled') }
  // Bundling the UMD entry avoids external imports and fixes its library versions.
  result = await embed(root, spec, { loader, actions: false, renderer: 'canvas' })
}
export function resize(size) { geometry = size; result?.view.width(size.width).height(size.height).resize().run() }
export function dispose() { result?.finalize(); result = undefined; root?.replaceChildren() }
export function snapshot() { return result.view.toImageURL('png', 2) }
