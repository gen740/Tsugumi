import source from './plotly.umd'
;(0, eval)(source)
const Plotly = globalThis.Plotly
let root, size, options
export async function initialise(element, data, geometry) {
  root = element; size = geometry
  await update(data)
}
export async function update({ config }) {
  options = config
  const layout = { ...config.layout, width: size.width, height: size.height, autosize: false }
  await Plotly.react(root, config.data ?? [], layout, { responsive: false, displaylogo: false })
}
export async function resize(geometry) { size = geometry; if (options) await update({ config: options }) }
export function dispose() { if (root) Plotly.purge(root) }
export function snapshot() { return Plotly.toImage(root, { format: 'png', width: size.width, height: size.height, scale: 2 }) }
