/**
 * Choices for an Input Card's Path source: every project file and folder of
 * the Card's project, written as the core reads them (relative to the Card's
 * definition folder, or project:<path> from the project root).
 */
export function pathChoices(sourcePaths: readonly string[], definitionDir: string): string[] {
  const components = definitionDir.split('/')
  if (components.at(-3) !== 'definitions') return []
  const root = components.slice(0, -3).map((part) => part + '/').join('')
  const paths = new Set<string>()
  for (const source of sourcePaths) {
    if (!source.startsWith(root)) continue
    const rel = source.slice(root.length)
    // A nested project's files belong to that project.
    const parts = rel.split('/')
    if (!rel || parts[0] === 'externals') continue
    for (let n = 1; n <= parts.length; n++) paths.add(root + parts.slice(0, n).join('/'))
  }
  return [...paths].filter((p) => p !== definitionDir.slice(0, -1)).sort().map((p) =>
    p.startsWith(definitionDir) ? p.slice(definitionDir.length) : 'project:' + p.slice(root.length))
}
