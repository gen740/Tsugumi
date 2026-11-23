import { arr, dbl, get, J, str } from '../../shared/json'

export type NixPathLockStatus =
  | { kind: 'unlocked' }
  | { kind: 'urlChanged' }
  | { kind: 'locked'; revision: string; modified?: Date }

export function nixPathLockStatus(document: J | undefined, name: string, url: string): NixPathLockStatus {
  const entry = arr(get(document, 'nixPath'))?.find(row => str(get(row, 'name')) === name)
  if (!entry) return { kind: 'unlocked' }
  if (str(get(entry, 'url')) !== url) return { kind: 'urlChanged' }
  const rev = str(get(entry, 'locked', 'rev'))
  if (!rev) return { kind: 'unlocked' }
  const seconds = dbl(get(entry, 'locked', 'lastModified'))
  const modified = seconds === undefined ? undefined : new Date(seconds * 1000)
  return { kind: 'locked', revision: rev.slice(0, 7), modified: Number.isFinite(modified?.getTime()) ? modified : undefined }
}
