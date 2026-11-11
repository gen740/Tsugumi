// Draws a Vega-Lite spec read from stdin as SVG, with the versions the Web UI uses.
// harvest.py calls this for figures of several panels, which have no one set of points.
import fs from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..', 'web')
const { resolve } = createRequire(path.join(WEB, 'package.json'))
const load = (name) => import(pathToFileURL(resolve(name)).href)
const [vega, vl] = await Promise.all([load('vega'), load('vega-lite')])

// Without a canvas Vega guesses a text's width for Latin letters, and a Japanese legend runs off the figure.
vega.textMetrics.width = (item, text) => {
  const size = item.fontSize ?? 11
  return [...String(text ?? item.text ?? '')].reduce((w, c) => w + (c.codePointAt(0) > 0x2e7f ? size : size * 0.6), 0)
}

const spec = JSON.parse(fs.readFileSync(0, 'utf8'))
const view = new vega.View(vega.parse(vl.compile(spec).spec), { renderer: 'none' })
process.stdout.write(await view.toSVG())
