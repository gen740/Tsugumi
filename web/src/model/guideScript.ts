// What Tsugumi says without a model connection (agent/Shared/guide-script.json;
// the macOS app's GuideScript reads the same file): the steps of the tour of
// the demo report, each a guide_show with the step every choice leads to, and
// one reply to anything typed. Nothing is sent anywhere.
import script from '../../../agent/Shared/guide-script.json'
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

function step(r: Raw): ScriptStep {
  const show = parseShow(JSON.stringify(r.show ?? {}))
  return { show, text: r.text ?? show.say ?? '', next: r.next ?? {}, ran: r.ran }
}

export function scriptStep(id: string): ScriptStep | undefined {
  const r = (script.steps as Record<string, Raw>)[id]
  return r ? step(r) : undefined
}

/** The reply to anything typed. */
export const noConnection = (): ScriptStep => step(script.noConnection)
