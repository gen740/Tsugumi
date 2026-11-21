import { onData } from './dom.js'

export default function mount(root, data) {
  const canvas = document.createElement('canvas')
  canvas.style.cssText = 'width:100%;height:100%;display:block;touch-action:none;cursor:grab'
  root.append(canvas)
  const ctx = canvas.getContext('2d')
  let points = [], colors, extent = 1
  let yaw = 0.6, pitch = 0.4, scale = 1
  let dragging

  function draw() {
    const w = Math.round(canvas.clientWidth * devicePixelRatio)
    const h = Math.round(canvas.clientHeight * devicePixelRatio)
    if (w < 3 || h < 3 || !points.length) return
    if (canvas.width !== w) canvas.width = w
    if (canvas.height !== h) canvas.height = h
    const image = ctx.createImageData(w, h)
    const pixels = new Uint32Array(image.data.buffer)
    const size = Math.min(w, h) * 0.42 * scale / extent
    const cosYaw = Math.cos(yaw), sinYaw = Math.sin(yaw)
    const cosPitch = Math.cos(pitch), sinPitch = Math.sin(pitch)
    const [x, y, z] = points
    for (let i = 0; i < x.length; i++) {
      const rx = x[i] * cosYaw + z[i] * sinYaw
      const rz = -x[i] * sinYaw + z[i] * cosYaw
      const ry = y[i] * cosPitch - rz * sinPitch
      const sx = Math.floor(w / 2 + rx * size), sy = Math.floor(h / 2 - ry * size)
      if (sx < 1 || sy < 1 || sx >= w - 1 || sy >= h - 1) continue
      const c = colors?.[i] ?? 0x4a90e2
      const pixel = 0xff000000 | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff)
      for (let dy = -1; dy <= 1; dy++) {
        const at = (sy + dy) * w + sx
        pixels.fill(pixel, at - 1, at + 2)
      }
    }
    ctx.putImageData(image, 0, 0)
  }

  onData(root, data, (_, blobs) => {
    extent = 1e-9
    points = ['x', 'y', 'z'].map(name => {
      const values = new Float32Array(blobs[name].buffer, blobs[name].byteOffset, blobs[name].byteLength / 4)
      const center = values.reduce((sum, v) => sum + v, 0) / Math.max(values.length, 1)
      return values.map(v => {
        const offset = v - center
        extent = Math.max(extent, Math.abs(offset))
        return offset
      })
    })
    colors = new Uint32Array(blobs.colors.buffer, blobs.colors.byteOffset, blobs.colors.byteLength / 4)
    canvas.ariaLabel = `${points[0].length} points`
    draw()
  })
  new ResizeObserver(draw).observe(canvas)
  canvas.onpointerdown = event => {
    canvas.setPointerCapture(event.pointerId)
    dragging = { x: event.clientX, y: event.clientY, yaw, pitch }
  }
  canvas.onlostpointercapture = () => { dragging = undefined }
  canvas.onpointermove = event => {
    if (!dragging) return
    yaw = dragging.yaw + (event.clientX - dragging.x) / 120
    pitch = Math.max(-1.5, Math.min(1.5, dragging.pitch + (event.clientY - dragging.y) / 120))
    draw()
  }
  canvas.addEventListener('wheel', event => {
    if (!event.ctrlKey && !event.metaKey) return
    event.preventDefault()
    scale = Math.max(0.2, Math.min(8, scale * Math.exp(-event.deltaY * 0.01)))
    draw()
  }, { passive: false })
}
