// Trusted bridge between the browser UI and the sandboxed Card document.
'use strict'
const inChild = new URLSearchParams(location.search).get('child') === '1'
const policy = document.querySelector('meta[http-equiv="Content-Security-Policy"]').content
const messageMax = 1 << 24
const budget = 2 * messageMax

// Install the public API before any Card scripts run. Both directions have
// bounded queues, including before the MessagePort handshake completes.
function initializeCardView() {
  const max = 1 << 24
  let port, used = 0, reportedDrop = false
  const pending = []
  const flush = () => {
    if (!port) return
    for (const { bytes, route } of pending.splice(0))
      port.postMessage({ type: 'data', bytes, route }, [bytes, route])
  }

  window.tsugumi = {
    onmessage: null,
    send(bytes, route = new Uint8Array(16)) {
      if (!(bytes instanceof Uint8Array) || !(route instanceof Uint8Array) || route.byteLength !== 16)
        throw new TypeError('Expected Uint8Array bytes and a 16-byte route')
      if (bytes.byteLength > max || used > max) {
        if (!reportedDrop && port) port.postMessage({ type: 'dropped' })
        reportedDrop = true
        return
      }
      used += Math.max(1, bytes.byteLength)
      pending.push({ bytes: new Uint8Array(bytes).buffer, route: new Uint8Array(route).buffer })
      flush()
    },
  }

  const connect = event => {
    if (event.source !== parent || event.data?.tsugumi !== 1) return
    removeEventListener('message', connect)
    port = event.data.port
    port.onmessage = ({ data }) => {
      if (data?.type === 'reverseCredit') {
        used -= Math.max(1, data.size)
        reportedDrop = false
      } else if (data?.type === 'data') {
        try {
          window.tsugumi.onmessage?.(new Uint8Array(data.bytes))
        } finally {
          port.postMessage({ type: 'credit' })
        }
      }
    }
    flush()
    if (reportedDrop) port.postMessage({ type: 'dropped' })
  }
  addEventListener('message', connect)
}

const apiScript = `<script>(${initializeCardView.toString()})()<\/script>`

let current

function notify(message, transfer = []) {
  if (parent !== window) parent.postMessage(message, '*', transfer)
}

function dispose() {
  if (!current) return
  current.port?.close()
  current.frame.remove()
  current = undefined
}

function flush(view) {
  while (view.port && view.pending.length && view.outBytes <= budget - messageMax) {
    const { bytes, ticket } = view.pending.shift()
    const size = Math.max(1, bytes.byteLength)
    view.outBytes += size
    view.out.push(size)
    view.port.postMessage({ type: 'data', bytes }, [bytes])
    notify({ tsugumi: 'credit', ticket })
  }
}

function mount(html) {
  dispose()
  const frame = document.createElement('iframe')
  frame.setAttribute('sandbox', 'allow-scripts')
  frame.title = 'Card view'
  const content = html.replace(/^\s*<!doctype\s+html\s*>/i, '')
  frame.srcdoc = `<!doctype html><meta http-equiv="Content-Security-Policy" content="${policy}">${apiScript}${content}`
  const view = current = { frame, pending: [], out: [], outBytes: 0, reverseBytes: 0 }
  frame.addEventListener('load', () => {
    if (current !== view) return
    const { port1, port2 } = new MessageChannel()
    view.port = port1
    port1.onmessage = ({ data }) => {
      if (current !== view) return
      if (data?.type === 'credit') {
        const sent = view.out.shift()
        if (!sent) return
        view.outBytes -= sent
        flush(view)
      } else if (data?.type === 'dropped') {
        notify({ tsugumi: 'messageDropped' })
      } else if (data?.type === 'data' && data.bytes instanceof ArrayBuffer &&
        data.route instanceof ArrayBuffer && data.route.byteLength === 16) {
        if (data.bytes.byteLength > messageMax || view.reverseBytes > budget - messageMax) {
          notify({ tsugumi: 'messageDropped' })
          view.port.postMessage({ type: 'reverseCredit', size: data.bytes.byteLength })
          return
        }
        view.reverseBytes += Math.max(1, data.bytes.byteLength)
        notify({ tsugumi: 'send', bytes: data.bytes, route: data.route }, [data.bytes, data.route])
      }
    }
    frame.contentWindow.postMessage({ tsugumi: 1, port: port2 }, '*', [port2])
    flush(view)
  }, { once: true })
  document.body.append(frame)
}

window.tsugumiView = message => {
  switch (message?.type) {
    case 'mount':
      if (typeof message.html === 'string') mount(message.html)
      return
    case 'dispose':
      dispose()
      return
    case 'data':
      if (!current || !(message.bytes instanceof ArrayBuffer)) return
      current.pending.push({ bytes: message.bytes, ticket: message.ticket })
      flush(current)
      return
    case 'reverseCredit':
      if (current && Number.isInteger(message.size) && message.size >= 0) {
        current.reverseBytes = Math.max(0, current.reverseBytes - Math.max(1, message.size))
        current.port?.postMessage({ type: 'reverseCredit', size: message.size })
      }
  }
}

addEventListener('message', event => {
  if (parent === window) return
  // React renders the child through a portal. The opener sends through that
  // portal, while its sandboxed frame replies to the child window.
  if (event.source !== parent && !(inChild && event.source === parent.opener)) return
  if (event.data?.tsugumi === 'probe') notify({ tsugumi: 'ready' })
  if (event.data?.tsugumi === 'view') window.tsugumiView(event.data.message)
})
addEventListener('pagehide', dispose)
notify({ tsugumi: 'ready' })
