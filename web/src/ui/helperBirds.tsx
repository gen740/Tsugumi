// A bird per helper on the canvas (the Mac app's PresenceLayer): its thread, the
// halo where it stands, its name beside it and a bubble with what it does, all in
// its colour; a mark at the edge when it is out of view. Never takes a click.
import { CSSProperties, useEffect, useState } from 'react'
import type { ChatController } from '../model/chat'
import { guide } from '../model/guide'
import { BIRD, birdColor, BIRD_TIMING, bubbleRise, perchOf, Pt, rise } from '../model/helperBirds'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { CardKey } from '../model/session'
import { GuideBird } from './guide'
import { Icon } from './icons'

/** The middle of the bird in its picture (viewBox 130 270 680 470), looking right
 *  (looking left it is mirrored). */
const CENTER = { x: 370 / 680, y: 210 / 470 }
/** The window's bars float over the canvas this far down. */
const TOP_MARGIN = 52

export function HelperBirds({ chat, known, rect, screen, zoom, size }: {
  chat: ChatController
  known: (card: CardKey) => boolean
  rect: (card: CardKey) => { x: number; y: number }
  screen: (p: Pt) => Pt
  zoom: number
  size: { width: number; height: number }
}) {
  useObserve(chat, guide, prefs)
  const now = Date.now() / 1000
  const birds = guide.present ? chat.team.birds.filter((b) => b.visible(now)) : []
  const live = birds.length > 0
  // Frames only while a bird is out.
  const [, frame] = useState(0)
  useEffect(() => {
    if (!live) return
    let id = requestAnimationFrame(function loop() {
      frame((n) => n + 1)
      id = requestAnimationFrame(loop)
    })
    return () => cancelAnimationFrame(id)
  }, [live])
  const still = prefs.tsugumiStill
  const perch = (card: string) => (known(card) ? perchOf(rect(card)) : undefined)
  const z = zoom * BIRD.helperScale
  const fit = Math.min(1.3, Math.max(0.8, z))
  return <>
    {birds.map((b) => {
      const place = b.place(now, perch, still)
      if (!place) return null
      const color = birdColor(b.tint, prefs.dark)
      const fade = b.fade(now)
      const at = screen(place)
      const p = { x: at.x, y: at.y - 16 * fade.lift }
      const inside = p.x >= 8 && p.x <= size.width - 8 && p.y >= 8 && p.y <= size.height - 8
      const w = 680 * BIRD.scale * z, h = 470 * BIRD.scale * z
      const center = place.facing > 0 ? CENTER.x : 1 - CENTER.x
      const line = b.line(now)
      const since = line ? now - line.since : 0
      const leaving = line?.done ? rise((BIRD_TIMING.doneLine - since) / 0.8) : 1
      const lineWidth = line ? [...line.text].reduce((n, c) => n + (c.charCodeAt(0) < 128 ? 6.9 : 11.8), 22) : 0
      const top = p.y - bubbleRise(z)
      const trail = still ? [] : b.trail(now, perch).map(screen)
      return (
        <div key={b.name} aria-hidden style={{ display: 'contents', ['--bird' as string]: color }}>
          {trail.length > 1 && (
            <svg className="cv-wires helper-trail" width={size.width} height={size.height} style={{ opacity: fade.opacity }}>
              {trail.slice(1).map((q, k) => {
                const f = 1 - (k + 1) / trail.length
                return <line key={k} x1={trail[k].x} y1={trail[k].y} x2={q.x} y2={q.y} stroke={color} strokeLinecap="round"
                  strokeOpacity={0.4 * f} strokeWidth={(2 * f + 0.4) * Math.min(1.3, Math.max(0.7, z))} />
              })}
            </svg>
          )}
          <span className="helper-halo" style={{ left: p.x - 23 * z, top: p.y + 18 * z, width: 46 * z, height: 12 * z, opacity: fade.opacity }} />
          <div className={`helper-bird${place.flying || b.writing(now) ? ' flying' : ''}${still ? ' still' : ''}${place.facing > 0 ? ' right' : ''}`}
            style={{ left: p.x - center * w, top: p.y - CENTER.y * h, width: w, height: h, opacity: fade.opacity }}>
            <GuideBird />
          </div>
          <span className="helper-name" style={{ left: p.x + 30 * fit, top: p.y - 14, opacity: fade.opacity }}>{b.name}</span>
          {line && (
            <span className="helper-line material" style={{
              left: Math.min(size.width - 12 - lineWidth, Math.max(12, p.x - 36)),
              top: (inside ? Math.max(TOP_MARGIN, top) : top) + 5 * (1 - rise(since / 0.45)),
              maxWidth: size.width - 24,
              opacity: Math.min(1, since / 0.45) * leaving * fade.opacity,
            } as CSSProperties}>{line.text}</span>
          )}
          {!inside && (
            <span className="helper-edge" style={{ left: Math.min(size.width - 22, Math.max(22, p.x)), top: Math.min(size.height - 16, Math.max(16, p.y)) }}>
              <span className="helper-edge-dot" />
              <span style={{ display: 'inline-flex', transform: `rotate(${p.x > size.width ? 0 : p.x < 0 ? 180 : p.y < 0 ? -90 : 90}deg)` }}><Icon name="chevron.right" size={9} /></span>
            </span>
          )}
        </div>
      )
    })}
  </>
}
