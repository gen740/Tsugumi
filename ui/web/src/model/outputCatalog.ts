// Output ports read from an `_outputs.fbs` text that is being edited, for
// the Card preview. The core remains the authority (it resolves the types
// on apply); this only reads the port catalog's shape: the root table's
// fields, their message types and `required`.
import type { PortData } from './canvasKit'
import { L } from '../i18n'

export type OutputCatalogResult = { ports: PortData[] } | { error: string }

export function parseOutputCatalog(text: string): OutputCatalogResult {
  // Comments out, strings kept intact enough for attribute values.
  let src = ''
  let i = 0
  while (i < text.length) {
    if (text.startsWith('//', i)) {
      const nl = text.indexOf('\n', i)
      i = nl < 0 ? text.length : nl
      continue
    }
    if (text.startsWith('/*', i)) {
      const end = text.indexOf('*/', i + 2)
      if (end < 0) return { error: L('コメントが閉じていません') }
      i = end + 2
      continue
    }
    src += text[i]
    i++
  }
  const rootMatch = /root_type\s+([A-Za-z_][A-Za-z0-9_.]*)\s*;/.exec(src)
  if (!rootMatch) return { error: L('root_type がありません') }
  const root = rootMatch[1]
  const short = root.split('.').pop() ?? root
  const tableStart = new RegExp(`table\\s+${short.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\s*\\{`).exec(src)
  if (!tableStart) return { error: L('root_type の table {0} が見つかりません', short) }
  const bodyStart = tableStart.index + tableStart[0].length
  const close = src.indexOf('}', bodyStart)
  if (close < 0) return { error: L('table {0} が閉じていません', short) }
  const body = src.slice(bodyStart, close)
  const ports: PortData[] = []
  for (const raw of body.split(';')) {
    const field = raw.trim()
    if (!field) continue
    const colon = field.indexOf(':')
    const parts = colon < 0 ? [field] : [field.slice(0, colon).trim(), field.slice(colon + 1).trim()].filter((p) => p !== '')
    if (parts.length !== 2 || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(parts[0])) return { error: L('フィールドを読めません: {0}', field) }
    const typeAndAttrs = parts[1]
    const type = typeAndAttrs.split('(')[0].trim()
    if (!type) return { error: L('型がありません: {0}', parts[0]) }
    const paren = typeAndAttrs.indexOf('(')
    const attrs = paren >= 0 ? typeAndAttrs.slice(paren) : ''
    const required = /\brequired\b/.test(attrs)
    ports.push({ name: parts[0], typeName: type, optional: !required })
  }
  return { ports }
}
