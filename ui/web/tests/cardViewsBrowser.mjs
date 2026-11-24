import assert from 'node:assert/strict'
import * as esbuild from 'esbuild'
import fs from 'node:fs/promises'
import path from 'node:path'
import { installViewer } from '../scripts/install-viewer.mjs'
import { runInChromium } from './chromium.mjs'

export async function checkCardViewsInChromium(browser, web, directory) {
  const modules = path.join(directory, 'modules')
  // Keep both fresh installs and upgrades free of bundled views.
  await fs.mkdir(modules)
  await fs.writeFile(path.join(modules, 'legacy.js'), '')
  installViewer(path.join(web, '../shared/modules'), modules)
  assert.deepEqual(await fs.readdir(modules), ['viewer.html'])
  if (process.env.TSUGUMI_WEB_PUBLIC_DIR) {
    const distribution = path.join(process.env.TSUGUMI_WEB_PUBLIC_DIR, 'modules')
    assert.deepEqual(await fs.readdir(distribution), ['viewer.html'])
    assert.deepEqual(await fs.readFile(path.join(distribution, 'viewer.html')), await fs.readFile(path.join(modules, 'viewer.html')))
  }

  const html = path.join(directory, 'card-views.html')
  await esbuild.build({
    entryPoints: [path.join(web, 'tests/cardViewsBrowser.tsx')], outfile: path.join(directory, 'card-views.js'),
    bundle: true, jsx: 'automatic', loader: { '.md': 'text' }, logLevel: 'warning',
  })
  await fs.writeFile(html, '<!doctype html><div id="root"></div><script src="card-views.js"></script>')
  assert.equal(await runInChromium(browser, html, 'globalThis.cardViewsResult', ['--disable-gpu', '--disable-popup-blocking']), null)
  console.log('Chromium Card views: sessions, popups, exact bytes, fullscreen control, sandbox and teardown passed')
}

export async function checkChildWebGpuInChromium(browser, directory) {
  const html = path.join(directory, 'child-webgpu.html')
  await fs.writeFile(html, `<!doctype html><meta charset="utf-8"><script>
    globalThis.childWebGpuResult = (async () => {
      const adapter = await navigator.gpu?.requestAdapter()
      if (!adapter) throw new Error('No WebGPU adapter in the opener')
      const device = await adapter.requestDevice()
      const child = window.open('', '_blank', 'popup,width=320,height=240')
      if (!child || child.opener !== window) throw new Error('No same-origin child window')
      try {
        const format = navigator.gpu.getPreferredCanvasFormat()
        const usage = GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC
        const makeCanvas = doc => {
          const canvas = doc.createElement('canvas')
          canvas.width = canvas.height = 4
          doc.body.append(canvas)
          const context = canvas.getContext('webgpu')
          context.configure({ device, format, usage })
          return context.getCurrentTexture()
        }
        device.pushErrorScope('validation')
        makeCanvas(document)
        const texture = makeCanvas(child.document)
        const encoder = device.createCommandEncoder()
        const pass = encoder.beginRenderPass({ colorAttachments: [{
          view: texture.createView(), clearValue: [1, 0, 0.25, 1], loadOp: 'clear', storeOp: 'store',
        }] })
        pass.end()
        const buffer = device.createBuffer({ size: 1024, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST })
        encoder.copyTextureToBuffer({ texture }, { buffer, bytesPerRow: 256 }, [4, 4, 1])
        device.queue.submit([encoder.finish()])
        const invalid = await device.popErrorScope()
        if (invalid) throw new Error(invalid.message)
        await buffer.mapAsync(GPUMapMode.READ)
        return { format, pixel: Array.from(new Uint8Array(buffer.getMappedRange(), 0, 4)) }
      } finally {
        child.close()
        device.destroy()
      }
    })()
  </script>`)
  const result = await runInChromium(browser, html, 'globalThis.childWebGpuResult', [
    '--enable-unsafe-webgpu', '--disable-popup-blocking',
  ])
  assert.ok(['rgba8unorm', 'bgra8unorm'].includes(result.format), result.format)
  assert.deepEqual(result.pixel, result.format === 'bgra8unorm' ? [64, 0, 255, 255] : [255, 0, 64, 255])
  console.log(`Chromium WebGPU: child canvas rendered with the opener device (${result.format}, ${result.pixel})`)
}
