/** A startup failure the user can resolve by archiving the incompatible history. */
export interface HistoryVersionMismatch {
  kind: 'incompatible_history'
  found: string
  expected: string
}

export function historyVersionMismatch(value: unknown): HistoryVersionMismatch | undefined {
  if (typeof value !== 'object' || !value) return
  const { kind, found, expected } = value as Record<string, unknown>
  if (kind === 'incompatible_history' && typeof found === 'string' && found && typeof expected === 'string' && expected) {
    return { kind, found, expected }
  }
}
