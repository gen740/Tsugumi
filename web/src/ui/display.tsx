// Display values (text, Markdown, scalar, line chart, table, image, point
// cloud, module) on the canvas at its zoom, or larger in the expanded viewer.
import { CSSProperties, ReactNode, useEffect, useMemo, useRef, useState } from 'react'
import type { DisplaySnapshot } from '../model/session'
import { arr, dbl, get, serialize, tryParse } from '../shared/json'
import { DawnLineChart } from './chart'
import { Button, Empty } from './controls'
import { tablePage } from '../model/tablePage'
import { Icon } from './icons'
import { Markdown } from './markdown'

/** Renders a Display value at `zoom` (on the canvas) or larger (`large`, the expanded viewer). */
export function DisplayContent({ display, large = false, zoom = 1 }: { display?: DisplaySnapshot; large?: boolean; zoom?: number }) {
  const size = (small: number, big: number) => (large ? big : small) * zoom
  // Scrolls in the large viewer; on the canvas (where the wheel moves the
  // canvas) the content is cut at the frame, with no scroll bar.
  const scrolling = (children: ReactNode, both = false) => (
    <div className={`display-body${large ? ' display-scroll' : ''}${both ? ' display-both' : ''}`}>{children}</div>
  )
  const v = display?.value
  if (!v) {
    if (large) return <Empty title="まだ結果がありません" icon="chart.xyaxis.line" description="実行すると表示されます。" />
    return <div className="display-center secondary" style={{ fontSize: size(10, 13) }}>まだ結果がありません（実行すると表示されます）</div>
  }
  switch (v.kind) {
    case 'text':
      return scrolling(<pre className={`display-text${large ? ' selectable' : ''}`} style={{ fontSize: size(11, 13) }}>{v.text}</pre>)
    case 'markdown':
      return scrolling(<div className={large ? 'selectable' : ''} style={{ fontSize: size(12, 14) }}><Markdown text={v.text} /></div>)
    case 'scalar':
      return (
        <div className="display-center col">
          <div style={{ fontSize: size(10, 17) }}>{v.label}</div>
          <div className={`mono-digits${large ? ' selectable' : ''}`} style={{ fontSize: size(26, 64) }}>{Number(v.value.toPrecision(8)).toString()}</div>
        </div>
      )
    case 'lineChart':
      return (
        <div className="display-chart" style={{ gap: 4 * zoom }}>
          <div className="display-chart-plot" style={{ borderRadius: 4 * zoom }}><DawnLineChart series={v.series} /></div>
          <div className="chart-legend" style={{ fontSize: size(9, 11) }}>
            {v.series.map((s, i) => <span key={i}><Icon name="line.diagonal" size={size(9, 11)} />{`${s.label} (${s.x.length})`}</span>)}
          </div>
        </div>
      )
    case 'table':
      return <TableDisplay key={`${display?.runId}:${display?.invocation}:${display?.revision}`} columns={v.columns} rows={v.rows} large={large} zoom={zoom} />
    case 'image':
      return <ImageDisplay data={v.data} type={v.mediaType} alt={v.alt} fontSize={size(10, 12)} />
    case 'points':
      return <PointCloudView x={v.x} y={v.y} z={v.z} colors={v.colors} />
    case 'module':
      return <ModuleDisplayView name={v.name} json={v.json} large={large} zoom={zoom} />
  }
}

function TableDisplay({ columns, rows, large, zoom }: { columns: string[]; rows: string[][]; large: boolean; zoom: number }) {
  const [requested, setPage] = useState(0)
  const page = tablePage(rows.length, large ? requested : 0)
  return <div style={{ display: 'flex', flexDirection: 'column', flex: 1, minHeight: 0, overflow: 'hidden', gap: 4 * zoom }}>
    {large ? <div className="row gap6 caption" style={{ flexShrink: 0 }}>
      <span>{page.start === page.end ? 0 : page.start + 1}–{page.end} / {rows.length} 行</span>
      <span className="spacer" />
      <Button disabled={page.index === 0} onClick={() => setPage(page.index - 1)}>前へ</Button>
      <select aria-label="ページ" value={page.index} onChange={(e) => setPage(Number(e.target.value))}>
        {Array.from({ length: page.count }, (_, i) => <option key={i} value={i}>{i + 1} / {page.count}</option>)}
      </select>
      <Button disabled={page.index + 1 >= page.count} onClick={() => setPage(page.index + 1)}>次へ</Button>
    </div> : <div className="secondary" style={{ fontSize: 9 * zoom }}>全 {rows.length} 行・先頭 {page.end} 行のプレビュー（拡大して全行表示）</div>}
    <div key={page.index} className={`display-body${large ? ' display-scroll display-both' : ''}`}>
      <table className="display-table" style={{ fontSize: (large ? 12 : 10) * zoom, borderSpacing: `${12 * zoom}px ${3 * zoom}px`, padding: 4 * zoom }}>
        <thead><tr>{columns.map((c, i) => <th key={i}>{c}</th>)}</tr></thead>
        <tbody>{rows.slice(page.start, page.end).map((r, i) => <tr key={page.start + i}>{r.map((cell, j) => <td key={j} className={large ? 'mono selectable' : 'mono'}>{cell}</td>)}</tr>)}</tbody>
      </table>
    </div>
  </div>
}

function ImageDisplay({ data, type, alt, fontSize }: { data: Uint8Array; type: string; alt: string; fontSize: number }) {
  const url = useMemo(() => URL.createObjectURL(new Blob([data as BlobPart], { type: type || 'image/png' })), [data, type])
  const [failed, setFailed] = useState(false)
  useEffect(() => () => URL.revokeObjectURL(url), [url])
  useEffect(() => setFailed(false), [url])
  if (failed) return <div className="display-center secondary" style={{ fontSize }}>画像を表示できません</div>
  return (
    <div className="display-center">
      <img className="display-image" src={url} alt={alt} draggable={false} onError={() => setFailed(true)} />
    </div>
  )
}

/** A 3D point cloud: drag to rotate, scroll to zoom. The projected points are
 *  written straight into a bitmap (3×3 pixels per point at the screen's scale). */
function PointCloudView({ x, y, z, colors }: { x: number[]; y: number[]; z: number[]; colors: number[] }) {
  const ref = useRef<HTMLCanvasElement>(null)
  const [view, setView] = useState({ yaw: 0.6, pitch: 0.4, scale: 1 })
  // Centered and measured once per data.
  const cloud = useMemo(() => {
    const n = Math.min(x.length, y.length, z.length)
    let cx = 0, cy = 0, cz = 0
    for (let i = 0; i < n; i++) { cx += x[i]; cy += y[i]; cz += z[i] }
    const d = Math.max(n, 1)
    cx /= d; cy /= d; cz /= d
    const px = new Float32Array(n), py = new Float32Array(n), pz = new Float32Array(n)
    let e = 1e-9
    for (let i = 0; i < n; i++) {
      px[i] = x[i] - cx; py[i] = y[i] - cy; pz[i] = z[i] - cz
      e = Math.max(e, Math.abs(px[i]), Math.abs(py[i]), Math.abs(pz[i]))
    }
    return { px, py, pz, n, extent: e }
  }, [x, y, z])

  useEffect(() => {
    const canvas = ref.current
    if (!canvas) return
    const draw = () => {
      const r = canvas.getBoundingClientRect()
      const scale = window.devicePixelRatio || 1
      const w = Math.round(r.width * scale), h = Math.round(r.height * scale)
      if (w <= 2 || h <= 2) return
      canvas.width = w
      canvas.height = h
      const ctx = canvas.getContext('2d')
      if (!ctx) return
      const img = ctx.createImageData(w, h)
      const buf = new Uint32Array(img.data.buffer)
      const s = (Math.min(w, h) * 0.42 * view.scale) / cloud.extent
      const cyw = Math.cos(view.yaw), syw = Math.sin(view.yaw), cp = Math.cos(view.pitch), sp = Math.sin(view.pitch)
      const hw = w / 2, hh = h / 2
      for (let i = 0; i < cloud.n; i++) {
        const rx = cloud.px[i] * cyw + cloud.pz[i] * syw
        const rz = -cloud.px[i] * syw + cloud.pz[i] * cyw
        const ry = cloud.py[i] * cp - rz * sp
        const sx = Math.floor(hw + rx * s), sy = Math.floor(hh - ry * s)
        if (sx < 1 || sy < 1 || sx >= w - 1 || sy >= h - 1) continue
        const c = i < colors.length ? colors[i] : 0x4a90e2
        // 0xRRGGBB to little-endian RGBA, opaque.
        const px = (0xff << 24) | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff)
        for (let dy = -1; dy <= 1; dy++) {
          const row = (sy + dy) * w + sx
          buf[row - 1] = px; buf[row] = px; buf[row + 1] = px
        }
      }
      ctx.putImageData(img, 0, 0)
    }
    draw()
    const o = new ResizeObserver(draw)
    o.observe(canvas)
    return () => o.disconnect()
  }, [cloud, colors, view])

  return (
    <div className="display-points" aria-label={`点群 ${x.length} 点`}
      onPointerDown={(e) => {
        e.stopPropagation()
        const start = { x: e.clientX, y: e.clientY, ...view }
        const move = (ev: PointerEvent) => setView((v) => ({
          ...v, yaw: start.yaw + (ev.clientX - start.x) / 120, pitch: Math.max(-1.5, Math.min(1.5, start.pitch + (ev.clientY - start.y) / 120)),
        }))
        const up = () => {
          window.removeEventListener('pointermove', move)
          window.removeEventListener('pointerup', up)
        }
        window.addEventListener('pointermove', move)
        window.addEventListener('pointerup', up)
      }}
      onWheel={(e) => {
        if (!e.ctrlKey && !e.metaKey) return
        e.stopPropagation()
        setView((v) => ({ ...v, scale: Math.max(0.2, Math.min(8, v.scale * Math.exp(-e.deltaY * 0.01))) }))
      }}>
      <canvas ref={ref} />
      <span className="display-points-caption caption2 secondary">{x.length} 点 · ドラッグで回転</span>
    </div>
  )
}

/** Data for a UI Canvas module. Known modules are drawn natively; any other
 *  module shows its data as formatted JSON. */
function ModuleDisplayView({ name, json, large, zoom }: { name: string; json: string; large: boolean; zoom: number }) {
  const value = useMemo(() => tryParse(json), [json])
  const bins = name === 'histogram' ? (arr(get(value, 'bins')) ?? []).map(dbl).filter((b): b is number => b !== undefined) : []
  if (bins.length) {
    const top = Math.max(...bins, 1e-9)
    return (
      <div className="display-histogram">
        {bins.map((b, i) => <div key={i} title={String(b)} style={{ height: `${Math.max(0.5, (100 * b) / top)}%` } as CSSProperties} />)}
      </div>
    )
  }
  return (
    <div className="display-module">
      <div className="secondary" style={{ fontSize: 10 * zoom, fontWeight: 700 }}>モジュール {name}</div>
      <pre className={`display-text${large ? ' selectable display-scroll' : ''}`} style={{ fontSize: (large ? 12 : 10) * zoom }}>
        {value !== undefined ? serialize(value) : json}
      </pre>
    </div>
  )
}
