import { onData } from './dom.js'

export default function mount(root, data) {
  const label = document.createElement('div')
  const value = document.createElement('div')
  root.style.cssText += ';display:grid;align-content:center;justify-items:center;text-align:center'
  value.style.cssText = 'font-size:clamp(24px,9vmin,64px);font-variant-numeric:tabular-nums'
  root.append(label, value)
  onData(root, data, config => {
    label.textContent = config.label
    value.textContent = Number(config.value.toPrecision(8))
  })
}
