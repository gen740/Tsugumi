// Line chart for Display values drawn with WebGPU (Dawn in Chromium), a port
// of ui/renderer: the same WGSL, the same triangle-strip geometry and colors.
// The chart is a full snapshot: new data only marks it dirty and drawing
// happens at most once per animation frame. WebGPU is required.
import { useEffect, useRef, useState } from 'react'
import type { LineSeries } from '../model/session'

const SHADER = /* wgsl */ `
struct VOut {
  @builtin(position) pos : vec4f,
  @location(0) color : vec4f,
};
@vertex fn vs(@location(0) p : vec2f, @location(1) c : vec4f) -> VOut {
  var o : VOut;
  o.pos = vec4f(p, 0.0, 1.0);
  o.color = c;
  return o;
}
@fragment fn fs(i : VOut) -> @location(0) vec4f {
  return i.color;
}
`

const PALETTE: [number, number, number][] = [[0.36, 0.67, 1.0], [1.0, 0.62, 0.26], [0.47, 0.86, 0.55], [0.94, 0.44, 0.62]]
const BG: [number, number, number] = [0.06, 0.07, 0.09]
const FLOATS_PER_VERTEX = 6

/** Appends a segment as two triangles of `px` pixel width (NDC coordinates). */
function segment(out: number[], x0: number, y0: number, x1: number, y1: number, px: number, w: number, h: number, c: number[]) {
  const dx = (x1 - x0) * w, dy = (y1 - y0) * h
  const len = Math.hypot(dx, dy)
  if (len < 1e-6) return
  // Normal in pixels, converted back to NDC per axis.
  const nx = (-dy / len) * px / w, ny = (dx / len) * px / h
  const v = (x: number, y: number) => out.push(x, y, c[0], c[1], c[2], c[3])
  v(x0 + nx, y0 + ny); v(x0 - nx, y0 - ny); v(x1 + nx, y1 + ny)
  v(x1 + nx, y1 + ny); v(x0 - nx, y0 - ny); v(x1 - nx, y1 - ny)
}

function build(series: LineSeries[], w: number, h: number, scale: number): Float32Array<ArrayBuffer> {
  const v: number[] = []
  if (!w || !h) return new Float32Array()
  let xmin = Infinity, xmax = -Infinity, ymin = Infinity, ymax = -Infinity
  for (const s of series) {
    const n = Math.min(s.x.length, s.y.length)
    for (let i = 0; i < n; i++) {
      xmin = Math.min(xmin, s.x[i]); xmax = Math.max(xmax, s.x[i])
      ymin = Math.min(ymin, s.y[i]); ymax = Math.max(ymax, s.y[i])
    }
  }
  // Plot area in NDC with a margin of 12 points.
  const mx = (12 * scale * 2) / w, my = (12 * scale * 2) / h
  const left = -1 + mx, right = 1 - mx, bottom = -1 + my, top = 1 - my
  const grid = [0.35, 0.38, 0.45, 1], faint = [0.25, 0.27, 0.32, 1]
  const hair = Math.max(1, scale) * 0.5
  segment(v, left, bottom, right, bottom, hair, w, h, grid)
  segment(v, left, bottom, left, top, hair, w, h, grid)
  for (let k = 1; k < 4; k++) {
    const y = bottom + ((top - bottom) * k) / 4
    segment(v, left, y, right, y, hair * 0.5, w, h, faint)
  }
  if (!(xmax > xmin)) { xmin -= 1; xmax += 1 }
  if (!(ymax > ymin)) { ymin -= 1; ymax += 1 }
  const pad = (ymax - ymin) * 0.05
  ymin -= pad
  ymax += pad
  const px = (x: number) => left + ((x - xmin) / (xmax - xmin)) * (right - left)
  const py = (y: number) => bottom + ((y - ymin) / (ymax - ymin)) * (top - bottom)
  const width = 1.25 * scale
  series.forEach((s, si) => {
    const c = [...PALETTE[si % PALETTE.length], 1]
    const n = Math.min(s.x.length, s.y.length)
    for (let i = 1; i < n; i++) segment(v, px(s.x[i - 1]), py(s.y[i - 1]), px(s.x[i]), py(s.y[i]), width, w, h, c)
  })
  return new Float32Array(v)
}

interface Renderer {
  resize(w: number, h: number, scale: number): void
  set(series: LineSeries[]): void
  render(): void
  destroy(): void
}

let devicePromise: Promise<GPUDevice | null> | undefined
/** One device shared by every chart of the page. */
function gpuDevice(): Promise<GPUDevice | null> {
  if (!devicePromise) {
    devicePromise = (async () => {
      if (!('gpu' in navigator)) return null
      try {
        const adapter = await navigator.gpu.requestAdapter()
        const device = await adapter?.requestDevice()
        device?.addEventListener('uncapturederror', (e) => console.error('tsugumi chart: WebGPU error', (e as GPUUncapturedErrorEvent).error.message))
        return device ?? null
      } catch {
        return null
      }
    })()
  }
  return devicePromise
}

async function webgpu(canvas: HTMLCanvasElement): Promise<Renderer | null> {
  const device = await gpuDevice()
  const ctx = canvas.getContext('webgpu') as GPUCanvasContext | null
  if (!device || !ctx) return null
  const format = navigator.gpu.getPreferredCanvasFormat()
  ctx.configure({ device, format, alphaMode: 'opaque' })
  const module = device.createShaderModule({ code: SHADER })
  const pipeline = device.createRenderPipeline({
    layout: 'auto',
    vertex: {
      module, entryPoint: 'vs',
      buffers: [{ arrayStride: FLOATS_PER_VERTEX * 4, attributes: [{ format: 'float32x2', offset: 0, shaderLocation: 0 }, { format: 'float32x4', offset: 8, shaderLocation: 1 }] }],
    },
    fragment: { module, entryPoint: 'fs', targets: [{ format }] },
    primitive: { topology: 'triangle-list' },
  })
  let series: LineSeries[] = []
  let w = 0, h = 0, scale = 1
  let buffer: GPUBuffer | undefined
  let capacity = 0
  let count = 0
  let upload = true
  return {
    resize(nw, nh, ns) {
      if (nw === w && nh === h && ns === scale) return
      w = nw; h = nh; scale = ns
      canvas.width = w
      canvas.height = h
      upload = true
    },
    set(s) {
      series = s
      upload = true
    },
    render() {
      if (!w || !h) return
      if (upload) {
        const verts = build(series, w, h, scale)
        if (verts.byteLength > capacity) {
          // Grow geometrically; the old buffer is released immediately.
          buffer?.destroy()
          capacity = Math.max(verts.byteLength, capacity * 2, 64 * 1024)
          buffer = device.createBuffer({ size: capacity, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST })
        }
        if (verts.byteLength && buffer) device.queue.writeBuffer(buffer, 0, verts)
        count = verts.length / FLOATS_PER_VERTEX
        upload = false
      }
      const enc = device.createCommandEncoder()
      const pass = enc.beginRenderPass({
        colorAttachments: [{ view: ctx.getCurrentTexture().createView(), loadOp: 'clear', storeOp: 'store', clearValue: { r: BG[0], g: BG[1], b: BG[2], a: 1 } }],
      })
      if (count && buffer) {
        pass.setPipeline(pipeline)
        pass.setVertexBuffer(0, buffer, 0, count * FLOATS_PER_VERTEX * 4)
        pass.draw(count)
      }
      pass.end()
      device.queue.submit([enc.finish()])
    },
    destroy() {
      buffer?.destroy()
      ctx.unconfigure()
    },
  }
}

export function DawnLineChart({ series }: { series: LineSeries[] }) {
  const ref = useRef<HTMLCanvasElement>(null)
  const r = useRef<Renderer | null>(null)
  const dirty = useRef(true)
  const latest = useRef(series)
  const [error, setError] = useState<string>()
  latest.current = series

  useEffect(() => {
    const canvas = ref.current!
    let raf = 0
    let alive = true
    let observer: ResizeObserver | undefined
    void (async () => {
      const renderer = await webgpu(canvas)
      if (!renderer) return setError('このブラウザでは WebGPU を使えないためグラフを表示できません（Chrome / Edge / Safari 26 以降で開いてください）。')
      if (!alive) return renderer.destroy()
      r.current = renderer
      const resize = () => {
        const rect = canvas.getBoundingClientRect()
        const scale = window.devicePixelRatio || 1
        renderer.resize(Math.max(1, Math.round(rect.width * scale)), Math.max(1, Math.round(rect.height * scale)), scale)
        dirty.current = true
      }
      observer = new ResizeObserver(resize)
      observer.observe(canvas)
      resize()
      renderer.set(latest.current)
      const tick = () => {
        if (dirty.current && r.current) {
          r.current.render()
          dirty.current = false
        }
        raf = requestAnimationFrame(tick)
      }
      raf = requestAnimationFrame(tick)
    })()
    return () => {
      alive = false
      cancelAnimationFrame(raf)
      observer?.disconnect()
      r.current?.destroy()
      r.current = null
    }
  }, [])

  useEffect(() => {
    r.current?.set(series)
    dirty.current = true
  }, [series])

  return (
    <div className="chart-box">
      <canvas ref={ref} className="chart" />
      {error && <div className="chart-error">{error}</div>}
    </div>
  )
}
