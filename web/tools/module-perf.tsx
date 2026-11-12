// Manual browser benchmark: the production display component, fixed plot data,
// initial render, 120 pan/zoom frames, updates and release of all viewers.
import { createRoot } from 'react-dom/client'
import { useEffect, useState } from 'react'
import { DisplayContent } from '../src/ui/display'
import type { DisplaySnapshot } from '../src/model/session'
import '../src/styles.css'
import '../src/styles/canvas.css'

type Scene = 'complex' | 'large' | 'many'
const make = (scene: Scene, revision: number) => {
  const count = scene === 'many' ? 12 : 1
  return Array.from({ length: count }, (_, i) => {
    const vega = scene === 'large' || (scene === 'many' && i % 2 === 1)
    const n = scene === 'large' ? 50000 : scene === 'complex' ? 5000 : 2000
    const x = Array.from({ length: n }, (_, j) => j / n * 30)
    const y = x.map((v, j) => Math.sin(v + revision + i) + Math.cos(j * 0.17) * 0.1)
    const spec = vega ? { mark: 'point', data: { values: x.map((x, j) => ({ x, y: y[j] })) },
      encoding: { x: { field: 'x', type: 'quantitative' }, y: { field: 'y', type: 'quantitative' } } }
      : { data: Array.from({ length: scene === 'complex' ? 8 : 2 }, (_, trace) => ({ x, y: y.map(v => v + trace), type: 'scatter', mode: 'lines', name: `series-${trace}` })), layout: { title: { text: `Result ${i} revision ${revision}` }, margin: { t: 40, l: 35, r: 10, b: 30 } } }
    return { runId: `run-${revision}`, invocation: 1n, revision: BigInt(revision), value: { kind: 'module', name: vega ? 'vega-lite' : 'plotly', json: JSON.stringify(spec) } } as DisplaySnapshot
  })
}
function Bench() {
  const [scene, setScene] = useState<Scene>('complex')
  const [items, setItems] = useState<DisplaySnapshot[]>([])
  const [expanded, setExpanded] = useState(false)
  const [revision, setRevision] = useState(0)
  const [camera, setCamera] = useState({ zoom: 1, x: 0 })
  const [report, setReport] = useState('Choose a scene, then Render.')
  const [start, setStart] = useState(0)
  const [animating, setAnimating] = useState(false)
  useEffect(() => {
    if (!start || !items.length) return
    const done = new Set<MessageEventSource>()
    const listener = (e: MessageEvent) => { if (e.data?.tsugumi === 'state' && e.data.state === 'ok' && e.source) done.add(e.source) }
    window.addEventListener('message', listener)
    const timer = setInterval(() => {
      const images = [...document.querySelectorAll<HTMLImageElement>('.display-image')].filter(image => image.complete && image.naturalWidth > 0).length
      if (images < items.length && done.size < items.length) return
      const elapsed = performance.now() - start
      const bytes = items.reduce((n, d) => n + (d.value.kind === 'module' ? d.value.json.length * 2 : 0), 0)
      setReport(`scene=${scene}; initial=${elapsed.toFixed(1)}ms; sourceBytes=${bytes}; images=${images}; completedFrames=${done.size}`)
      clearInterval(timer)
    }, 50)
    return () => { clearInterval(timer); window.removeEventListener('message', listener) }
  }, [start, items])
  const animate = () => {
    setAnimating(true)
    const deltas: number[] = []; let previous = performance.now(), i = 0
    const tick = (now: number) => {
      deltas.push(now - previous); previous = now
      setCamera({ zoom: 0.8 + Math.sin(i / 20) * 0.15, x: Math.sin(i / 15) * 30 })
      if (++i < 120) requestAnimationFrame(tick)
      else {
        deltas.sort((a, b) => a - b)
        const memory = (performance as Performance & { memory?: { usedJSHeapSize: number } }).memory?.usedJSHeapSize
        setReport(old => old + `\nframes=120; p50=${deltas[60].toFixed(1)}ms; p95=${deltas[114].toFixed(1)}ms; max=${deltas[119].toFixed(1)}ms; liveFrames=${document.querySelectorAll('iframe').length}; heapBytes=${memory ?? 'unavailable'}`)
        setAnimating(false)
      }
    }
    requestAnimationFrame(tick)
  }
  return <>
    <div style={{ position: 'fixed', top: 0, background: '#222', color: 'white', padding: 10, zIndex: 2, width: '100%' }}>
      <label>Scene <select value={scene} onChange={e => setScene(e.target.value as Scene)}><option value="complex">Complex Plotly</option><option value="large">Large Vega</option><option value="many">12 mixed plots</option></select></label>
      <button onClick={() => { setReport('Rendering…'); setItems(make(scene, revision)); setStart(performance.now()) }}>Render</button>
      <button disabled={animating || !items.length} onClick={animate}>Measure pan and zoom</button>
      <button onClick={() => { const r = revision + 1; setRevision(r); setReport('Rendering…'); setItems(make(scene, r)); setStart(performance.now()) }}>New result</button>
      <button onClick={() => setExpanded(!expanded)}>{expanded ? 'Close expanded' : 'Open expanded'}</button>
      <button onClick={() => { setItems([]); setExpanded(false) }}>Close all</button>
      <pre style={{ margin: 4, whiteSpace: 'pre-wrap' }} aria-label="Benchmark results">{report}</pre>
    </div>
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4, max-content)', gap: 12, position: 'absolute', top: 130, left: camera.x }}>
      {items.map((d, i) => <div key={i} style={{ width: (items.length > 1 ? 260 : 800) * camera.zoom, height: (items.length > 1 ? 150 : 480) * camera.zoom, display: 'flex' }}><DisplayContent display={d} zoom={camera.zoom} /></div>)}
    </div>
    {expanded && items[0] && <div style={{ position: 'absolute', inset: '150px 80px 20px', display: 'flex', background: 'white' }}><DisplayContent display={items[0]} large /></div>}
  </>
}
createRoot(document.getElementById('root')!).render(<Bench />)
