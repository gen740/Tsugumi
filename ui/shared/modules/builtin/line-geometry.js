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
