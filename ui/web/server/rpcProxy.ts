// The page authenticates at the HTTP origin. hostd authenticates its binary
// Hello independently and owns all WebSocket framing, validation and RPC.
import type { IncomingMessage } from 'node:http'
import net from 'node:net'
import type { Duplex } from 'node:stream'

export function pageRpcUpgrade(req: IncomingMessage, port: number, authorized: boolean): boolean {
  const host = req.headers.host
  const key = req.headers['sec-websocket-key']
  return req.url === '/rpc/host' && req.method === 'GET' && authorized &&
    (host === `127.0.0.1:${port}` || host === `localhost:${port}`) && req.headers.origin === `http://${host}` &&
    req.headers.upgrade?.toLowerCase() === 'websocket' && req.headers['sec-websocket-protocol'] === 'tsugumi.host.v1' &&
    req.headers['sec-websocket-version'] === '13' && typeof key === 'string' && /^[A-Za-z0-9+/]{22}==$/.test(key)
}

export function proxyHostRpc(req: IncomingMessage, page: Duplex, head: Buffer, endpoint: string) {
  const url = new URL(endpoint)
  const host = net.createConnection({ host: url.hostname, port: Number(url.port) })
  page.once('error', () => host.destroy())
  page.once('close', () => host.destroy())
  host.once('error', () => page.destroy())
  host.once('close', () => page.end())
  // The edge checked Origin. Forward only the WebSocket handshake fields;
  // the page cookie and its HTTP headers never leave the edge.
  host.write([
    'GET /rpc HTTP/1.1', `Host: ${url.host}`, 'Upgrade: websocket', 'Connection: Upgrade',
    'Sec-WebSocket-Version: 13', `Sec-WebSocket-Key: ${req.headers['sec-websocket-key']}`,
    'Sec-WebSocket-Protocol: tsugumi.host.v1', '', '',
  ].join('\r\n'))
  host.write(head)
  page.pipe(host).pipe(page)
}
