import { decodeSeries, palette, relativePosition, visibleRange } from './line-geometry.js'
import { onData } from './dom.js'

export default function mount(root, data) {
  const canvas = document.createElement('canvas')
  const legend = document.createElement('div')
  root.style.cssText += ';display:flex;flex-direction:column;overflow:hidden;font-size:11px'
  canvas.style.cssText = 'display:block;flex:1;min-height:0;width:100%'
  legend.style.cssText = 'display:flex;flex-wrap:wrap;gap:4px 12px;padding:2px 12px 6px'
  root.append(canvas, legend)
  const ctx = canvas.getContext('2d')
  const color = i => `rgb(${palette[i % palette.length].map(c => Math.round(c * 255)).join(',')})`
  let series = []
  let range
  function draw() {
    const w = canvas.clientWidth
    const h = canvas.clientHeight
    if (!range || !w || !h) return
    canvas.width = Math.round(w * devicePixelRatio)
    canvas.height = Math.round(h * devicePixelRatio)
    ctx.scale(devicePixelRatio, devicePixelRatio)
    const left = Math.min(12, w / 4), right = w - left
    const top = Math.min(12, h / 4), bottom = h - top
    ctx.strokeStyle = '#80858e'
    ctx.beginPath()
    ctx.moveTo(left, top)
    ctx.lineTo(left, bottom)
    ctx.lineTo(right, bottom)
    ctx.stroke()
    ctx.globalAlpha = 0.25
    ctx.beginPath()
    for (let i = 1; i < 4; i++) {
      const y = top + (bottom - top) * i / 4
      ctx.moveTo(left, y)
      ctx.lineTo(right, y)
    }
    ctx.stroke()
    ctx.globalAlpha = 1
    const norm = (v, a, b, span) => a === b ? 0.5 : relativePosition(v, a, span, b)
    for (const [i, line] of series.entries()) {
      ctx.strokeStyle = color(i)
      ctx.lineWidth = 2
      ctx.beginPath()
      for (let j = 0; j < line.x.length; j++) {
        const x = left + norm(line.x[j], range.xmin, range.xmax, range.xspan) * (right - left)
        const y = bottom - (0.05 + norm(line.y[j], range.ymin, range.ymax, range.yspan) * 0.9) * (bottom - top)
        if (j === 0) ctx.moveTo(x, y)
        else ctx.lineTo(x, y)
      }
      ctx.stroke()
    }
  }
  onData(root, data, (config, blobs) => {
    series = decodeSeries({ config, blobs })
    range = visibleRange(series)
    legend.replaceChildren(...series.map((line, i) => {
      const label = document.createElement('span')
      label.style.color = color(i)
      label.textContent = `${line.label} (${line.x.length})`
      return label
    }))
    draw()
  })
  new ResizeObserver(draw).observe(canvas)
}
