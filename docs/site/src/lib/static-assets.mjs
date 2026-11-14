import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import sirv from 'sirv'
import { preparePlayer } from '../../player/build.mjs'

const docs = fileURLToPath(new URL('../../../', import.meta.url))

// Handwritten scripts and recorded assets stay in the checkout. Player outputs
// come from the shared Nix build, or are generated before Astro uses them.
export function staticAssets() {
  let assets
  return {
    name: 'tsugumi-static-assets',
    hooks: {
      'astro:config:setup': () => {
        const player = preparePlayer()
        assets = new Map([
          ...['assets', 'site/assets', 'site/js', 'site/site.css'].map(relative => [relative, path.join(docs, relative)]),
          ...['js/player.js', 'player.css', 'player-data'].map(relative => [`site/${relative}`, path.join(player, relative)]),
        ])
      },
      'astro:server:setup': ({ server }) => {
        // Exact files precede directory middleware so a local player.js cannot
        // shadow the supplied build inside the handwritten scripts' directory.
        const files = [...assets].filter(([, source]) => fs.statSync(source).isFile())
        server.middlewares.use((req, res, next) => {
          const relative = new URL(req.url, 'http://localhost').pathname.slice(1)
          const source = files.find(([name]) => name === relative)?.[1]
          if (!source) return next()
          res.setHeader('Content-Type', relative.endsWith('.js') ? 'text/javascript; charset=utf-8' : 'text/css; charset=utf-8')
          fs.createReadStream(source).pipe(res)
        })
        for (const [relative, source] of assets) {
          if (fs.statSync(source).isDirectory()) server.middlewares.use(`/${relative}`, sirv(source, { dev: true }))
        }
        const sources = [...assets.values()]
        server.watcher.add(sources)
        server.watcher.on('change', file => {
          if (sources.some(source => file === source || file.startsWith(source + path.sep))) {
            server.ws.send({ type: 'full-reload' })
          }
        })
      },
      'astro:build:done': ({ dir }) => {
        for (const [relative, source] of assets) {
          fs.cpSync(source, new URL(relative, dir), { recursive: true })
        }
      },
    },
  }
}
