import { Builder, ByteBuffer } from 'flatbuffers'
import * as H from '../../shared/gen/tsugumi/host'
import { HelloRequestT } from '../../shared/gen/tsugumi/service/hello-request'
import { Client } from '../../shared/gen/tsugumi/trace/client'
import { api } from '../api'
import { modelErrorMessage } from '../i18n'
import { RPCError, s, webSocketTransport, type Transport } from './rpc'

export class HostConnection {
  private id = 0n

  constructor(private transport: Transport) {}

  async call<T>(type: H.RequestPayload, value: H.RequestT['value'], expected: H.ReplyPayload): Promise<T> {
    const builder = new Builder()
    const frame = new H.MessageT(++this.id, H.Payload.Request, new H.RequestT(type, value))
    H.Message.finishMessageBuffer(builder, frame.pack(builder))
    const response = await this.transport.call(builder.asUint8Array(), 0)
    const reply = H.Message.getRootAsMessage(new ByteBuffer(response)).unpack().payload as H.ReplyT
    if (reply.valueType === H.ReplyPayload.Failure) {
      const failure = reply.value as import('../../shared/gen/tsugumi/service/failure').FailureT
      throw new Error(modelErrorMessage(s(failure.message), s(failure.code)))
    }
    if (reply.valueType !== expected) throw new RPCError('protocol', 'Unexpected Host RPC reply')
    return reply.value as T
  }

  onEvent(fn: (event: H.EventT) => void) {
    this.transport.onEvent = (frame) => fn(H.Message.getRootAsMessage(new ByteBuffer(frame)).unpack().payload as H.EventT)
  }

  onClose(fn: (reason: string) => void) { this.transport.onClose = fn }
  close() { this.transport.close() }
}

export async function openPageRpc(service: 'host' | 'agent') {
  const { token } = await api<{ token: string }>('/api/host/connect', {})
  const transport = await webSocketTransport(`ws://${location.host}/rpc/${service}`, undefined, service === 'host' ? 2 : 1)
  return { token, transport }
}

export async function openHost(): Promise<HostConnection> {
  const { token, transport } = await openPageRpc('host')
  const client = new HostConnection(transport)
  try {
    await client.call(H.RequestPayload.HelloRequest, new HelloRequestT(token, Client.Web), H.ReplyPayload.HelloResult)
    return client
  } catch (error) {
    client.close()
    throw error
  }
}

let connection: Promise<HostConnection> | undefined

const listeners = new Set<(event: H.EventT) => void>()
const closeListeners = new Set<() => void>()

export function onHostEvent(fn: (event: H.EventT) => void): () => void {
  listeners.add(fn)
  return () => listeners.delete(fn)
}

export function onHostClose(fn: () => void): () => void {
  closeListeners.add(fn)
  return () => closeListeners.delete(fn)
}

export function sharedHost(): Promise<HostConnection> {
  if (!connection) {
    const current = openHost()
    connection = current
    const clear = () => { if (connection === current) connection = undefined }
    void current.then((client) => {
      client.onClose(() => {
        clear()
        for (const listener of closeListeners) listener()
      })
      client.onEvent((event) => { for (const listener of listeners) listener(event) })
    }, clear)
  }
  return connection
}

export async function hostCall<T>(type: H.RequestPayload, value: H.RequestT['value'], expected: H.ReplyPayload): Promise<T> {
  return (await sharedHost()).call<T>(type, value, expected)
}
