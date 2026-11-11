// What Tsugumi says without a model connection (agent/Shared/guide-script.json;
// the macOS app's GuideScript reads the same file): the tours of a few demos,
// each step a guide_show with the step every choice leads to, what Home's
// first question is answered with, and one reply to anything typed. Nothing
// is sent anywhere.
import japanese from '../../../agent/Shared/guide-script.json'
import english from '../../../agent/Shared/guide-script.en.json'
import { language } from '../i18n'
import { type GuideShow, parseShow } from './guide'

export interface ScriptStep {
  show: GuideShow
  /** What the panel or the chat shows (the bubble's line when the step has no text of its own). */
  text: string
  /** The step a tapped choice leads to. */
  next: Record<string, string>
  /** While this step is out, the step a run's end leads to (succeeded, or otherwise). */
  ran?: { ok?: string; else?: string }
}

interface Raw { show?: unknown; text?: string; next?: Record<string, string>; ran?: { ok?: string; else?: string } }
interface RawTour { keywords: string[]; home: Record<string, Raw>; steps: Record<string, Raw> }

// In the UI's language (the same steps and choices in both files).
const script = language() === 'en' ? english : japanese
const tours = script.tours as unknown as Record<string, RawTour>

function step(r: Raw, demo?: string): ScriptStep {
  // A step of the same tour, unless it names its tour.
  const resolve = (id?: string) => (id === undefined || id.includes('/') || !demo ? id : `${demo}/${id}`)
  const show = parseShow(JSON.stringify(r.show ?? {}))
  const next = Object.fromEntries(Object.entries(r.next ?? {}).map(([k, v]) => [k, resolve(v)!]))
  return { show, text: r.text ?? show.say ?? '', next, ran: r.ran && { ok: resolve(r.ran.ok), else: resolve(r.ran.else) } }
}

/** A step by "<demo>/<id>" ("noConnection" and "otherProject" have no demo). */
export function scriptStep(id: string): ScriptStep | undefined {
  if (id === 'noConnection' || id === 'otherProject') return step((script as unknown as Record<string, Raw>)[id])
  const [demo, name] = id.split('/')
  const r = tours[demo]?.home[name] ?? tours[demo]?.steps[name]
  return r ? step(r, demo) : undefined
}

/** The reply to anything typed. */
export const noConnection = (): ScriptStep => scriptStep('noConnection')!

/** Whether a step is answered on Home (a tour's "home"). */
export const homeStep = (id: string) => { const [demo, name] = id.split('/'); return !!name && !!tours[demo]?.home[name] }

/** The demos with a tour, in order. */
export const scriptDemos = Object.keys(tours)

/** The demo Home's first question picks for an answer: the first with one of its words, else the first. */
export function scriptDemoFor(interest: string): string {
  const t = interest.toLowerCase()
  return scriptDemos.find((d) => tours[d].keywords.some((k) => t.includes(k.toLowerCase()))) ?? scriptDemos[0]
}

/** Where a demo's tour begins (or continues): "otherProject" for a demo without one. */
export const scriptStart = (demo: string, id: 'start' | 'resume') => (tours[demo]?.steps[id] ? `${demo}/${id}` : 'otherProject')
