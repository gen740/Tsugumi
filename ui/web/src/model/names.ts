/** The document rule for Card ids, port names, and secret names. */
export function validName(name: string): boolean {
  return /^[a-z][a-z0-9_]{0,63}$/.exec(name)?.[0] === name
}

/** A valid stem for the core allocator. The allocator reserves the final id. */
export function cardStem(name: string): string {
  return name.replace(/[A-Z]/g, c => c.toLowerCase())
    .replace(/[^a-z0-9_]+/g, '_').replace(/^[^a-z]+/, '').slice(0, 64) || 'card'
}

export function nextInputName(names: Iterable<string>): string {
  const taken = new Set(names)
  let n = 1
  while (taken.has(`in${n}`)) n++
  return `in${n}`
}
