import { buildVertices, decodeSeries, palette } from './line-geometry.js'

const shader = `
struct Vertex { @builtin(position) position: vec4f, @location(0) color: vec4f }
@vertex fn vs(@location(0) p: vec2f, @location(1) color: vec4f) -> Vertex {
  var result: Vertex; result.position = vec4f(p, 0.0, 1.0); result.color = color; return result;
}
@fragment fn fs(vertex: Vertex) -> @location(0) vec4f { return vertex.color; }
`
let canvas, legend, device, context, pipeline, buffer, capacity = 0, geometry, series = [], dark = false
export async function initialise(root, data, size) {
  if (!navigator.gpu) throw new Error('WebGPU is unavailable in this web view')
  const adapter = await navigator.gpu.requestAdapter()
  if (!adapter) throw new Error('WebGPU has no available adapter')
  device = await adapter.requestDevice()
  canvas = document.createElement('canvas')
  legend = document.createElement('div')
  root.style.cssText = 'position:absolute;inset:0;display:flex;flex-direction:column;overflow:hidden;font:11px system-ui'
  canvas.style.cssText = 'display:block;min-height:0;flex:1;width:100%'
  legend.style.cssText = 'display:flex;flex-wrap:wrap;gap:4px 12px;padding:2px 12px 6px'
  root.append(canvas, legend)
  context = canvas.getContext('webgpu')
  const format = navigator.gpu.getPreferredCanvasFormat()
  context.configure({ device, format, alphaMode: 'opaque' })
  const module = device.createShaderModule({ code: shader })
  pipeline = device.createRenderPipeline({ layout: 'auto',
    vertex: { module, entryPoint: 'vs', buffers: [{ arrayStride: 24, attributes: [
      { format: 'float32x2', offset: 0, shaderLocation: 0 }, { format: 'float32x4', offset: 8, shaderLocation: 1 },
    ] }] }, fragment: { module, entryPoint: 'fs', targets: [{ format }] }, primitive: { topology: 'triangle-list' } })
  geometry = size
  await update(data)
}
export async function update(data) {
  series = decodeSeries(data)
  legend.replaceChildren()
  series.forEach((s, index) => {
    const label = document.createElement('span')
    const color = palette[index % palette.length].map(c => Math.round(c * 255))
    label.style.color = `rgb(${color.join(',')})`
    label.textContent = `${s.label} (${s.x.length})`
    legend.append(label)
  })
  resize(geometry)
}
export function resize(size) {
  geometry = size; dark = size.colorScheme === 'dark'
  canvas.parentElement.style.background = dark ? '#101217' : '#ffffff'
  // Measure after laying out the legend; snapshots and live views use the same geometry.
  canvas.width = Math.max(1, Math.round(size.width * size.pixelRatio))
  canvas.height = Math.max(1, Math.round(Math.max(1, size.height - legend.offsetHeight) * size.pixelRatio))
  draw()
}
function draw() {
  if (!geometry.visible || !device) return
  const vertices = buildVertices(series, canvas.width, canvas.height, geometry.pixelRatio, dark)
  if (vertices.byteLength > capacity) {
    buffer?.destroy()
    capacity = Math.max(vertices.byteLength, capacity * 2, 65536)
    buffer = device.createBuffer({ size: capacity, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST })
  }
  if (vertices.byteLength) device.queue.writeBuffer(buffer, 0, vertices)
  const encoder = device.createCommandEncoder()
  const pass = encoder.beginRenderPass({ colorAttachments: [{ view: context.getCurrentTexture().createView(),
    clearValue: dark ? [0.06, 0.07, 0.09, 1] : [1, 1, 1, 1], loadOp: 'clear', storeOp: 'store' }] })
  if (vertices.length) { pass.setPipeline(pipeline); pass.setVertexBuffer(0, buffer); pass.draw(vertices.length / 6) }
  pass.end(); device.queue.submit([encoder.finish()])
}
export async function snapshot() {
  draw(); await device.queue.onSubmittedWorkDone()
  // Composite the module's own legend with the GPU canvas, without a host drawing API.
  const out = document.createElement('canvas'), scale = geometry.pixelRatio
  out.width = canvas.width; out.height = Math.max(1, Math.round(geometry.height * scale))
  const ctx = out.getContext('2d')
  ctx.fillStyle = dark ? '#101217' : '#ffffff'; ctx.fillRect(0, 0, out.width, out.height)
  ctx.drawImage(canvas, 0, 0)
  ctx.font = `${11 * scale}px system-ui`; ctx.textBaseline = 'top'
  for (const label of legend.children) {
    const box = label.getBoundingClientRect(), parent = legend.parentElement.getBoundingClientRect()
    ctx.fillStyle = label.style.color; ctx.fillText(label.textContent, (box.left - parent.left) * scale, (box.top - parent.top) * scale)
  }
  return out.toDataURL('image/png')
}
export function dispose() {
  buffer?.destroy(); context?.unconfigure(); device?.destroy()
  canvas?.remove(); legend?.remove()
  canvas = legend = device = context = pipeline = buffer = undefined
  capacity = 0; series = []
}
