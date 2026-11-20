// The server's messages in the app's language (the same table as the page's,
// ui/shared/i18n/en.json). The setting is read where it is kept (chat.ts) and
// set here.
import { translate } from '../shared/i18n'

export type Language = 'ja' | 'en'

let current: Language = 'ja'

export const language = (): Language => current
export function setLanguage(l: Language) { current = l }

export function L(ja: string, ...values: unknown[]): string {
  return translate(current, ja, values)
}
