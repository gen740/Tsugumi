import { onData } from './dom.js'

export default function mount(root, data) {
  const text = document.createElement('pre')
  text.style.cssText = 'padding:8px;margin:0;white-space:pre-wrap;overflow-wrap:anywhere'
  root.append(text)
  onData(root, data, config => { text.textContent = config.text })
}
