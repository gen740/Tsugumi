import { arr, get, J, str } from '../../shared/json'

export type NixPathLockStatus =
  | { kind: 'unlocked' }
  | { kind: 'urlChanged' }
  | { kind: 'locked'; pin: string }

export function nixPathLockStatus(document: J | undefined, name: string, url: string): NixPathLockStatus {
  const entry = arr(get(document, 'entries'))?.find(row => str(get(row, 'name')) === name)
  const hash = str(get(entry, 'narHash'))
  if (!hash) return { kind: 'unlocked' }
  if (str(get(entry, 'url')) !== url) return { kind: 'urlChanged' }
  return { kind: 'locked', pin: str(get(entry, 'rev'))?.slice(0, 7) ?? hash.slice(0, 14) }
}
