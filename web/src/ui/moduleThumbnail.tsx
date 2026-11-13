import type { ModuleBlobs } from '../model/moduleSources'
import { useEffect, useReducer, useRef, useState } from 'react'
import { L } from '../i18n'
import { FigurePreview, FigureState, FigureWanted, figureLook, settledThumbnailSize } from '../model/moduleThumbnails'
import { moduleThumbnails } from './moduleRenderer'

const SETTLED: FigureState = { mode: 'update', running: false, stale: false }

/** A preview only. The last picture stays on screen until the next one is
 *  ready, and `figureLook` decides how: at full strength only as the result
 *  it is a picture of (or, followed live, the revision just before it), dimmed
 *  otherwise. Full detail stays in Expand. */
export function ModuleThumbnail({ name, json, blobs, lineage, zoom, figure = SETTLED }: { name: string; json: string; blobs: ModuleBlobs; lineage: string; zoom: number; figure?: FigureState }) {
  const ref = useRef<HTMLDivElement>(null)
  const [visible, setVisible] = useState(false)
  const [size, setSize] = useState({ width: 0, height: 0 })
  const [, redraw] = useReducer((n: number) => n + 1, 0)
  const preview = useRef<FigurePreview>()
  preview.current ??= new FigurePreview(moduleThumbnails, redraw)
  useEffect(() => {
    const node = ref.current
    if (!node) return
    const observer = new IntersectionObserver(entries => setVisible(entries.some(e => e.isIntersecting)))
    observer.observe(node)
    return () => observer.disconnect()
  }, [])
  useEffect(() => {
    const node = ref.current
    if (!node) return
    const measure = () => {
      setSize(old => settledThumbnailSize(node.clientWidth, node.clientHeight, zoom, old))
    }
    measure()
    const observer = new ResizeObserver(measure)
    observer.observe(node)
    return () => observer.disconnect()
  }, [zoom])
  const wanted: FigureWanted = { name, json, blobs, width: size.width, height: size.height, lineage }
  const follow = figure.mode === 'live' && figure.running
  useEffect(() => {
    preview.current!.want(visible ? wanted : undefined, follow)
  }, [name, json, blobs, lineage, size.width, size.height, visible, follow])
  useEffect(() => () => preview.current!.close(), [])
  const look = figureLook(wanted, preview.current.picture, figure)
  return <div ref={ref} className="display-center" style={{ background: 'white', borderRadius: 4 * zoom, overflow: 'hidden' }}
    aria-label={name === 'plotly' ? 'Plotly' : 'Vega-Lite'} aria-busy={look.waiting || look.dimmed}>
    {look.image
      ? <img className="display-image" style={{ imageRendering: 'auto', opacity: look.dimmed ? 0.4 : 1, transition: 'opacity 0.15s' }} src={look.image}
          alt={look.dimmed ? L('グラフのプレビュー（前の結果。更新を待っています）') : L('グラフのプレビュー（拡大して操作）')} draggable={false} />
      : <span className="secondary" style={{ fontSize: 10 * zoom }}>{look.error ?? L('グラフのプレビューを作成中…')}</span>}
  </div>
}
