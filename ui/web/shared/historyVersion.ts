/** A startup failure with the stored and required state format versions. */
export interface HistoryVersionMismatch {
  kind: 'incompatible_history' | 'incompatible_state'
  found: string
  expected: string
  path?: string
}

export function historyVersionMismatch(value: unknown): HistoryVersionMismatch | undefined {
  if (typeof value !== 'object' || !value) return
  const { kind, found, expected, path } = value as Record<string, unknown>
  if ((kind === 'incompatible_history' || kind === 'incompatible_state') && typeof found === 'string' && found && typeof expected === 'string' && expected) {
    return typeof path === 'string' && path ? { kind, found, expected, path } : { kind, found, expected }
  }
}
