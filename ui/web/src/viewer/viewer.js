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
  const replies = new Map()
  let nextSend = 0
  const flush = () => {
    if (!port) return
    for (const data of pending.splice(0)) port.postMessage(data, [data.bytes])
  }

  window.tsugumi = {
    onmessage: null,
    async send(bytes, to) {
      if (!(bytes instanceof Uint8Array) || typeof to !== 'string' || !to || to.includes('\0'))
        throw new TypeError('Expected Uint8Array bytes and a Card path or "*"')
      const size = Math.max(1, bytes.byteLength) + 2 * to.length
      if (bytes.byteLength > max || to.length > max / 2 || used > max) {
        if (!reportedDrop && port) port.postMessage({ type: 'dropped' })
        reportedDrop = true
        throw new Error('The view message queue is full')
      }
      used += size
      const id = ++nextSend
      return await new Promise((resolve, reject) => {
        replies.set(id, { size, resolve, reject })
        pending.push({ type: 'data', id, bytes: new Uint8Array(bytes).buffer, to })
        flush()
      })
    },
  }

  const connect = event => {
    if (event.source !== parent || event.data?.tsugumi !== 1) return
    removeEventListener('message', connect)
    port = event.data.port
    port.onmessage = ({ data }) => {
      if (data?.type === 'sendResult') {
        const reply = replies.get(data.id)
        if (!reply) return
        replies.delete(data.id)
        used -= reply.size
        reportedDrop = false
        if (data.error) reply.reject(new Error(data.error))
        else reply.resolve()
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
        typeof data.to === 'string' && Number.isSafeInteger(data.id)) {
        const size = Math.max(1, data.bytes.byteLength) + 2 * data.to.length
        if (data.bytes.byteLength > messageMax || data.to.length > messageMax / 2 || view.reverseBytes > budget - messageMax) {
          notify({ tsugumi: 'messageDropped' })
          view.port.postMessage({ type: 'sendResult', id: data.id, error: 'The view message queue is full' })
          return
        }
        view.reverseBytes += size
        notify({ tsugumi: 'send', bytes: data.bytes, to: data.to, id: data.id }, [data.bytes])
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
    case 'sendResult':
      if (current && Number.isSafeInteger(message.id) && Number.isInteger(message.size) && message.size >= 0) {
        current.reverseBytes = Math.max(0, current.reverseBytes - message.size)
        current.port?.postMessage({ type: 'sendResult', id: message.id, error: message.error })
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
