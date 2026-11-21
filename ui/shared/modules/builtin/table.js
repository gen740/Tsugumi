import { onData } from './dom.js'

const pageSize = 100

export default function mount(root, data) {
  const table = document.createElement('table')
  table.style.cssText = 'border-spacing:12px 3px;padding:4px;white-space:pre-wrap;font-size:12px;text-align:left'
  const header = table.createTHead()
  const body = table.createTBody()
  const navigation = document.createElement('div')
  navigation.style.cssText = 'display:flex;align-items:center;gap:8px;padding:4px 12px'
  const range = document.createElement('span')
  const previous = document.createElement('button')
  const next = document.createElement('button')
  previous.textContent = '‹'
  next.textContent = '›'
  previous.ariaLabel = 'Previous page'
  next.ariaLabel = 'Next page'
  navigation.append(previous, range, next)
  root.append(navigation, table)
  let page = 0
  let rows = []
  const show = () => {
    const start = page * pageSize
    const end = Math.min(start + pageSize, rows.length)
    navigation.hidden = rows.length <= pageSize
    navigation.style.display = navigation.hidden ? 'none' : 'flex'
    range.textContent = `${start + 1}–${end} / ${rows.length}`
    previous.disabled = page === 0
    next.disabled = end === rows.length
    body.replaceChildren()
    for (const cells of rows.slice(start, end)) {
      const row = body.insertRow()
      for (const cell of cells) row.insertCell().textContent = cell
    }
  }
  previous.onclick = () => { page--; show() }
  next.onclick = () => { page++; show() }
  onData(root, data, config => {
    header.replaceChildren()
    const row = header.insertRow()
    for (const column of config.columns) {
      const cell = document.createElement('th')
      cell.scope = 'col'
      cell.textContent = column
      row.append(cell)
    }
    rows = config.rows
    page = Math.min(page, Math.max(0, Math.ceil(rows.length / pageSize) - 1))
    show()
  })
}
