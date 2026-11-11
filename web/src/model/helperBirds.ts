// The helpers of a delegate call on the canvas, a bird each (the Mac app's
// AgentPresence, PresenceColor and ProjectSession's helperPresences): each comes
// to the Cards it works on, one short scene after another, says what it does and
// goes a little while after it ended. Times in seconds.
import { workScene } from './guide'
import { L } from '../i18n'

/** Lengths of the scenes: unhurried, so that the bird glides. */
export const BIRD_TIMING = { scene: 1.6, flight: 1, maxBacklog: 5, linger: 8, fadeIn: 0.5, fadeOut: 1.6, doneLine: 3 }

/** Starts and stops without a jolt. */
export function rise(x: number): number {
  const u = Math.min(1, Math.max(0, x))
  return u * u * u * (u * (6 * u - 15) + 10)
}

export interface Pt { x: number; y: number }

/** A flight from `a` to `b`, `u` of the way there: it arcs a little upwards. */
export function flightPoint(a: Pt, b: Pt, u: number): Pt {
  const e = rise(u)
  return { x: a.x + (b.x - a.x) * e, y: a.y + (b.y - a.y) * e - 20 * Math.sin(Math.PI * e) }
}

/** The bird's size in the picture's units, from its feet to the middle of its
 *  body (world units) and how much smaller a helper's is; how far above its
 *  middle the bubble's top is. */
export const BIRD = { scale: 0.075, feetToCenter: { x: -0.6, y: -16.95 }, helperScale: 0.8 }
export const bubbleRise = (z: number) => 54 * Math.min(1.3, Math.max(0.8, z))

/** Where a helper's bird stands on a Card (its top edge, world units). */
export const perchOf = (r: { x: number; y: number }): Pt => ({ x: r.x + 26 + BIRD.feetToCenter.x, y: r.y + BIRD.feetToCenter.y })

/** How far above the Card's top edge a helper's bird and its bubble reach (points). */
export const helperClear = (zoom: number) => -BIRD.feetToCenter.y * zoom + bubbleRise(zoom * BIRD.helperScale)

/** How large the Agent's bird stands by what it points at, from that element's
 *  `data-canvas-zoom`: by a Card it goes with the canvas, or it would stand over the
 *  Cards when zoomed out, but stays large enough to see and never grows past its own
 *  size. What is not on the canvas has no zoom and leaves the bird as it is. */
export const perchScale = (canvasZoom?: string) => (canvasZoom === undefined ? 1 : Math.max(0.45, Math.min(1, Number(canvasZoom))))

function hsv(h: number, s: number, v: number): string {
  const f = (n: number) => {
    const k = (n + h * 6) % 6
    return Math.round((v - v * s * Math.max(0, Math.min(k, 4 - k, 1))) * 255)
  }
  return `rgb(${f(5)}, ${f(3)}, ${f(1)})`
}

const HUES = [0.08, 0.58, 0.83, 0.13, 0.95, 0.45, 0.7, 0.3]

/** The colour of a helper's bird, one hue per helper (1, 2, …). */
export const birdColor = (tint: number, dark: boolean) => {
  const h = HUES[(Math.max(1, tint) - 1) % HUES.length]
  return dark ? hsv(h, 0.45, 0.95) : hsv(h, 0.75, 0.62)
}

/** The main Agent's colour while it has helpers out: gold, to tell it apart. */
export const leadColor = (dark: boolean) => (dark ? 'rgb(250, 204, 89)' : 'rgb(184, 133, 13)')

export type BirdOutcome = 'done' | 'failed' | 'stopped' | 'limit'
const OUTCOME_LINE: Record<BirdOutcome, string> = { done: L('できました'), failed: L('うまくいきませんでした'), stopped: L('止めました'), limit: L('途中で止まっています') }

interface Scene { card: string; line: string; start: number }
export interface BirdPlace extends Pt { flying: boolean; facing: 1 | -1 }

/** What the bird looks at while its writer plays a file: that file's Card. */
export interface BirdWriter { at(now: number): { card: string } | undefined }

export class HelperBird {
  scenes: Scene[] = []
  working = true
  endedAt?: number
  private appearedAt?: number
  private activity?: { text: string; at: number }
  private outcome: BirdOutcome = 'done'
  private flight?: { card: string; from?: Pt; at: number }

  constructor(readonly name: string, readonly tint: number, private readonly writer: () => BirdWriter | undefined, now: number) {
    this.appearedAt = now
  }

  /** The next scene; one that only reads is left out when the bird is far behind. */
  visit(card: string, line: string, now: number, keep = false) {
    const last = this.scenes[this.scenes.length - 1]
    const lastEnd = last ? last.start + BIRD_TIMING.scene : now
    const start = Math.max(now, lastEnd)
    if (!keep && last?.card === card && lastEnd > now - 0.5) return // already there
    if (!keep && start - now > BIRD_TIMING.maxBacklog) return
    this.scenes.push({ card, line, start })
  }

  /** What it does between scenes (looking things up, checking, waiting). */
  doing(text: string | undefined, now: number) {
    if (this.working && text) this.activity = { text, at: now }
  }

  ended(outcome: BirdOutcome, now: number) {
    if (!this.working) return
    this.working = false
    this.outcome = outcome
    const last = this.scenes[this.scenes.length - 1]
    this.endedAt = Math.max(now, last ? last.start + BIRD_TIMING.scene : now)
  }

  private scene(now: number): Scene | undefined {
    for (let i = this.scenes.length - 1; i >= 0; i--) if (this.scenes[i].start <= now) return this.scenes[i]
    return undefined
  }

  /** The Card it is at: the file it writes first, otherwise its scene's. */
  card(now: number): string | undefined {
    return this.writer()?.at(now)?.card ?? this.scene(now)?.card
  }

  writing(now: number) {
    return !!this.writer()?.at(now)
  }

  /** It is on the canvas: during its work and a little while after. */
  visible(now: number) {
    return this.working || (this.endedAt !== undefined && now - this.endedAt < BIRD_TIMING.linger)
  }

  /** It fades in as it comes; at the end it rises (`lift`, 0…1) as it fades away. */
  fade(now: number): { opacity: number; lift: number } {
    const fadeIn = this.appearedAt === undefined ? 1 : rise((now - this.appearedAt) / BIRD_TIMING.fadeIn)
    if (this.working || this.endedAt === undefined) return { opacity: fadeIn, lift: 0 }
    const out = rise((this.endedAt + BIRD_TIMING.linger - now) / BIRD_TIMING.fadeOut)
    return { opacity: Math.min(fadeIn, out), lift: 1 - out }
  }

  /** What its bubble says at `now`, and since when; undefined: no bubble. */
  line(now: number): { text: string; since: number; done?: boolean } | undefined {
    const file = this.writer()?.at(now)
    if (file) return { text: L('{0} を書いています', file.card), since: this.flight?.at ?? now }
    const s = this.scene(now)
    if (s && s.start + BIRD_TIMING.scene > now) return { text: s.line, since: s.start }
    if (this.working) {
      if (this.activity && this.activity.at >= (s?.start ?? -Infinity)) return { text: this.activity.text, since: this.activity.at }
      if (s) return { text: s.line, since: s.start }
      return { text: L('考えています'), since: this.appearedAt ?? now }
    }
    const doneAt = Math.max(s ? s.start + BIRD_TIMING.scene : 0, this.endedAt ?? 0)
    return now - doneAt < BIRD_TIMING.doneLine ? { text: OUTCOME_LINE[this.outcome], since: doneAt, done: true } : undefined
  }

  /** Where it is at `now` (world units), given where it stands on a Card
   *  (undefined: that Card is not in the scope shown). Still: at each place at once. */
  place(now: number, perch: (card: string) => Pt | undefined, still = false): BirdPlace | undefined {
    // A new Card has no place until it is saved: the bird stays by its last one.
    const file = this.writer()?.at(now)?.card
    const card = file && perch(file) ? file : this.scene(now)?.card
    if (!card) return undefined
    if (card !== this.flight?.card) {
      const from = this.flight && !still ? this.point(now, perch) : undefined
      this.flight = { card, from, at: now }
    }
    const p = this.point(now, perch)
    if (!p) return undefined
    const ahead = this.point(now + 0.15, perch) ?? p
    return { ...p, flying: !!this.flight!.from && now < this.flight!.at + BIRD_TIMING.flight, facing: ahead.x < p.x - 2 ? -1 : 1 }
  }

  /** Where it was over the last moment, the newest first (its thread). */
  trail(now: number, perch: (card: string) => Pt | undefined): Pt[] {
    const out: Pt[] = []
    for (let k = 0; k <= 18; k++) {
      const p = this.point(now - 0.045 * k, perch)
      if (p) out.push(p)
    }
    return out
  }

  private point(t: number, perch: (card: string) => Pt | undefined): Pt | undefined {
    const f = this.flight
    const to = f && perch(f.card)
    if (!f || !to) return undefined
    if (!f.from || t >= f.at + BIRD_TIMING.flight) return to
    return t <= f.at ? f.from : flightPoint(f.from, to, (t - f.at) / BIRD_TIMING.flight)
  }
}

/** The helpers of the last delegate call, in the order it named them. */
export class HelperTeam {
  birds: HelperBird[] = []

  constructor(private readonly writerOf: (name: string) => BirdWriter | undefined) {}

  /** A delegate call started: each helper's bird comes to its first Card. */
  start(tasks: { name: string; cards: string[] }[], now: number) {
    this.birds = tasks.map((t, i) => {
      const b = new HelperBird(t.name, i + 1, () => this.writerOf(t.name), now)
      if (t.cards[0]) b.visit(t.cards[0], L('{0} を読んでいます', t.cards[0]), now)
      return b
    })
  }

  bird(name: string) {
    return this.birds.find((b) => b.name === name)
  }

  /** Its colour's number; a helper of an earlier call has the first (the Mac app's too). */
  tint(name: string) {
    return this.bird(name)?.tint ?? 1
  }

  /** Helpers are on the canvas: the main Agent stands out among them. */
  out(now: number) {
    return this.birds.some((b) => b.visible(now))
  }

  /** What a helper did. */
  note(name: string, e: { kind: string; name?: string; arguments?: string; message?: string }, now: number) {
    const b = this.bird(name)
    if (!b) return
    switch (e.kind) {
      case 'text':
        b.doing(L('考えています'), now)
        break
      case 'toolStarted': {
        const s = workScene(e.name ?? '', e.arguments ?? '{}')
        if (s.card) b.visit(s.card, s.line ?? '', now, e.name === 'run' || e.name === 'project_save')
        else b.doing(s.line, now)
        break
      }
      case 'finished':
        b.ended('done', now)
        break
      case 'stepLimit':
        b.ended('limit', now)
        break
      case 'error':
        b.ended(e.message === 'cancelled' ? 'stopped' : 'failed', now)
        break
    }
  }

  /** The main Agent's turn ended: no helper is at work any more, whatever reached the page. */
  endAll(outcome: BirdOutcome, now: number) {
    for (const b of this.birds) b.ended(outcome, now)
  }
}
