// The file the Agent is writing, by its Card and clear of the bird (the Mac
// app's WritingPanel): what has arrived and nothing else, marked as saved or not.
// A Card that is not on this canvas has it where there is room, with its name.
import { useEffect, useState } from 'react'
import { guide } from '../model/guide'
import { birdColor, helperClear, HelperTeam, leadColor } from '../model/helperBirds'
import { LiveCode, LiveCodePlayer, PANEL, panelFrame, panelFreeFrame, panelScale, panelTail } from '../model/liveCode'
import { prefs } from '../model/prefs'
import { CardKey } from '../model/session'

type Pt = { x: number; y: number }

export function WritingPanels({ writers, team, known, rect, obstacles, screen, zoom, size }: {
  writers: Map<string, LiveCodePlayer>
  /** A panel has the colour of the bird that writes it. */
  team: HelperTeam
  known: (card: CardKey) => boolean
  rect: (card: CardKey) => { x: number; y: number; width: number; height: number }
  /** The Cards and frames of the scope, which a panel keeps clear of if it can. */
  obstacles: () => { x: number; y: number; width: number; height: number }[]
  screen: (p: Pt) => Pt
  zoom: number
  size: { width: number; height: number }
}) {
  const [codes, setCodes] = useState<(LiveCode & { who: string })[]>([])
  useEffect(() => {
    // Frames only while there is something to show.
    const t = setInterval(() => {
      const now = Date.now() / 1000
      const next = [...writers].flatMap(([who, w]) => {
        const code = w.at(now)
        return code ? [{ ...code, who }] : []
      })
      setCodes((before) => (before.length || next.length ? next : before))
    }, 50)
    return () => clearInterval(t)
  }, [writers])
  const m = PANEL.margins
  const view = { x: m.left, y: m.top, width: size.width - m.left - m.right, height: size.height - m.top - m.bottom }
  const s = panelScale(zoom)
  return <>
    {codes.map((c, slot) => {
      const others = obstacles().map((o) => ({ ...screen(o), width: o.width * zoom, height: o.height * zoom }))
      const r = known(c.card) ? rect(c.card) : undefined, tl = r && screen(r)
      const card = r && tl && { x: tl.x, y: tl.y, width: r.width * zoom, height: r.height * zoom }
      // The Agent's bird stands beside the Card with its head this far above the
      // top edge; a helper's stands on the edge, its bubble above it.
      const clear = c.who ? helperClear(zoom) : guide.present ? 54 : 0
      const f = card ? panelFrame(card, clear, s, view, others) : panelFreeFrame(s, view, others, slot)
      if (!f) return null
      const color = c.who ? birdColor(team.tint(c.who), prefs.dark) : team.out(Date.now() / 1000) ? leadColor(prefs.dark) : undefined
      const lines = panelTail(c.text)
      // To the right of the Card; the line is hidden where they overlap.
      const x = card && Math.min(f.x + f.width, card.x + card.width) - 20 * s
      const from = card && (f.y + f.height <= card.y ? f.y + f.height : card.y + card.height)
      const to = card && (f.y + f.height <= card.y ? card.y : f.y)
      return (
        <div key={c.card + c.file} aria-hidden style={{ display: 'contents', ...(color && { ['--bird' as string]: color }) }}>
          {card && to! > from! && <span className="cv-writing-line" style={{ left: x, top: from, height: to! - from! }} />}
          <div className="cv-writing" data-writing-card={c.card} style={{ left: f.x, top: f.y, width: f.width, height: f.height, ['--s' as string]: s }}>
            <div className="cv-writing-head">
              <span className="cv-writing-file"><span>{card ? c.file : `${c.card}/${c.file}`}</span></span>
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
