import source from './plotly.umd'
import { responsiveChart } from './responsive-chart.js'

;(0, eval)(source)
const Plotly = globalThis.Plotly

export default function mount(root, data) {
  return responsiveChart(root, data, (config, width, height) =>
    Plotly.react(root, config.data ?? [], { ...config.layout, width, height, autosize: false }, {
      responsive: false, displaylogo: false,
    }))
}
