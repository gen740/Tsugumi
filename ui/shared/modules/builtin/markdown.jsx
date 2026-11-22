import { createRoot } from 'react-dom/client'
import { Markdown } from '../../../web/src/ui/markdown'
import { onData } from './dom.js'

export default function mount(root, data) {
  root.style.padding = '0 10px'
  root.addEventListener('click', event => {
    if (event.target.closest('a')) event.preventDefault()
  })
  const view = createRoot(root)
  onData(root, data, config => view.render(<Markdown text={config.text} />))
}
