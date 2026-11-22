// Serialize asynchronous chart renders. Data and size can both change while
// a chart library is rendering, so keep the latest inputs for its next draw.
export async function responsiveChart(root, data, render) {
  let config = JSON.parse(data.config)
  let pending = false
  let working = false
  async function draw() {
    pending = true
    if (working) return
    working = true
    try {
      while (pending) {
        pending = false
        await render(config, root.clientWidth, root.clientHeight)
      }
    } finally {
      working = false
    }
  }
  const refresh = () => { void draw().catch(error => { root.textContent = String(error.message ?? error) }) }
  root.addEventListener('tsugumi:data', event => {
    config = JSON.parse(event.detail.config)
    refresh()
  })
  await draw()
  new ResizeObserver(refresh).observe(root)
}
