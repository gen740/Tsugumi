// The server's messages in the app's language (the same table as the page's,
// web/src/i18n/en.json). The setting is read where it is kept (chat.ts) and
// set here.
import EN from '../src/i18n/en.json'

export type Language = 'ja' | 'en'

const en = EN as Record<string, string>
let current: Language = 'ja'

export const language = (): Language => current
export function setLanguage(l: Language) { current = l }

export function L(ja: string, ...values: unknown[]): string {
  const text = current === 'en' ? en[ja] ?? ja : ja
  return values.length ? text.replace(/\{(\d+)\}/g, (m, i) => (Number(i) < values.length ? String(values[Number(i)]) : m)) : text
}
