// Standard WebGPU only. Pointer input never leaves the frame or needs a worker.
let canvas, device, context, pipeline, uniform, bind, buffer, count = 0
let yaw = 0.6, pitch = 0.3, geometry, dragging, visible = true
export async function initialise(root, data, size) {
  if (!navigator.gpu) throw new Error('WebGPU is unavailable in this web view')
  const adapter = await navigator.gpu.requestAdapter()
  if (!adapter) throw new Error('WebGPU has no available adapter')
  device = await adapter.requestDevice()
  canvas = document.createElement('canvas')
  canvas.style.cssText = 'width:100%;height:100%;display:block;touch-action:none'
  root.append(canvas)
  context = canvas.getContext('webgpu')
  const format = navigator.gpu.getPreferredCanvasFormat()
  context.configure({ device, format, alphaMode: 'opaque' })
  const shader = device.createShaderModule({ code: `
    struct Camera { angles: vec2f, aspect: f32, unused: f32 }
    @group(0) @binding(0) var<uniform> camera: Camera;
    struct Vertex { @builtin(position) position: vec4f, @location(0) color: vec3f }
    @vertex fn vs(@location(0) p: vec3f) -> Vertex {
      let c = cos(camera.angles); let s = sin(camera.angles);
      let q = vec3f(c.x*p.x+s.x*p.z, p.y, -s.x*p.x+c.x*p.z);
      let r = vec3f(q.x, c.y*q.y-s.y*q.z, s.y*q.y+c.y*q.z);
      var out: Vertex;
      out.position = vec4f(r.x/camera.aspect, r.y, r.z*0.2+0.5, 1.0);
      out.color = p*0.4+vec3f(0.5);
      return out;
    }
    @fragment fn fs(in: Vertex) -> @location(0) vec4f { return vec4f(in.color, 1.0); }
  ` })
  pipeline = device.createRenderPipeline({ layout: 'auto', primitive: { topology: 'point-list' },
    vertex: { module: shader, entryPoint: 'vs', buffers: [{ arrayStride: 12, attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x3' }] }] },
    fragment: { module: shader, entryPoint: 'fs', targets: [{ format }] } })
  uniform = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST })
  bind = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: uniform } }] })
  canvas.onpointerdown = e => { canvas.setPointerCapture(e.pointerId); dragging = [e.clientX, e.clientY] }
  canvas.onpointerup = canvas.onpointercancel = () => { dragging = undefined }
  canvas.onpointermove = e => {
    if (!dragging) return
    yaw += (e.clientX - dragging[0]) * 0.01
    pitch += (e.clientY - dragging[1]) * 0.01
    dragging = [e.clientX, e.clientY]; draw()
  }
  geometry = size
  await update(data)
  resize(size)
}
export async function update({ config, blobs }) {
  const bytes = blobs.points
  if (!bytes || bytes.byteLength !== config.count * 12) throw new Error('Expected float32 XYZ triples')
  buffer?.destroy()
  buffer = device.createBuffer({ size: Math.max(4, bytes.byteLength), usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST })
  device.queue.writeBuffer(buffer, 0, bytes)
  count = config.count
  draw()
}
export function resize(size) {
  geometry = size; visible = size.visible
  canvas.width = Math.max(1, Math.round(size.width * size.pixelRatio))
  canvas.height = Math.max(1, Math.round(size.height * size.pixelRatio))
  draw()
}
function draw() {
  if (!device || !buffer || !visible) return
  device.queue.writeBuffer(uniform, 0, new Float32Array([yaw, pitch, geometry.width / Math.max(1, geometry.height), 0]))
  const encoder = device.createCommandEncoder()
  const pass = encoder.beginRenderPass({ colorAttachments: [{ view: context.getCurrentTexture().createView(), clearValue: [0.03, 0.04, 0.07, 1], loadOp: 'clear', storeOp: 'store' }] })
  pass.setPipeline(pipeline); pass.setBindGroup(0, bind); pass.setVertexBuffer(0, buffer); pass.draw(count); pass.end()
  device.queue.submit([encoder.finish()])
}
export async function snapshot() { draw(); await device.queue.onSubmittedWorkDone(); return canvas.toDataURL('image/png') }
export function dispose() { buffer?.destroy(); uniform?.destroy(); device?.destroy(); canvas?.remove() }
