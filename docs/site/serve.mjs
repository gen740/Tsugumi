// The documentation site with live reload, for working on the documents and the
// figures. A change to a document or to docs/site/ rebuilds with build.mjs and
// reloads the open pages (a stylesheet is swapped in place); a build that fails
// shows its error on the page. The script is added as a page is served, so the
// generated files stay what build.mjs --check expects.
//   node docs/site/serve.mjs [port]   (or PORT=...; 127.0.0.1 only)
import { spawn } from 'node:child_process'
import fs from 'node:fs'
import http from 'node:http'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const SITE = path.dirname(fileURLToPath(import.meta.url))
const DOCS = path.dirname(SITE)
const ROOT = path.dirname(DOCS)
const PORT = Number(process.argv[2] ?? process.env.PORT ?? 4100)

/** What build.mjs writes under docs/: a change there is the build's own. */
const GENERATED = /^(pages\/|index\.html$|index\.md$|llms\.txt$|\.nojekyll$)/
/** Documents at the top of the repository that the site has pages for. */
const TOP = new Set(['README.md', 'INSTALL.md', 'THIRD_PARTY_NOTICES.md'])

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.md': 'text/markdown; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
}

const CLIENT = `<script>{
  const live = new EventSource('/__live')
  live.addEventListener('reload', () => location.reload())
  live.addEventListener('css', () => {
    for (const l of document.querySelectorAll('link[rel=stylesheet]')) {
      const url = new URL(l.href)
      url.searchParams.set('live', Date.now())
      l.href = url
    }
  })
  live.addEventListener('failed', (e) => {
    let box = document.getElementById('live-error')
    if (!box) {
      box = document.createElement('pre')
      box.id = 'live-error'
      box.style.cssText = 'position:fixed;inset:auto 16px 16px 16px;z-index:9999;max-height:50vh;overflow:auto;margin:0;padding:12px 14px;background:#2b1414;color:#ffd9d4;font:12px/1.5 ui-monospace,monospace;border-radius:8px;white-space:pre-wrap'
      document.body.append(box)
    }
    box.textContent = JSON.parse(e.data)
  })
}</script>`

const clients = new Set()
function send(event, data = '') {
  for (const c of clients) c.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`)
}

function build() {
  return new Promise((resolve) => {
    let log = ''
    const p = spawn(process.execPath, [path.join(SITE, 'build.mjs')], { stdio: ['ignore', 'pipe', 'pipe'] })
    p.stdout.on('data', (d) => { log += d })
    p.stderr.on('data', (d) => { log += d })
    p.on('close', (code) => resolve({ ok: code === 0, log: log.trim() }))
  })
}

let timer = null
let building = false
let pending = { build: false, reload: false, css: false }

async function flush() {
  if (building) return
  const now = pending
  pending = { build: false, reload: false, css: false }
  if (now.build) {
    building = true
    const started = Date.now()
    const r = await build()
    building = false
    console.log(r.ok ? `built in ${Date.now() - started} ms` : `build failed:\n${r.log}`)
    if (!r.ok) send('failed', r.log)
    else send('reload')
    if (pending.build || pending.reload || pending.css) flush()
    return
  }
  if (now.reload) send('reload')
  else if (now.css) send('css')
}

/** Editors write a file in several steps, so changes are gathered for a moment. */
function changed(rel) {
  if (rel.endsWith('.css')) pending.css = true
  else if (rel.endsWith('.md') || rel === 'docs/site/build.mjs') pending.build = true
  else pending.reload = true
  clearTimeout(timer)
  timer = setTimeout(flush, 80)
}

fs.watch(DOCS, { recursive: true }, (_, name) => {
  if (!name) return
  const rel = name.split(path.sep).join('/')
  if (GENERATED.test(rel) || rel.endsWith('.DS_Store') || rel === 'site/serve.mjs') return
  changed(`docs/${rel}`)
})
fs.watch(ROOT, (_, name) => { if (TOP.has(name)) changed(name) })

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost')
  if (url.pathname === '/__live') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store' })
    res.write(': connected\n\n')
    clients.add(res)
    req.on('close', () => clients.delete(res))
    return
  }
  let file = path.join(DOCS, decodeURIComponent(url.pathname))
  if (file !== DOCS && !file.startsWith(DOCS + path.sep)) {
    res.writeHead(403).end('forbidden')
    return
  }
  if (fs.existsSync(file) && fs.statSync(file).isDirectory()) file = path.join(file, 'index.html')
  if (!fs.existsSync(file)) {
    res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' }).end(`not found: ${url.pathname}`)
    return
  }
  const ext = path.extname(file).toLowerCase()
  const headers = { 'Content-Type': TYPES[ext] ?? 'application/octet-stream', 'Cache-Control': 'no-store' }
  if (ext === '.html') {
    res.writeHead(200, headers).end(fs.readFileSync(file, 'utf8').replace('</body>', `${CLIENT}\n</body>`))
  } else {
    res.writeHead(200, headers)
    fs.createReadStream(file).pipe(res)
  }
})

const first = await build()
if (!first.ok) console.error(`build failed:\n${first.log}`)
server.listen(PORT, '127.0.0.1', () => console.log(`docs: http://localhost:${PORT}/ (${first.ok ? first.log : 'last good build'})`))
