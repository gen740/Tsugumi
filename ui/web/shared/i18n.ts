import EN from '../../shared/i18n/en.json'

const dictionary: Record<string, string | { text: string } | undefined> = EN

export function translate(language: 'ja' | 'en', ja: string, values: unknown[]): string {
  const entry = language === 'en' ? dictionary[ja] : undefined
  const text = typeof entry === 'string' ? entry : entry?.text ?? ja
  return values.length ? text.replace(/\{(\d+)\}/g, (m, i) => (Number(i) < values.length ? String(values[Number(i)]) : m)) : text
}
