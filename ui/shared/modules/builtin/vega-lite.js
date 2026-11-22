import vegaSource from './vega.umd'
import liteSource from './vega-lite.umd'
import embedSource from './vega-embed.umd'
import { responsiveChart } from './responsive-chart.js'

for (const source of [vegaSource, liteSource, embedSource]) (0, eval)(source)

export default function mount(root, data) {
  const loader = globalThis.vega.loader()
  loader.load = loader.sanitize = async () => { throw new Error('Put data in the display; network is disabled') }
  let result
  let last
  return responsiveChart(root, data, async (config, width, height) => {
    const simple = !['hconcat', 'vconcat', 'concat', 'facet', 'repeat'].some(key => key in config)
    if (config !== last) {
      result?.finalize()
      const spec = simple ? {
        ...config, width: Math.max(1, width - 40), height: Math.max(1, height - 40),
        autosize: { type: 'fit', contains: 'padding' },
      } : config
      result = await globalThis.vegaEmbed(root, spec, { loader, actions: false, renderer: 'canvas' })
      last = config
    } else if (simple) {
      await result.view.width(Math.max(1, width - 40)).height(Math.max(1, height - 40)).resize().runAsync()
    }
  })
}
