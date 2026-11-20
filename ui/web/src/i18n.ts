// The UI's language. The source strings are Japanese; English comes from
// ui/shared/i18n/en.json, keyed by the Japanese text, with {0}, {1}… for the values put
// in. The language is the app's setting (the server keeps it, shared with the
// desktop app on a Mac); this page reads it once at load, and changing it
// reloads the page.
import { translate } from '../shared/i18n'

export type Language = 'ja' | 'en'

const STORE = 'tsugumi.language'

function initial(): Language {
  const forced = (globalThis as { TSUGUMI_LANGUAGE?: string }).TSUGUMI_LANGUAGE
  if (forced === 'en' || forced === 'ja') return forced
  try {
    const v = localStorage.getItem(STORE)
    if (v === 'en' || v === 'ja') return v
  } catch { /* the page works without it */ }
  return 'ja'
}

let current: Language = initial()

export const language = (): Language => current

/** The text in the current language, with the values put in. */
export function L(ja: string, ...values: unknown[]): string {
  return translate(current, ja, values)
}

/** The server's setting as this page's: reloads when they differ (the
 *  strings were made at load). */
export function adoptLanguage(l: unknown) {
  if ((l !== 'en' && l !== 'ja') || l === current) return
  try { localStorage.setItem(STORE, l) } catch { /* the page works without it */ }
  if ((globalThis as { TSUGUMI_LANGUAGE?: string }).TSUGUMI_LANGUAGE) return
  location.reload()
}

/** Known host authentication states use the same wording in chat and the guide. */
export function modelErrorMessage(message: string, code?: string): string {
  if (code === 'chatgpt_sign_in_required') return L('ChatGPT にもう一度サインインしてください。')
  if (code === 'chatgpt_plan_disabled')
    return L('このアカウントでは ChatGPT プランの利用が許可されていないため、Agent は使えません。許可するか、API キーの接続を選んでください。')
  return message
}
