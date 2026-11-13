// Pure CPU geometry. JavaScript numbers retain the input's double precision
// until the visible range's origin has been subtracted and scaled to NDC.
export const palette = [[0.36, 0.67, 1.0], [1.0, 0.62, 0.26], [0.47, 0.86, 0.55], [0.94, 0.44, 0.62]]

export function decodeSeries({ config, blobs }) {
  const read = name => {
    const bytes = blobs[name]
    if (!(bytes instanceof Uint8Array) || bytes.byteLength % 8) throw new Error(`Invalid ${name} float64 blob`)
    const data = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
    return Array.from({ length: bytes.byteLength / 8 }, (_, i) => data.getFloat64(i * 8, true))
  }
  const x = read('x'), y = read('y')
  if (x.length !== y.length || x.length > 10000 || !Array.isArray(config.series)) throw new Error('Invalid line chart')
  let end = 0
  const series = config.series.map(row => {
    if (!Number.isSafeInteger(row.offset) || row.offset !== end || !Number.isSafeInteger(row.count) || row.count < 0 || row.count > x.length - end)
      throw new Error('Invalid series range')
    end += row.count
    return { id: row.id, label: row.label, x: x.slice(row.offset, end), y: y.slice(row.offset, end) }
  })
  if (end !== x.length || !x.every(Number.isFinite) || !y.every(Number.isFinite)) throw new Error('Invalid line coordinates')
  return series
}

export function visibleRange(series) {
  let xmin = Infinity, xmax = -Infinity, ymin = Infinity, ymax = -Infinity
  for (const s of series) for (let i = 0; i < s.x.length; i++) {
    xmin = Math.min(xmin, s.x[i]); xmax = Math.max(xmax, s.x[i])
    ymin = Math.min(ymin, s.y[i]); ymax = Math.max(ymax, s.y[i])
  }
  if (xmin === Infinity) return { xmin: 0, xmax: 1, ymin: 0, ymax: 1 }
  // A constant, even near Number.MAX_VALUE, has a finite local span. Never
  // add a tiny epsilon to a large absolute value: that addition can round away.
  const xspan = xmax - xmin || Math.max(1, Math.abs(xmin) * Number.EPSILON)
  const yspan = ymax - ymin || Math.max(1, Math.abs(ymin) * Number.EPSILON)
  return { xmin, xmax, ymin, ymax, xspan, yspan }
}

export function relativePosition(value, origin, span, end) {
  // Split only the exceptional overflow case; the usual subtraction is f64.
  return Number.isFinite(span) ? (value - origin) / span : (value / 2 - origin / 2) / (end / 2 - origin / 2)
}

export function buildVertices(series, width, height, scale = 1, dark = false, range = visibleRange(series)) {
  const vertices = []
  if (width < 1 || height < 1) return new Float32Array()
  const segment = (x0, y0, x1, y1, px, color) => {
    const dx = (x1 - x0) * width, dy = (y1 - y0) * height, length = Math.hypot(dx, dy)
    if (length < 1e-6) return
    const nx = -dy / length * px / width, ny = dx / length * px / height
    const v = (x, y) => vertices.push(x, y, ...color)
    v(x0 + nx, y0 + ny); v(x0 - nx, y0 - ny); v(x1 + nx, y1 + ny)
    v(x1 + nx, y1 + ny); v(x0 - nx, y0 - ny); v(x1 - nx, y1 - ny)
  }
  const mx = Math.min(0.8, 24 * scale / width), my = Math.min(0.8, 24 * scale / height)
  const left = -1 + mx, right = 1 - mx, bottom = -1 + my, top = 1 - my
  const grid = dark ? [0.35, 0.38, 0.45, 1] : [0.55, 0.58, 0.65, 1]
  const faint = dark ? [0.25, 0.27, 0.32, 1] : [0.87, 0.88, 0.9, 1]
  segment(left, bottom, right, bottom, scale * 0.5, grid)
  segment(left, bottom, left, top, scale * 0.5, grid)
  for (let k = 1; k < 4; ++k) {
    const y = bottom + (top - bottom) * k / 4
    segment(left, y, right, y, scale * 0.25, faint)
  }
  const xspan = range.xspan ?? (range.xmax - range.xmin || 1)
  const yspan = range.yspan ?? (range.ymax - range.ymin || 1)
  const px = value => range.xmin === range.xmax ? (left + right) / 2 : left + relativePosition(value, range.xmin, xspan, range.xmax) * (right - left)
  // Apply padding after origin subtraction, so tiny differences at large
  // magnitudes are not lost by expanding the absolute range first.
  const py = value => range.ymin === range.ymax ? (top + bottom) / 2 : bottom + ((relativePosition(value, range.ymin, yspan, range.ymax) + 0.05) / 1.1) * (top - bottom)
  series.forEach((s, index) => {
    const color = [...palette[index % palette.length], 1]
    for (let i = 1; i < s.x.length; ++i) segment(px(s.x[i - 1]), py(s.y[i - 1]), px(s.x[i]), py(s.y[i]), 1.25 * scale, color)
  })
  return new Float32Array(vertices)
}
