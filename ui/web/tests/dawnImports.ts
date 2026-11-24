import assert from 'node:assert/strict'
import { Handles, TrapError, importNames, textureFormatValue, webgpuImports } from '../shared/gen/webgpu_imports.gen.js'

function fixture() {
  const memory = new WebAssembly.Memory({ initial: 1 })
  const handles = new Handles()
  const imports = webgpuImports({ memory: () => memory.buffer, handles })
  const u32 = (at: number, value: number) => new DataView(memory.buffer).setUint32(at, value, true)
  const u64 = (at: number, value: bigint) => new DataView(memory.buffer).setBigUint64(at, value, true)
  const text = (at: number, value: string) => new Uint8Array(memory.buffer).set(new TextEncoder().encode(value), at)
  return { memory, handles, imports, u32, u64, text }
}

function trap(call: () => unknown, message: RegExp) {
  assert.throws(call, (error: unknown) => error instanceof TrapError && message.test(error.message))
}

assert.equal(importNames.length, 49)
assert.equal(textureFormatValue.get('bgra8unorm'), 0x1b)

{
  const { handles, imports, u32, u64, text } = fixture()
  const shader = {}
  const pipeline = {}
  const deviceId = handles.add('Device', {
    createRenderPipeline(descriptor: Record<string, unknown>) {
      assert.equal(descriptor.layout, 'auto')
      assert.deepEqual(descriptor.vertex, {
        module: shader,
        entryPoint: 'vs',
        constants: { override: 2.5 },
        buffers: [{ stepMode: 'instance', arrayStride: 16, attributes: [{ format: 'float32x3', offset: 4, shaderLocation: 2 }] }],
      })
      assert.deepEqual(descriptor.fragment, {
        module: shader,
        constants: {},
        targets: [{ format: 'bgra8unorm', writeMask: 15 }, null],
      })
      assert.deepEqual(descriptor.depthStencil, {
        format: 'depth24plus',
        depthWriteEnabled: true,
        depthCompare: 'less',
        stencilFront: {},
        stencilBack: {},
        stencilReadMask: 0,
        stencilWriteMask: 0,
        depthBias: 0,
        depthBiasSlopeScale: 0,
        depthBiasClamp: 0,
      })
      return pipeline
    },
  })
  const shaderId = handles.add('ShaderModule', shader)
  const descriptor = 8
  const vertex = descriptor + 16
  const buffer = 128
  const attribute = 160
  const depth = 192
  const fragment = 272
  const target = 312
  const name = 384
  const constant = 400
  const constantName = 432
  u32(descriptor + 72, depth)
  u32(descriptor + 92, fragment)
  u32(vertex + 4, shaderId)
  u32(vertex + 8, name)
  u32(vertex + 12, 0xffffffff)
  u32(vertex + 16, 1)
  u32(vertex + 20, constant)
  u32(vertex + 24, 1)
  u32(vertex + 28, buffer)
  text(name, 'vs')
  u32(constant + 4, constantName)
  u32(constant + 8, 8)
  u64(constant + 16, 0x4004000000000000n) // f64 2.5.
  text(constantName, 'override')
  u32(buffer + 4, 2) // Instance step mode.
  u64(buffer + 8, 16n)
  u32(buffer + 16, 1)
  u32(buffer + 20, attribute)
  u32(attribute + 4, 0x1e) // Float32x3.
  u64(attribute + 8, 4n)
  u32(attribute + 16, 2)
  u32(depth + 4, 0x2e) // Depth24Plus.
  u32(depth + 8, 1)
  u32(depth + 12, 2) // Less.
  u32(fragment + 4, shaderId)
  u32(fragment + 12, 0xffffffff)
  u32(fragment + 24, 2)
  u32(fragment + 28, target)
  u32(target + 4, 0x1b) // BGRA8Unorm.
  u64(target + 16, 0xfn)
  const id = imports.wgpuDeviceCreateRenderPipeline(deviceId, descriptor)
  assert.equal(handles.get('RenderPipeline', id), pipeline)
  u32(attribute + 4, 0x7fffffff)
  trap(() => imports.wgpuDeviceCreateRenderPipeline(deviceId, descriptor),
    /wgpuDeviceCreateRenderPipeline: WGPUVertexAttribute.format: unknown enum/)
}

{
  const { handles, imports, u32, u64 } = fixture()
  const layout = {}
  const buffer = {}
  const sampler = {}
  const group = {}
  const deviceId = handles.add('Device', {
    createBindGroup(descriptor: unknown) {
      assert.deepEqual(descriptor, {
        label: '',
        layout,
        entries: [
          { binding: 2, resource: { buffer, offset: 256 } },
          { binding: 6, resource: sampler },
        ],
      })
      return group
    },
    createBindGroupLayout(descriptor: unknown) {
      assert.deepEqual(descriptor, {
        label: '',
        entries: [{ binding: 4, visibility: 1, buffer: { hasDynamicOffset: false, minBindingSize: 0 } }],
      })
      return layout
    },
  })
  const descriptor = 8
  const first = 64
  const second = first + 40
  const layoutDescriptor = 192
  const layoutEntry = 256
  u32(layoutDescriptor + 12, 1)
  u32(layoutDescriptor + 16, layoutEntry)
  u32(layoutEntry + 4, 4)
  u64(layoutEntry + 8, 1n) // Vertex visibility.
  u32(layoutEntry + 28, 1) // Undefined buffer binding type defaults to uniform.
  const layoutId = imports.wgpuDeviceCreateBindGroupLayout(deviceId, layoutDescriptor)
  const bufferId = handles.add('Buffer', buffer)
  u32(descriptor + 12, layoutId)
  u32(descriptor + 16, 2)
  u32(descriptor + 20, first)
  u32(first + 4, 2)
  u32(first + 8, bufferId)
  u64(first + 16, 256n)
  u64(first + 24, 0xffffffffffffffffn)
  u32(second + 4, 6)
  u32(second + 32, handles.add('Sampler', sampler))
  const id = imports.wgpuDeviceCreateBindGroup(deviceId, descriptor)
  assert.equal(handles.get('BindGroup', id), group)
  for (const invalid of [0, 999, bufferId]) {
    u32(descriptor + 12, invalid)
    trap(() => imports.wgpuDeviceCreateBindGroup(deviceId, descriptor),
      /wgpuDeviceCreateBindGroup: WGPUBindGroupDescriptor.layout: .*handle id/)
  }
  u32(descriptor + 12, layoutId)
  u32(descriptor + 16, 0xffffffff)
  trap(() => imports.wgpuDeviceCreateBindGroup(deviceId, descriptor),
    /wgpuDeviceCreateBindGroup: WGPUBindGroupDescriptor.entries: out of bounds/)
}

{
  const { handles, imports, memory, u32, text } = fixture()
  const deviceId = handles.add('Device', {
    createShaderModule(descriptor: unknown) {
      assert.deepEqual(descriptor, { label: 'name', code: 'fn main() {}' })
      return null
    },
  })
  const descriptor = 8
  const chain = 32
  u32(descriptor, chain)
  u32(descriptor + 4, 64)
  u32(descriptor + 8, 4)
  text(64, 'name-extra')
  u32(chain + 4, 2) // WGPUShaderSourceWGSL.
  u32(chain + 8, 80)
  u32(chain + 12, 0xffffffff)
  text(80, 'fn main() {}')
  assert.equal(imports.wgpuDeviceCreateShaderModule(deviceId, descriptor), 0)
  u32(chain + 4, 1)
  trap(() => imports.wgpuDeviceCreateShaderModule(deviceId, descriptor),
    /wgpuDeviceCreateShaderModule: .*nextInChain: expected WGPUShaderSourceWGSL/)
  u32(chain + 4, 2)
  u32(chain, 0xfffffffc)
  trap(() => imports.wgpuDeviceCreateShaderModule(deviceId, descriptor),
    /wgpuDeviceCreateShaderModule: .*nextInChain: unexpected chained extension/)
  u32(chain, 0)
  const last = memory.buffer.byteLength - 1
  u32(chain + 8, last)
  text(last, 'x')
  trap(() => imports.wgpuDeviceCreateShaderModule(deviceId, descriptor),
    /wgpuDeviceCreateShaderModule: WGPUShaderSourceWGSL.code: unterminated string/)
  trap(() => imports.wgpuDeviceCreateCommandEncoder(deviceId, descriptor),
    /wgpuDeviceCreateCommandEncoder: .*nextInChain: unexpected chained extension/)
  // The view ABI requires WGSL source for every shader module.
  u32(descriptor, 0)
  trap(() => imports.wgpuDeviceCreateShaderModule(deviceId, descriptor),
    /wgpuDeviceCreateShaderModule: .*nextInChain: missing WGPUShaderSourceWGSL/)
}

{
  const { handles, imports, u32, u64 } = fixture()
  let created = 0
  const deviceId = handles.add('Device', {
    createBindGroupLayout(descriptor: { entries: { binding: number }[] }) {
      assert.equal(descriptor.entries[0].binding, 3)
      created++
      return {}
    },
  })
  const descriptor = 8
  const entry = 64
  u32(descriptor + 12, 1)
  u32(descriptor + 16, entry)
  u32(entry + 4, 3)
  u64(entry + 8, 2n)
  imports.wgpuDeviceCreateBindGroupLayout(deviceId, descriptor)
  assert.equal(created, 1)
  u32(entry + 16, 4) // bindingArraySize
  trap(() => imports.wgpuDeviceCreateBindGroupLayout(deviceId, descriptor),
    /WGPUBindGroupLayoutEntry.bindingArraySize: binding arrays are not in the view ABI/)
}

{
  const { handles, imports, memory, u32 } = fixture()
  let writes = 0
  let destroys = 0
  const buffer = { destroy: () => ++destroys }
  const texture = {}
  const bufferId = handles.add('Buffer', buffer)
  const textureId = handles.add('Texture', texture)
  const queueId = handles.add('Queue', {
    writeBuffer(actual: object, offset: number, data: Uint8Array) {
      ++writes
      assert.equal(actual, buffer)
      assert.equal(offset, 2 ** 34)
      assert.equal(data.buffer, memory.buffer)
      assert.equal(data.byteOffset, memory.buffer.byteLength - 4)
      assert.deepEqual([...data], [1, 2, 3, 4])
    },
    writeTexture(destination: unknown, data: Uint8Array, layout: unknown, size: unknown) {
      assert.deepEqual(destination, { texture, mipLevel: 0, origin: { x: 0, y: 0, z: 0 } })
      assert.equal(data.buffer, memory.buffer)
      assert.equal(data.byteLength, 4)
      assert.deepEqual(layout, { offset: 0 })
      assert.deepEqual(size, { width: 1, height: 1, depthOrArrayLayers: 1 })
    },
  })
  memory.grow(1)
  const data = memory.buffer.byteLength - 4
  new Uint8Array(memory.buffer).set([1, 2, 3, 4], data)
  imports.wgpuQueueWriteBuffer(queueId, bufferId, 1n << 34n, data, 4)
  trap(() => imports.wgpuQueueWriteBuffer(queueId, bufferId, 0n, data, 5),
    /wgpuQueueWriteBuffer: data: out of bounds/)
  trap(() => imports.wgpuQueueWriteBuffer(queueId, bufferId, 0n, -4, 8),
    /wgpuQueueWriteBuffer: data: out of bounds/)
  assert.equal(writes, 1)
  const destination = 32
  const layout = 64
  const size = 80
  u32(destination, textureId)
  u32(layout + 8, 0xffffffff)
  u32(layout + 12, 0xffffffff)
  u32(size, 1)
  u32(size + 4, 1)
  u32(size + 8, 1)
  imports.wgpuQueueWriteTexture(queueId, destination, data, 4, layout, size)
  imports.wgpuBufferDestroy(bufferId)
  assert.equal(destroys, 1)
  assert.equal(handles.get('Buffer', bufferId), buffer)
  imports.wgpuBufferRelease(bufferId)
  trap(() => imports.wgpuBufferRelease(bufferId), /wgpuBufferRelease: buffer: unknown WebGPU handle/)
  assert.notEqual(handles.add('Buffer', buffer), bufferId)
}
