import { useEffect, useRef, useState } from 'react'
import { L } from '../i18n'
import { moduleThumbnailKey, settledThumbnailSize } from '../model/moduleThumbnails'
import { moduleThumbnails } from './moduleRenderer'

/** A preview only. Its content identity prevents an old run or same-sized
 *  update being presented as the current result. Full detail stays in Expand. */
export function ModuleThumbnail({ name, json, zoom }: { name: string; json: string; zoom: number }) {
  const ref = useRef<HTMLDivElement>(null)
  const [visible, setVisible] = useState(false)
  const [size, setSize] = useState({ width: 0, height: 0 })
  const [shown, setShown] = useState<{ name: string; json: string; width: number; height: number; image?: string; error?: string }>()
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
  useEffect(() => {
    if (!visible) { setShown(undefined); return }
    if (!size.width || !size.height) return
    const abort = new AbortController()
    void (async () => {
      const key = await moduleThumbnailKey(name, json, size.width, size.height)
      const image = await moduleThumbnails.request({ key, name, json, ...size }, abort.signal)
      if (!abort.signal.aborted && image) setShown({ name, json, ...size, image })
    })().catch(error => { if (!abort.signal.aborted) setShown({ name, json, ...size, error: String(error.message ?? error) }) })
    return () => abort.abort()
  }, [name, json, size.width, size.height, visible])
  const current = shown?.name === name && shown?.json === json && shown?.width === size.width && shown?.height === size.height
  return <div ref={ref} className="display-center" style={{ background: 'white', borderRadius: 4 * zoom, overflow: 'hidden' }}
    aria-label={name === 'plotly' ? 'Plotly' : 'Vega-Lite'}>
    {current && shown.image ? <img className="display-image" style={{ imageRendering: 'auto' }} src={shown.image} alt={L('グラフのプレビュー（拡大して操作）')} draggable={false} />
      : <span className="secondary" style={{ fontSize: 10 * zoom }}>{current && shown.error ? shown.error : L('グラフのプレビューを作成中…')}</span>}
  </div>
}
