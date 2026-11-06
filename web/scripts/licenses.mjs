// Copies the license files of the plot viewer's libraries and of the packages
// they depend on, as web/package-lock.json resolves them, into a folder with a
// list of them (macOS/tools/bundle-licenses.sh): the minified builds the app
// ships contain those dependencies, without notices.
//
//   node web/scripts/licenses.mjs <folder>
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const web = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const lock = JSON.parse(fs.readFileSync(path.join(web, 'package-lock.json'), 'utf8')).packages

const VIEWER_PACKAGES = ['plotly.js-dist-min', 'vega', 'vega-lite', 'vega-embed']

// The folder npm resolves `name` to from a package's folder.
const resolve = (from, name) => {
  for (let at = from; ; at = at.slice(0, at.lastIndexOf('node_modules/')).replace(/\/$/, '')) {
    const key = `${at ? `${at}/` : ''}node_modules/${name}`
    if (lock[key]) return key
    if (!at) throw new Error(`${name} (needed by ${from}) is not in package-lock.json`)
  }
}

const withDependencies = (roots) => {
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
const writeLicenses = (keys, folder) => {
  const rows = []
  for (const key of [...keys].sort()) {
    const name = key.slice(key.lastIndexOf('node_modules/') + 'node_modules/'.length)
    const from = path.join(web, key)
    const files = fs.readdirSync(from).filter((f) => /^(licen[cs]e|copying|notice)/i.test(f) && fs.statSync(path.join(from, f)).isFile())
    if (!files.length) throw new Error(`${name} has no license file`)
    const to = path.join(folder, key.replace(/node_modules\//g, ''))
    fs.mkdirSync(to, { recursive: true })
    for (const f of files) fs.copyFileSync(path.join(from, f), path.join(to, f))
    rows.push(`| ${name} | ${lock[key].version} | ${lock[key].license ?? ''} |`)
  }
  fs.writeFileSync(path.join(folder, 'README.md'), `# npm packages\n\nFrom web/package-lock.json.\n\n| Package | Version | License |\n| --- | --- | --- |\n${rows.join('\n')}\n`)
}

const folder = process.argv[2]
if (!folder) {
  console.error('usage: licenses.mjs <folder>')
  process.exit(2)
}
writeLicenses(withDependencies(VIEWER_PACKAGES), folder)
