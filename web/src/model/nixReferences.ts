import { CardKey, key, ProjectSession } from './session'
import { Rect } from './canvasKit'

export interface NixReference {
  id: string
  source: CardKey
  target: CardKey
  expression: string
  valid: boolean
}

export function nixReferences(session: Pick<ProjectSession, 'resolution'>): NixReference[] {
  return [...session.resolution].flatMap(([source, card]) => card.nixReferences.map(ref => ({
    id: JSON.stringify([source, ref.local, ref.name]), source, target: key(ref.target),
    expression: `inputs.${ref.local ? 'local' : 'upstream'}.${JSON.stringify(ref.name)}`,
    valid: ref.valid,
  }))).sort((a, b) => a.source.localeCompare(b.source) || a.expression.localeCompare(b.expression))
}

/** A nested reference meets its visible container; the list keeps the exact path. */
export function visibleNixCard(card: CardKey, scope: string[], visible: Set<CardKey>): CardKey | undefined {
  const parts = card.split('/')
  if (parts.length <= scope.length || !scope.every((s, i) => parts[i] === s)) return
  const projected = key(parts.slice(0, scope.length + 1))
  return visible.has(projected) ? projected : undefined
}

/** Side connections distinguish Nix references from data ports. */
export function nixCurve(a: Rect, b: Rect): string {
  const right = b.x + b.width / 2 >= a.x + a.width / 2
  const x1 = right ? a.x + a.width : a.x, x2 = right ? b.x : b.x + b.width
  const y1 = a.y + a.height / 2, y2 = b.y + b.height / 2
  const bend = Math.max(45, Math.abs(x2 - x1) / 2) * (right ? 1 : -1)
  return `M ${x1},${y1} C ${x1 + bend},${y1} ${x2 - bend},${y2} ${x2},${y2}`
}
