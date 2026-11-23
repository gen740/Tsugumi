import { get, J, str } from '../../shared/json'

export type NixPathLockStatus =
  | { kind: 'unlocked' }
  | { kind: 'urlChanged' }
  | { kind: 'locked'; revision: string; modified?: Date }

export function nixPathLockStatus(document: J | undefined, name: string, url: string): NixPathLockStatus {
  const entry = str(get(document, 'nixPath', name))
  if (!entry) return { kind: 'unlocked' }
  try {
    const pin = new URL(entry)
    const intent = new URL(url)
    const [owner, repo, rev] = pin.pathname.split('/')
    const [wantedOwner, wantedRepo] = intent.pathname.split('/')
    if (pin.protocol !== intent.protocol || owner !== wantedOwner || repo !== wantedRepo) return { kind: 'urlChanged' }
    if (!rev) return { kind: 'unlocked' }
    const seconds = pin.searchParams.get('lastModified')
    const modified = seconds === null ? undefined : new Date(Number(seconds) * 1000)
    return { kind: 'locked', revision: rev.slice(0, 7), modified: Number.isFinite(modified?.getTime()) ? modified : undefined }
  } catch {
    return { kind: 'urlChanged' }
  }
}
