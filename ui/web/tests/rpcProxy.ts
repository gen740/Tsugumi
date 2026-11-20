import assert from 'node:assert/strict'
import type { IncomingMessage } from 'node:http'
import { pageRpcUpgrade } from '../server/rpcProxy'

const request = {
  url: '/rpc/host', method: 'GET',
  headers: {
    host: 'localhost:8780', origin: 'http://localhost:8780', upgrade: 'websocket',
    'sec-websocket-protocol': 'tsugumi.host.v1', 'sec-websocket-version': '13',
    'sec-websocket-key': 'dGhlIHNhbXBsZSBub25jZQ==',
  },
}
function accepted(headers = {}, pageCookie = true, url = request.url) {
  return pageRpcUpgrade({ ...request, url, headers: { ...request.headers, ...headers } } as IncomingMessage, 8780, pageCookie)
}
assert.equal(accepted(), 'host')
assert.equal(accepted({ host: '127.0.0.1:8780', origin: 'http://127.0.0.1:8780' }), 'host')
assert.equal(accepted({ 'x-tsugumi-agent': 'mcp-key' }, false), undefined)
assert.equal(accepted({ origin: undefined }), undefined)
assert.equal(accepted({ origin: 'null' }), undefined)
assert.equal(accepted({ origin: 'http://127.0.0.1:8780' }), undefined)
assert.equal(accepted({ host: 'other.test:8780', origin: 'http://other.test:8780' }), undefined)
assert.equal(accepted({ 'sec-websocket-protocol': 'tsugumi.agent.v1' }), undefined)
assert.equal(accepted({}, true, '/rpc/host?key=page-key'), undefined)
assert.equal(accepted({ 'sec-websocket-protocol': 'tsugumi.agent.v1' }, true, '/rpc/agent'), 'agent')
