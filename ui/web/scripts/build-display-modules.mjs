import * as esbuild from 'esbuild'
import fs from 'node:fs'
import path from 'node:path'

export async function buildDisplayModules(viewer, dependencies, out) {
  fs.mkdirSync(out, { recursive: true })
  const script = fs.readFileSync(path.join(viewer, 'viewer.js'), 'utf8')
  if (script.includes('</script')) throw new Error('viewer.js cannot be inlined: it contains </script')
  const page = fs.readFileSync(path.join(viewer, 'viewer.html'), 'utf8').replace('<script src="viewer.js"></script>', () => `<script>\n${script}</script>`)
  if (!page.includes(script)) throw new Error('viewer.html has no viewer script to inline')
  fs.writeFileSync(path.join(out, 'viewer.html'), page)

  const library = {
    'plotly.umd': 'plotly.js-dist-min/plotly.min.js',
    'vega.umd': 'vega/build/vega.min.js',
    'vega-lite.umd': 'vega-lite/build/vega-lite.min.js',
    'vega-embed.umd': 'vega-embed/build/vega-embed.min.js',
  }
  await esbuild.build({
    entryPoints: ['text', 'markdown', 'scalar', 'table', 'image', 'points', 'line-chart', 'plotly', 'vega-lite']
      .map(name => path.join(viewer, 'builtin', name + (name === 'markdown' ? '.jsx' : '.js'))),
    outdir: out, bundle: true, format: 'esm', platform: 'browser', target: 'es2022', jsx: 'automatic',
    minify: true, legalComments: 'none', nodePaths: [dependencies],
    define: { 'process.env.NODE_ENV': '"production"' },
    plugins: [{
      name: 'offline-chart-libraries',
      setup(build) {
        build.onResolve({ filter: /^\.\/.*\.umd$/ }, ({ path: name }) => ({
          path: path.join(dependencies, library[name.slice(2)]), namespace: 'chart-library',
        }))
        build.onLoad({ filter: /.*/, namespace: 'chart-library' }, async ({ path: file }) => {
          const result = await esbuild.transform(fs.readFileSync(file, 'utf8'), { minify: true, legalComments: 'none' })
          return { contents: result.code, loader: 'text' }
        })
      },
    }],
  })
}
