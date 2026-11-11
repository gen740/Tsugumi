import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import sirv from 'sirv'

const docs = fileURLToPath(new URL('../../../', import.meta.url))
const directories = ['assets', 'site/assets', 'site/js', 'site/player-data']
const files = ['site/site.css', 'site/player.css']

// Keep shared illustrations and demo recordings in one place. Astro serves them
// in development and copies them into the disposable site output for publication.
export function staticAssets() {
  return {
    name: 'tsugumi-static-assets',
    hooks: {
      'astro:server:setup': ({ server }) => {
        for (const relative of directories) {
          server.middlewares.use(`/${relative}`, sirv(path.join(docs, relative), { dev: true }))
        }
        server.middlewares.use((req, res, next) => {
          const relative = new URL(req.url, 'http://localhost').pathname.slice(1)
          if (!files.includes(relative)) return next()
          res.setHeader('Content-Type', 'text/css; charset=utf-8')
          fs.createReadStream(path.join(docs, relative)).pipe(res)
        })
        const sources = [...directories, ...files].map(relative => path.join(docs, relative))
        server.watcher.add(sources)
        server.watcher.on('change', file => {
          if (sources.some(source => file === source || file.startsWith(source + path.sep))) {
            server.ws.send({ type: 'full-reload' })
          }
        })
      },
      'astro:build:done': ({ dir }) => {
        for (const relative of [...directories, ...files]) {
          fs.cpSync(path.join(docs, relative), new URL(relative, dir), { recursive: true })
        }
      },
    },
  }
}
