// Copies the license files of npm packages (and of the packages they depend on,
// as web/package-lock.json resolves them) into a folder, with a list of them.
// A package without a license file stops the build, unless named in NO_FILE.
//
//   node web/scripts/licenses.mjs viewer <folder>   the plot viewer's libraries (macOS/build-app.sh)
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const lock = JSON.parse(fs.readFileSync(path.join(web, 'package-lock.json'), 'utf8')).packages

// The minified builds the viewer loads contain their dependencies, without notices.
export const VIEWER_PACKAGES = ['plotly.js-dist-min', 'vega', 'vega-lite', 'vega-embed']

// Published without a license file: their package.json declares the license.
const NO_FILE = new Set()

// The folder npm resolves `name` to from a package's folder.
const resolve = (from, name) => {
  for (let at = from; ; at = at.slice(0, at.lastIndexOf('node_modules/')).replace(/\/$/, '')) {
    const key = `${at ? `${at}/` : ''}node_modules/${name}`
    if (lock[key]) return key
    if (!at) throw new Error(`${name} (needed by ${from}) is not in package-lock.json`)
  }
}

export const withDependencies = (roots) => {
  const seen = new Set()
  const visit = (key) => {
    if (seen.has(key)) return
    seen.add(key)
    for (const name of Object.keys(lock[key].dependencies ?? {})) visit(resolve(key, name))
  }
  for (const name of roots) visit(`node_modules/${name}`)
  return seen
}

// keys: package-lock.json keys (node_modules/<name>, nested too).
export const writeLicenses = (keys, folder) => {
  const rows = []
  for (const key of [...keys].sort()) {
    const name = key.slice(key.lastIndexOf('node_modules/') + 'node_modules/'.length)
    const from = path.join(web, key)
    const files = fs.readdirSync(from).filter((f) => /^(licen[cs]e|copying|notice)/i.test(f) && fs.statSync(path.join(from, f)).isFile())
    if (!files.length && !NO_FILE.has(name)) throw new Error(`${name} has no license file`)
    const to = path.join(folder, key.replace(/node_modules\//g, ''))
    fs.mkdirSync(to, { recursive: true })
    for (const f of files) fs.copyFileSync(path.join(from, f), path.join(to, f))
    rows.push(`| ${name} | ${lock[key].version} | ${lock[key].license ?? ''} |`)
  }
  fs.writeFileSync(path.join(folder, 'README.md'), `# npm packages\n\nFrom web/package-lock.json.\n\n| Package | Version | License |\n| --- | --- | --- |\n${rows.join('\n')}\n`)
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [what, folder] = process.argv.slice(2)
  if (what !== 'viewer' || !folder) {
    console.error('usage: licenses.mjs viewer <folder>')
    process.exit(2)
  }
  writeLicenses(withDependencies(VIEWER_PACKAGES), folder)
}
