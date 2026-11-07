// A project's nixSources (tsugumi.json): flakes its Cards read as
// inputs.sources.<name>. The same in macOS/Sources/App/Model/NixSources.swift
// (tests/fixtures/nix-source-refs.json).

/** The name a flake gets by default: the last part of its address, as a Nix attribute. */
export function sourceName(ref: string): string {
  const address = ref.split('?')[0].replace(/\/+$/, '').replace(/\.git$/, '').replace(/\/[0-9a-f]{40}$/, '')
  const last = address.split(/[/:]/).pop() ?? ''
  const name = last.replace(/[^A-Za-z0-9_-]/g, '-')
  return /^[A-Za-z_]/.test(name) ? name : `_${name}`
}

/** What to ask for to find a locked flake's newest commit: its address without the commit and the hash. */
export function unlockedRef(ref: string): string {
  return ref.split('?')[0].replace(/\/[0-9a-f]{40}$/, '')
}

/** The commit of a locked reference, shortened, when its address says it. */
export function shortCommit(ref: string): string | undefined {
  return ref.split('?')[0].match(/\/([0-9a-f]{40})$/)?.[1].slice(0, 7)
}
