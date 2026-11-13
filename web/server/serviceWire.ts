import { ExecutionServicesRequestT } from '../src/gen/tsugumi/host/execution-services-request'
import { ExecutionServicePutRequestT } from '../src/gen/tsugumi/host/execution-service-put-request'
import { ExecutionServiceRemoveRequestT } from '../src/gen/tsugumi/host/execution-service-remove-request'
import { ExecutionProjectAuthorizeRequestT } from '../src/gen/tsugumi/host/execution-project-authorize-request'
import { ExecutionProjectRevokeRequestT } from '../src/gen/tsugumi/host/execution-project-revoke-request'
import { DemoListRequestT as ADemoListRequest } from '../src/gen/tsugumi/agent/demo-list-request'
import { ToolCatalogRequestT as AToolCatalogRequest } from '../src/gen/tsugumi/agent/tool-catalog-request'
import { HostRecentListRequestT as HHostRecentListRequest } from '../src/gen/tsugumi/host/host-recent-list-request'
import { HostProjectDefaultsRequestT as HHostProjectDefaultsRequest } from '../src/gen/tsugumi/host/host-project-defaults-request'
// Binary service adapters. Only model content and open-ended tool data are JSON.
import * as fb from 'flatbuffers'
import { Message as AgentMessage, MessageT as AgentMessageT } from '../src/gen/tsugumi/agent/message'
import { RequestT as AgentRequestT } from '../src/gen/tsugumi/agent/request'
import { RequestPayload as AgentRequestPayload } from '../src/gen/tsugumi/agent/request-payload'
import { Payload as AgentPayload } from '../src/gen/tsugumi/agent/payload'
import { Message as HostMessage, MessageT as HostMessageT } from '../src/gen/tsugumi/host/message'
import { RequestT as HostRequestT } from '../src/gen/tsugumi/host/request'
import { RequestPayload as HostRequestPayload } from '../src/gen/tsugumi/host/request-payload'
import { Payload as HostPayload } from '../src/gen/tsugumi/host/payload'
import { ProviderT } from '../src/gen/tsugumi/service/provider'
import { CredentialRegistrationT } from '../src/gen/tsugumi/host/credential-registration'
import { SkillReferenceT } from '../src/gen/tsugumi/agent/skill-reference'
import { Action } from '../src/gen/tsugumi/agent/action'
import { Access } from '../src/gen/tsugumi/agent/access'
import { GrantScope } from '../src/gen/tsugumi/agent/grant-scope'
import { HelloRequestT as AHelloRequest } from '../src/gen/tsugumi/service/hello-request'
import { ConfigureRequestT as AConfigureRequest } from '../src/gen/tsugumi/agent/configure-request'
import { ConversationCreateRequestT as AConversationCreateRequest } from '../src/gen/tsugumi/agent/conversation-create-request'
import { ConversationListRequestT as AConversationListRequest } from '../src/gen/tsugumi/agent/conversation-list-request'
import { ConversationGetRequestT as AConversationGetRequest } from '../src/gen/tsugumi/agent/conversation-get-request'
import { ConversationDeleteRequestT as AConversationDeleteRequest } from '../src/gen/tsugumi/agent/conversation-delete-request'
import { ConversationSearchRequestT as AConversationSearchRequest } from '../src/gen/tsugumi/agent/conversation-search-request'
import { ConversationSendRequestT as AConversationSendRequest } from '../src/gen/tsugumi/agent/conversation-send-request'
import { ConversationActionRequestT as AConversationActionRequest } from '../src/gen/tsugumi/agent/conversation-action-request'
import { ConversationCancelRequestT as AConversationCancelRequest } from '../src/gen/tsugumi/agent/conversation-cancel-request'
import { EventsRequestT as AEventsRequest } from '../src/gen/tsugumi/agent/events-request'
import { ToolCallRequestT as AToolCallRequest } from '../src/gen/tsugumi/agent/tool-call-request'
import { AccessRequestT as AAccessRequest } from '../src/gen/tsugumi/agent/access-request'
import { AccessResolveRequestT as AAccessResolveRequest } from '../src/gen/tsugumi/agent/access-resolve-request'
import { AccessRevokeRequestT as AAccessRevokeRequest } from '../src/gen/tsugumi/agent/access-revoke-request'
import { AccessListRequestT as AAccessListRequest } from '../src/gen/tsugumi/agent/access-list-request'
import { DemoReadRequestT as ADemoReadRequest } from '../src/gen/tsugumi/agent/demo-read-request'
import { VersionSummarizeRequestT as AVersionSummarizeRequest } from '../src/gen/tsugumi/agent/version-summarize-request'
import { ConversationSummarizeRequestT as AConversationSummarizeRequest } from '../src/gen/tsugumi/agent/conversation-summarize-request'
import { HelloRequestT as HHelloRequest } from '../src/gen/tsugumi/service/hello-request'
import { ConfigureRequestT as HConfigureRequest } from '../src/gen/tsugumi/host/configure-request'
import { RegisterProjectRequestT as HRegisterProjectRequest } from '../src/gen/tsugumi/host/register-project-request'
import { RegisterFolderRequestT as HRegisterFolderRequest } from '../src/gen/tsugumi/host/register-folder-request'
import { ModelsListRequestT as HModelsListRequest } from '../src/gen/tsugumi/host/models-list-request'
import { CredentialImportRequestT as HCredentialImportRequest } from '../src/gen/tsugumi/host/credential-import-request'
import { CredentialTokenRequestT as HCredentialTokenRequest } from '../src/gen/tsugumi/host/credential-token-request'
import { CredentialForgetRequestT as HCredentialForgetRequest } from '../src/gen/tsugumi/host/credential-forget-request'
import { HostProjectInfoRequestT as HHostProjectInfoRequest } from '../src/gen/tsugumi/host/host-project-info-request'
import { HostProjectCreateRequestT as HHostProjectCreateRequest } from '../src/gen/tsugumi/host/host-project-create-request'
import { HostProjectDuplicateRequestT as HHostProjectDuplicateRequest } from '../src/gen/tsugumi/host/host-project-duplicate-request'
import { HostDemoListRequestT as HHostDemoListRequest } from '../src/gen/tsugumi/host/host-demo-list-request'
import { HostDemoCopyRequestT as HHostDemoCopyRequest } from '../src/gen/tsugumi/host/host-demo-copy-request'
import { HostRecentRememberRequestT as HHostRecentRememberRequest } from '../src/gen/tsugumi/host/host-recent-remember-request'
import { HostRecentForgetRequestT as HHostRecentForgetRequest } from '../src/gen/tsugumi/host/host-recent-forget-request'

export class CoreAgentError extends Error {
  constructor(readonly code: string, message: string, readonly requestId?: string) { super(message) }
}

function enumValue(e: any, value: unknown): number {
  if (value === undefined) return 0
  const n = typeof value === 'string' ? e[value] : value
  if (typeof n !== 'number' || typeof e[n] !== 'string') throw new Error('Invalid service enum value')
  return n
}
function provider(v: any): ProviderT | null {
  if (!v) return null
  return new ProviderT(v.provider, v.api, v.auth, v.base_url, v.model, v.credential, v.credential_id,
    v.effort, v.fast, v.web_search, v.max_tokens, v.context_limit)
}
function registration(v: any): CredentialRegistrationT | null {
  if (!v) return null
  return new CredentialRegistrationT(v.client_id, v.access_token, v.refresh_token, v.token_endpoint,
    v.revocation_endpoint, v.resource, BigInt(Math.trunc(v.expires_at_ms ?? 0)), v.id_token, v.scope)
}
export function hostMethod(method: string): boolean {
  return method !== 'HelloRequest' && method !== 'ConfigureRequest' &&
    typeof (HostRequestPayload as any)[method] === 'number'
}
function request(method: string, v: any, host: boolean): fb.IGeneratedObject {
  if (host) switch (method) {
    case 'ExecutionServicesRequest': return new ExecutionServicesRequestT()
    case 'ExecutionServicePutRequest': return new ExecutionServicePutRequestT(v.id, v.name, v.endpoint, v.workerPublicKey, v.verify)
    case 'ExecutionServiceRemoveRequest': return new ExecutionServiceRemoveRequestT(v.id)
    case 'ExecutionProjectAuthorizeRequest': return new ExecutionProjectAuthorizeRequestT(v.project_id, v.path, v.control_dir, v.guest)
    case 'ExecutionProjectRevokeRequest': return new ExecutionProjectRevokeRequestT(v.project_id)

    case 'HostRecentListRequest': return new HHostRecentListRequest()
    case 'HostProjectDefaultsRequest': return new HHostProjectDefaultsRequest()
    case 'HelloRequest': return new HHelloRequest(v.token, v.client_id, v.protocol)
    case 'ConfigureRequest': return new HConfigureRequest(provider(v.provider))
    case 'RegisterProjectRequest': return new HRegisterProjectRequest(v.project_id, v.endpoint, v.token, v.title)
    case 'RegisterFolderRequest': return new HRegisterFolderRequest(v.project_id, v.resource_id, v.folder, v.title)
    case 'ModelsListRequest': return new HModelsListRequest(provider(v.provider))
    case 'CredentialImportRequest': return new HCredentialImportRequest(registration(v.registration), v.replace)
    case 'CredentialTokenRequest': return new HCredentialTokenRequest(v.client_id, v.stale_token)
    case 'CredentialForgetRequest': return new HCredentialForgetRequest(v.client_id)
    case 'HostProjectInfoRequest': return new HHostProjectInfoRequest(v.path)
    case 'HostProjectCreateRequest': return new HHostProjectCreateRequest(v.parent, v.name, v.flow)
    case 'HostProjectDuplicateRequest': return new HHostProjectDuplicateRequest(v.path, v.parent, v.name)
    case 'HostDemoListRequest': return new HHostDemoListRequest(v.language)
    case 'HostDemoCopyRequest': return new HHostDemoCopyRequest(v.id, v.parent, v.language)
    case 'HostRecentRememberRequest': return new HHostRecentRememberRequest(v.path)
    case 'HostRecentForgetRequest': return new HHostRecentForgetRequest(v.paths)
  }
  else switch (method) {
    case 'DemoListRequest': return new ADemoListRequest()
    case 'ToolCatalogRequest': return new AToolCatalogRequest()
    case 'HelloRequest': return new AHelloRequest(v.token, v.client_id, v.protocol)
    case 'ConfigureRequest': return new AConfigureRequest(provider(v.provider), v.conversation_id)
    case 'ConversationCreateRequest': return new AConversationCreateRequest(v.conversation_id, v.project_id, v.role)
    case 'ConversationListRequest': return new AConversationListRequest(v.project_id)
    case 'ConversationGetRequest': return new AConversationGetRequest(v.conversation_id)
    case 'ConversationDeleteRequest': return new AConversationDeleteRequest(v.conversation_id)
    case 'ConversationSearchRequest': return new AConversationSearchRequest(v.project_id, v.text)
    case 'ConversationSendRequest': return new AConversationSendRequest(v.conversation_id, v.turn_id, v.text, v.selected_cards, v.selected_displays, v.skill_names, v.intent, v.attached_resources, (v.skill_references ?? []).map((s: any) => new SkillReferenceT(s.name, s.resource_id, s.path)))
    case 'ConversationActionRequest': return new AConversationActionRequest(v.conversation_id, enumValue(Action, v.action))
    case 'ConversationCancelRequest': return new AConversationCancelRequest(v.conversation_id)
    case 'EventsRequest': return new AEventsRequest(v.conversation_id, BigInt(v.after_seq ?? 0), v.limit)
    case 'ToolCallRequest': return new AToolCallRequest(v.conversation_id, v.project_id, v.name, v.arguments_json, v.op_id)
    case 'AccessRequest': return new AAccessRequest(v.project_id, v.conversation_id, enumValue(Access, v.access), enumValue(GrantScope, v.scope), v.reason)
    case 'AccessResolveRequest': return new AAccessResolveRequest(v.request_id, v.allow)
    case 'AccessRevokeRequest': return new AAccessRevokeRequest(v.grant_id)
    case 'AccessListRequest': return new AAccessListRequest(v.conversation_id)
    case 'DemoReadRequest': return new ADemoReadRequest(v.path)
    case 'VersionSummarizeRequest': return new AVersionSummarizeRequest(v.project_id, v.commit)
    case 'ConversationSummarizeRequest': return new AConversationSummarizeRequest(v.conversation_id, provider(v.provider))
  }
  throw new Error(`Unknown ${host ? 'Host' : 'Agent'} request: ${method}`)
}
export function encodeRequest(method: string, value: unknown, id: bigint, host: boolean): Uint8Array {
  const builder = new fb.Builder()
  const payload = request(method, value, host)
  if (host) {
    const tag = (HostRequestPayload as any)[method] as HostRequestPayload
    const message = new HostMessageT(id, HostPayload.Request, new HostRequestT(tag, payload as never))
    HostMessage.finishMessageBuffer(builder, message.pack(builder))
  } else {
    const tag = (AgentRequestPayload as any)[method] as AgentRequestPayload
    const message = new AgentMessageT(id, AgentPayload.Request, new AgentRequestT(tag, payload as never))
    AgentMessage.finishMessageBuffer(builder, message.pack(builder))
  }
  return builder.asUint8Array()
}

// Adapt generated native objects to the existing HTTP/SSE presentation DTOs.
// This walks typed fields; it does not parse or generate a JSON envelope.
function view(value: any): any {
  if (value === null || value === undefined) return undefined
  if (Array.isArray(value)) return value.map(view)
  if (typeof value === 'bigint') {
    const n = Number(value)
    if (!Number.isSafeInteger(n)) throw new Error('Service value exceeds the JSON integer range')
    return n
  }
  if (typeof value !== 'object') return value
  const out: Record<string, any> = {}
  const names: Record<string, string> = { projectId: 'project_id', conversationId: 'conversation_id',
    eventSeq: 'event_seq', nextSeq: 'next_seq', callId: 'call_id', contextSummary: 'contextSummary',
    inputJson: 'input', argumentsJson: 'arguments', resultJson: 'result', inputSchemaJson: 'inputSchema',
    contentJson: 'content', providerContentJson: 'providerContent', providerApi: 'providerApi', mediaType: 'media_type', clientId: 'client_id', accessToken: 'access_token', capabilityFile: 'capability_file' }
  for (const [key, v] of Object.entries(value)) {
    const name = names[key] ?? key
    if (v === null || v === undefined) {
      if (['title', 'text', 'projectId', 'conversationId', 'reason', 'state'].includes(key)) out[name] = ''
      if (key === 'summary' || key === 'resultJson' && 'callId' in value) out[name] = null
      continue
    }
    if (key === 'resultJson' && 'callId' in value) { out[name] = JSON.parse(String(v)); continue }
    if (key === 'access') { out[name] = Access[v as number]; continue }
    if (key === 'scope') { out[name] = GrantScope[v as number]; continue }
    out[name] = ['seq', 'eventSeq', 'nextSeq'].includes(key) ? v : ['contentJson', 'providerContentJson', 'inputSchemaJson'].includes(key) ? JSON.parse(String(v)) : view(v)
  }
  return out
}
export function decodeReply(bytes: Uint8Array, host: boolean): { id: bigint; value: any; error?: CoreAgentError } {
  if (bytes.length < 12) throw new Error('Truncated service reply')
  const buffer = new fb.ByteBuffer(bytes)
  const identifier = host ? 'TSH1' : 'TSA1'
  if (!buffer.__has_identifier(identifier)) throw new Error('Wrong service identifier')
  const message = host ? HostMessage.getRootAsMessage(buffer).unpack() : AgentMessage.getRootAsMessage(buffer).unpack()
  if (message.payloadType !== (host ? HostPayload.Reply : AgentPayload.Reply) || !message.payload) throw new Error('Expected service reply')
  const reply = message.payload as any
  // Generated unpack() normalizes omitted Host vectors to empty arrays.
  const value = view(reply.value)
  if (!value) throw new Error('Missing service reply value')
  if (value.code !== undefined) return { id: message.requestId, value: undefined,
    error: new CoreAgentError(value.code, value.message, reply.value.requestId ?? undefined) }
  let result = value
  // Vector result tables keep the public API's array shape.
  for (const key of ['conversations', 'matches', 'paths', 'demos', 'models', 'projects', 'tools', 'services']) {
    if (Object.keys(value).length === 1 && Array.isArray(value[key])) { result = value[key]; break }
  }
  return { id: message.requestId, value: result }
}
