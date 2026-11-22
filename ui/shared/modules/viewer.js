// Trusted bridge. Each module identity gets a fresh, opaque-origin document.
'use strict'
const policy = "default-src 'none'; script-src blob: 'unsafe-inline' 'unsafe-eval'; style-src 'unsafe-inline'; img-src blob: data:; font-src data:; connect-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'"

function runtime() {
  const root = document.createElement('div')
  root.style.cssText = 'position:absolute;inset:0;overflow:auto'
  document.body.append(root)
  const state = (value, message) => parent.postMessage({ tsugumi: 'state', state: value, message }, '*')
  const fail = error => state('error', String(error?.message ?? error).slice(0, 4096))
  addEventListener('error', event => fail(event.error ?? event.message))
  addEventListener('unhandledrejection', event => fail(event.reason))
  let mounted
  let updates = Promise.resolve()

  // ModuleRead still assembles the ABI 8 SDK's Wasm wrapper. Its exports are
  // private to this adapter until the SDK starts shipping default mount itself.
  async function mountABI8Wasm(module, data) {
    const size = () => ({
      width: root.clientWidth, height: root.clientHeight, pixelRatio: devicePixelRatio,
      visible: !document.hidden, colorScheme: document.documentElement.style.colorScheme,
    })
    const decode = ({ config, blobs }) => ({ config: JSON.parse(config), blobs })
    await module.initialise(root, decode(data), size())
    root.addEventListener('tsugumi:data', event => {
      try { module.update(decode(event.detail)) } catch (error) { fail(error) }
    })
    const resize = () => {
      try { module.resize(size()) } catch (error) { fail(error) }
    }
    new ResizeObserver(resize).observe(root)
    addEventListener('visibilitychange', resize)
    root.addEventListener('tsugumi-view-error', event => fail(event.detail))
  }

  addEventListener('message', event => {
    if (event.source !== parent || event.data?.tsugumi !== 'render') return
    const { code, data, colorScheme } = event.data
    document.documentElement.style.colorScheme = colorScheme === 'dark' ? 'dark' : 'light'
    if (!data) return
    if (!mounted) {
      mounted = (async () => {
        const url = URL.createObjectURL(new Blob([code], { type: 'text/javascript' }))
        try {
          const module = await import(url)
          if (typeof module.default === 'function') await module.default(root, data)
          else if (code.startsWith('const tsugumiWasm=Uint8Array.from(atob("') && typeof module.initialise === 'function')
            await mountABI8Wasm(module, data)
          else throw new Error('A display module must export default mount(root, data)')
          state('ok')
        } finally {
          URL.revokeObjectURL(url)
        }
      })()
      void mounted.catch(fail)
    } else {
      updates = updates.then(async () => {
        if (await mounted.catch(() => false) === false) return
        root.dispatchEvent(new CustomEvent('tsugumi:data', { detail: data }))
      }).catch(fail)
    }
  })
  addEventListener('keydown', e => {
    if (e.key === 'Escape') parent.postMessage({ tsugumi: 'key', key: 'Escape' }, '*')
  })
  parent.postMessage({ tsugumi: 'inner-ready' }, '*')
}

let current
function notify(message) {
  if (parent !== window) parent.postMessage(message, '*')
  window.webkit?.messageHandlers?.tsugumi?.postMessage(message)
}

function makeFrame(name, code) {
  current?.frame.remove()
  const frame = document.createElement('iframe')
  frame.sandbox = 'allow-scripts'
  frame.allow = 'webgpu'
  frame.title = 'Card display'
  frame.srcdoc = `<!doctype html><meta http-equiv="Content-Security-Policy" content="${policy}"><style>html,body{margin:0;height:100%;overflow:hidden;color:CanvasText;background:Canvas;font:14px system-ui}</style><body><script>(${runtime.toString()})()<` + '/script></body>'
  current = { frame, name, code, ready: false, pending: undefined }
  document.body.append(frame)
}

function flush() {
  if (!current?.ready || !current.pending) return
  current.frame.contentWindow.postMessage({ tsugumi: 'render', ...current.pending, code: current.code }, '*')
  current.code = undefined
  current.pending = undefined
}

window.tsugumiRender = message => {
  if (typeof message.name !== 'string') return
  try {
    let data
    if (typeof message.config === 'string') {
      const limit = 16 * 1024 * 1024
      let size = new TextEncoder().encode(message.config).byteLength
      const blobs = Object.fromEntries(Object.entries(message.blobs ?? {}).map(([name, value]) => {
        const bytes = typeof value === 'string' ? Uint8Array.from(atob(value), c => c.charCodeAt(0)) : value
        if (!(bytes instanceof Uint8Array)) throw new Error('Invalid display blob')
        size += bytes.byteLength
        return [name, bytes]
      }))
      if (size > limit) throw new Error('Display data exceeds 16 MiB')
      data = { config: message.config, blobs }
    }
    if (current?.name !== message.name) {
      if (typeof message.code !== 'string' || !data) return
      makeFrame(message.name, message.code)
    }
    // Updates received while the document starts replace its initial data.
    current.pending = { data: data ?? current.pending?.data, colorScheme: message.colorScheme }
    flush()
  } catch (error) {
    notify({ tsugumi: 'state', state: 'error', message: String(error.message).slice(0, 4096) })
  }
}

addEventListener('message', event => {
  if (current && event.source === current.frame.contentWindow) {
    const message = event.data
    if (message?.tsugumi === 'inner-ready') { current.ready = true; flush() }
    if (message?.tsugumi === 'state') {
      const state = message.state === 'ok' ? 'ok' : 'error'
      document.body.dataset.state = state
      notify({ tsugumi: 'state', state, message: String(message.message ?? '').slice(0, 4096) })
    }
    if (message?.tsugumi === 'key') notify({ tsugumi: 'key', key: 'Escape' })
  } else if (event.source === parent) {
    if (event.data?.tsugumi === 'probe') notify({ tsugumi: 'ready' })
    else if (event.data?.name) window.tsugumiRender(event.data)
  }
})
addEventListener('pagehide', () => current?.frame.remove())
notify({ tsugumi: 'ready' })
