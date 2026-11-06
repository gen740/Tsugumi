// The file the Agent is writing, by its Card and clear of the bird (the Mac
// app's WritingPanel): what has arrived and nothing else, marked as saved or not.
import { useEffect, useState } from 'react'
import { guide } from '../model/guide'
import { LiveCode, LiveCodePlayer, PANEL, panelFrame, panelScale, panelTail } from '../model/liveCode'
import { CardKey } from '../model/session'

type Pt = { x: number; y: number }

export function WritingPanels({ writers, known, rect, obstacles, screen, zoom, size }: {
  writers: Map<string, LiveCodePlayer>
  known: (card: CardKey) => boolean
  rect: (card: CardKey) => { x: number; y: number; width: number; height: number }
  /** The Cards and frames of the scope, which a panel keeps clear of if it can. */
  obstacles: () => { x: number; y: number; width: number; height: number }[]
  screen: (p: Pt) => Pt
  zoom: number
  size: { width: number; height: number }
}) {
  const [codes, setCodes] = useState<LiveCode[]>([])
  useEffect(() => {
    // Frames only while there is something to show.
    const t = setInterval(() => {
      const now = Date.now() / 1000
      const next = [...writers.values()].flatMap((w) => w.at(now) ?? [])
      setCodes((before) => (before.length || next.length ? next : before))
    }, 50)
    return () => clearInterval(t)
  }, [writers])
  const m = PANEL.margins
  const view = { x: m.left, y: m.top, width: size.width - m.left - m.right, height: size.height - m.top - m.bottom }
  const s = panelScale(zoom)
  return <>
    {codes.filter((c) => known(c.card)).map((c) => {
      const r = rect(c.card), tl = screen(r)
      const card = { x: tl.x, y: tl.y, width: r.width * zoom, height: r.height * zoom }
      // The bird stands beside the Card with its head this far above the top edge.
      const others = obstacles().map((o) => ({ ...screen(o), width: o.width * zoom, height: o.height * zoom }))
      const f = panelFrame(card, guide.present ? 54 : 0, s, view, others)
      if (!f) return null
      const lines = panelTail(c.text)
      // To the right of the Card; the line is hidden where they overlap.
      const x = Math.min(f.x + f.width, card.x + card.width) - 20 * s
      const from = f.y + f.height <= card.y ? f.y + f.height : card.y + card.height
      const to = f.y + f.height <= card.y ? card.y : f.y
      return (
        <div key={c.card + c.file} aria-hidden style={{ display: 'contents' }}>
          {to > from && <span className="cv-writing-line" style={{ left: x, top: from, height: to - from }} />}
          <div className="cv-writing" style={{ left: f.x, top: f.y, width: f.width, height: f.height, ['--s' as string]: s }}>
            <div className="cv-writing-head">
              <span className="cv-writing-file"><span>{c.file}</span></span>
              <span className="cv-writing-state">{c.saved ? '保存済み' : '保存前'}</span>
            </div>
            {lines.map((l, i) => (
              // The line being written shows its end, the ones above their beginning.
              <div key={i} className={i === lines.length - 1 ? 'cv-writing-row writing' : 'cv-writing-row'}><span>{l || ' '}</span></div>
            ))}
          </div>
        </div>
      )
    })}
  </>
}
