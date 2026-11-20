import { Builder, ByteBuffer } from 'flatbuffers'
import * as A from '../../shared/gen/tsugumi/agent'
import * as H from '../../shared/gen/tsugumi/host'
import { HelloRequestT } from '../../shared/gen/tsugumi/service/hello-request'
import { FailureT } from '../../shared/gen/tsugumi/service/failure'
import { Client } from '../../shared/gen/tsugumi/trace/client'
import type { AccessGroup, ChatRow } from '../../shared/chatPresentation'
import { modelErrorMessage } from '../i18n'
import { onHostClose, onHostEvent, openPageRpc, sharedHost } from './host'
import { RPCError, s, sOpt } from './rpc'

type Read = (id: string, seq: bigint, limit: number) => Promise<A.EventsPageT>
let agent: Promise<Read> | undefined

async function openAgent(): Promise<Read> {
  const { transport, token } = await openPageRpc('agent')
  let id = 0n
  const call = async (type: A.RequestPayload, value: A.RequestT['value'], expected: A.ReplyPayload) => {
    const builder = new Builder()
    const frame = new A.MessageT(++id, A.Payload.Request, new A.RequestT(type, value))
    A.Message.finishMessageBuffer(builder, frame.pack(builder))
    const response = await transport.call(builder.asUint8Array(), 0)
    const reply = A.Message.getRootAsMessage(new ByteBuffer(response)).unpack().payload as A.ReplyT
    if (reply.valueType === A.ReplyPayload.Failure) {
      const failure = reply.value as FailureT
      throw new Error(modelErrorMessage(s(failure.message), s(failure.code)))
    }
    if (reply.valueType !== expected) throw new RPCError('protocol', 'Unexpected Agent RPC reply')
    return reply.value
  }
  try {
    await call(A.RequestPayload.HelloRequest, new HelloRequestT(token, Client.Web), A.ReplyPayload.HelloResult)
    return (conversation, seq, limit) => call(A.RequestPayload.EventsRequest,
      new A.EventsRequestT(conversation, seq, limit), A.ReplyPayload.EventsPage) as Promise<A.EventsPageT>
  } catch (error) {
    transport.close()
    throw error
  } finally {
    transport.onClose = () => { agent = undefined }
  }
}

const readEvents: Read = async (id, seq, limit) => {
  if (!agent) {
    const pending = openAgent()
    agent = pending
    void pending.catch(() => { if (agent === pending) agent = undefined })
  }
  return (await agent)(id, seq, limit)
}

function chatRow(row: A.ChatRowT): ChatRow {
  return {
    id: s(row.id), kind: s(row.kind), role: s(row.role), text: s(row.text), name: s(row.name),
    arguments: s(row.arguments_), result: s(row.result), state: s(row.state), archived: row.archived,
    summaryJa: s(row.summaryJa), summaryEn: s(row.summaryEn), scopeJa: s(row.scopeJa), scopeEn: s(row.scopeEn),
    instruction: s(row.instruction), failed: row.failed, recovered: row.recovered,
    links: row.links.map(l => ({ title: s(l.title), url: s(l.url) })), children: row.children.map(chatRow),
  }
}

function accessGroup(group: A.AccessGroupT): AccessGroup {
  return { title: s(group.title), path: s(group.path), displayPath: s(group.displayPath),
    lines: group.lines.map(l => ({ request: s(l.request), operation: s(l.operation), neededNow: l.neededNow })),
  }
}

function eventFields(e: A.EventT) {
  return {
    kind: s(e.kind), text: s(e.text), message: s(e.message), input: s(e.inputJson), id: s(e.id), name: s(e.name),
    arguments: s(e.inputJson ?? e.text), delta: s(e.inputJson ?? e.text), index: e.index, ok: e.ok,
    asked: e.asked.map(s), groups: e.groups.map(accessGroup), code: sOpt(e.code),
  }
}

type AgentEvent = ReturnType<typeof eventFields> & { event?: AgentEvent }
function agentEvent(e: A.EventT): AgentEvent {
  return { ...eventFields(e), event: e.event ? agentEvent(e.event) : undefined }
}

function observe(id: string, wake: () => void) {
  const offEvent = onHostEvent((event) => {
    if (event.valueType === H.EventPayload.ConversationChanged &&
        s((event.value as H.ConversationChangedT).conversationId) === id) wake()
  })
  const offClose = onHostClose(wake)
  return () => { offEvent(); offClose() }
}

// The Host event wakes readers. Agent RPC retains the cursor and presentation
// semantics shared with other clients. Coalesce wakes while a read is in flight.
export class ConversationEvents {
  private observation?: { id: string; stop: () => void }

  constructor(private receive: (event: AgentEvent | { kind: 'presentation'; rows: ChatRow[] }) => void,
    private failed: (error: unknown) => void,
    private read = readEvents, private connect: () => Promise<unknown> = sharedHost, private watch = observe) {}

  start(id: string, after: bigint) {
    if (this.observation?.id === id) return
    this.close()
    let active = true
    let reading = false
    let dirty = false
    let seq = after
    // Use the Agent protocol's page size to bound each reply. A full page
    // requires another read even when its notifications were coalesced.
    const limit = 200
    const drain = async () => {
      dirty = true
      if (reading || !active) return
      reading = true
      try {
        while (dirty && active) {
          dirty = false
          await this.connect()
          if (!active) return
          const result = await this.read(id, seq, limit)
          if (!active) return
          seq = result.nextSeq
          for (const entry of result.events) {
            if (!active) return
            this.receive(agentEvent(entry.event!))
          }
          if (active && result.hasRows) this.receive({ kind: 'presentation', rows: result.rows.map(chatRow) })
          dirty ||= result.events.length === limit
        }
      } catch (error) {
        if (active) {
          this.close()
          this.failed(error)
        }
      } finally {
        reading = false
      }
    }
    const off = this.watch(id, () => void drain())
    this.observation = { id, stop: () => { active = false; off() } }
    void drain()
  }

  close() {
    this.observation?.stop()
    this.observation = undefined
  }
}
