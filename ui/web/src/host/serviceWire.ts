import { L } from '../i18n'
import * as H from '../../shared/gen/tsugumi/host'
import { ModelSelectionRequestT } from '../../shared/gen/tsugumi/host/model-selection-request'
import { RuntimeAction } from '../../shared/gen/tsugumi/host/runtime-action'
import { AttachFoldersRequestT } from '../../shared/gen/tsugumi/host/attach-folders-request'
import { EnvironmentRequestT } from '../../shared/gen/tsugumi/host/environment-request'
import { EnvironmentAction } from '../../shared/gen/tsugumi/host/environment-action'
import { HostRuntimeRequestT } from '../../shared/gen/tsugumi/host/host-runtime-request'
import { EventPayload as HostEventPayload } from '../../shared/gen/tsugumi/host/event-payload'
import { SettingT } from '../../shared/gen/tsugumi/host/setting'
import { SettingsGetRequestT as HSettingsGetRequest } from '../../shared/gen/tsugumi/host/settings-get-request'
import { SettingsUpdateRequestT as HSettingsUpdateRequest } from '../../shared/gen/tsugumi/host/settings-update-request'
import { ExpectedAccountT } from '../../shared/gen/tsugumi/host/expected-account'
import { ProviderSetRequestT as HProviderSetRequest } from '../../shared/gen/tsugumi/host/provider-set-request'
import { ProviderStatusRequestT as HProviderStatusRequest } from '../../shared/gen/tsugumi/host/provider-status-request'
import { SignInBeginRequestT as HSignInBeginRequest } from '../../shared/gen/tsugumi/host/sign-in-begin-request'
import { SignInActionRequestT as HSignInActionRequest } from '../../shared/gen/tsugumi/host/sign-in-action-request'
import { SignInStatusRequestT as HSignInStatusRequest } from '../../shared/gen/tsugumi/host/sign-in-status-request'
import { ExecutionProjectAuthorizeRequestT } from '../../shared/gen/tsugumi/host/execution-project-authorize-request'
import { ExecutionProjectRevokeRequestT } from '../../shared/gen/tsugumi/host/execution-project-revoke-request'
import { DemoListRequestT as HDemoListRequest } from '../../shared/gen/tsugumi/host/demo-list-request'
import { CallIdRequestT } from '../../shared/gen/tsugumi/agent/call-id-request'
import { ToolCatalogRequestT as AToolCatalogRequest } from '../../shared/gen/tsugumi/agent/tool-catalog-request'
import { HostRecentListRequestT as HHostRecentListRequest } from '../../shared/gen/tsugumi/host/host-recent-list-request'
import { HostProjectDefaultsRequestT as HHostProjectDefaultsRequest } from '../../shared/gen/tsugumi/host/host-project-defaults-request'
// Binary service adapters. Only model content and open-ended tool data are JSON.
import * as fb from 'flatbuffers'
import { Message as AgentMessage, MessageT as AgentMessageT } from '../../shared/gen/tsugumi/agent/message'
import { RequestT as AgentRequestT } from '../../shared/gen/tsugumi/agent/request'
import { RequestPayload as AgentRequestPayload } from '../../shared/gen/tsugumi/agent/request-payload'
import { ReplyPayload as AgentReplyPayload } from '../../shared/gen/tsugumi/agent/reply-payload'
import { Payload as AgentPayload } from '../../shared/gen/tsugumi/agent/payload'
import { Message as HostMessage, MessageT as HostMessageT } from '../../shared/gen/tsugumi/host/message'
import { RequestT as HostRequestT } from '../../shared/gen/tsugumi/host/request'
import { RequestPayload as HostRequestPayload } from '../../shared/gen/tsugumi/host/request-payload'
import { Payload as HostPayload } from '../../shared/gen/tsugumi/host/payload'
import { ProviderSelectionT } from '../../shared/gen/tsugumi/service/provider-selection'
import { ProviderInputT } from '../../shared/gen/tsugumi/host/provider-input'
import { SkillsListRequestT as HSkillsListRequest } from '../../shared/gen/tsugumi/host/skills-list-request'
import { Action } from '../../shared/gen/tsugumi/agent/action'
import { HelloRequestT as AHelloRequest } from '../../shared/gen/tsugumi/service/hello-request'
import { ConfigureRequestT as AConfigureRequest } from '../../shared/gen/tsugumi/agent/configure-request'
import { ConversationCreateRequestT as AConversationCreateRequest } from '../../shared/gen/tsugumi/agent/conversation-create-request'
import { ConversationListRequestT as AConversationListRequest } from '../../shared/gen/tsugumi/agent/conversation-list-request'
import { ConversationGetRequestT as AConversationGetRequest } from '../../shared/gen/tsugumi/agent/conversation-get-request'
import { ConversationDeleteRequestT as AConversationDeleteRequest } from '../../shared/gen/tsugumi/agent/conversation-delete-request'
import { ConversationSendRequestT as AConversationSendRequest } from '../../shared/gen/tsugumi/agent/conversation-send-request'
import { ConversationActionRequestT as AConversationActionRequest } from '../../shared/gen/tsugumi/agent/conversation-action-request'
import { ConversationCancelRequestT as AConversationCancelRequest } from '../../shared/gen/tsugumi/agent/conversation-cancel-request'
import { ConversationRunWakeRequestT as AConversationRunWakeRequest } from '../../shared/gen/tsugumi/agent/conversation-run-wake-request'
import { EventsRequestT as AEventsRequest } from '../../shared/gen/tsugumi/agent/events-request'
import { ToolCallRequestT as AToolCallRequest } from '../../shared/gen/tsugumi/agent/tool-call-request'
import { ToolCallCancelRequestT as AToolCallCancelRequest } from '../../shared/gen/tsugumi/agent/tool-call-cancel-request'
import { DecisionResolveRequestT as ADecisionResolveRequest } from '../../shared/gen/tsugumi/agent/decision-resolve-request'
import { DecisionListRequestT as ADecisionListRequest } from '../../shared/gen/tsugumi/agent/decision-list-request'
import { DemoReadRequestT as HDemoReadRequest } from '../../shared/gen/tsugumi/host/demo-read-request'
import { VersionNoteRequestT as HVersionNoteRequest } from '../../shared/gen/tsugumi/host/version-note-request'
import { ConversationSummarizeRequestT as AConversationSummarizeRequest } from '../../shared/gen/tsugumi/agent/conversation-summarize-request'
import { HelloRequestT as HHelloRequest } from '../../shared/gen/tsugumi/service/hello-request'
import { RegisterProjectRequestT as HRegisterProjectRequest } from '../../shared/gen/tsugumi/host/register-project-request'
import { RegisterFolderRequestT as HRegisterFolderRequest } from '../../shared/gen/tsugumi/host/register-folder-request'
import { ModelsListRequestT as HModelsListRequest } from '../../shared/gen/tsugumi/host/models-list-request'
import { HostProjectInfoRequestT as HHostProjectInfoRequest } from '../../shared/gen/tsugumi/host/host-project-info-request'
import { HostProjectCreateRequestT as HHostProjectCreateRequest } from '../../shared/gen/tsugumi/host/host-project-create-request'
import { HostProjectDuplicateRequestT as HHostProjectDuplicateRequest } from '../../shared/gen/tsugumi/host/host-project-duplicate-request'
import { HostDemoListRequestT as HHostDemoListRequest } from '../../shared/gen/tsugumi/host/host-demo-list-request'
import { HostDemoCopyRequestT as HHostDemoCopyRequest } from '../../shared/gen/tsugumi/host/host-demo-copy-request'
import { HostRecentRememberRequestT as HHostRecentRememberRequest } from '../../shared/gen/tsugumi/host/host-recent-remember-request'
import { HostRecentForgetRequestT as HHostRecentForgetRequest } from '../../shared/gen/tsugumi/host/host-recent-forget-request'

import { GuideGetRequestT } from '../../shared/gen/tsugumi/host/guide-get-request'
import { GuideAdvanceRequestT } from '../../shared/gen/tsugumi/host/guide-advance-request'
import { GuideInputT } from '../../shared/gen/tsugumi/host/guide-input'

export class CoreHostError extends Error {
  constructor(readonly code: string, message: string, readonly requestId?: string, readonly path?: string) { super(message) }
}

function enumValue(e: any, value: unknown): number {
  if (value === undefined) return 0
  const n = typeof value === 'string' ? e[value] : value
  return n
}
function provider(v: any): ProviderSelectionT | null {
  if (!v) return null
  return new ProviderSelectionT(v.profile, v.provider_id, v.account_profile, v.model, v.use_choices, v.purpose)
}
function providerInput(v: any): ProviderInputT | null {
  return v ? new ProviderInputT(v.endpoint, v.api_key, v.model, v.max_tokens, v.context_limit) : null
}
export function hostMethod(method: string): boolean {
  return method !== 'HelloRequest' && method !== 'ConfigureRequest' &&
    typeof (HostRequestPayload as any)[method] === 'number'
}
function request(method: string, v: any, host: boolean): fb.IGeneratedObject {
  if (host) switch (method) {
    case 'DemoListRequest': return new HDemoListRequest()
    case 'DemoReadRequest': return new HDemoReadRequest(v.path)
    case 'VersionNoteRequest': return new HVersionNoteRequest(v.project_id, v.commit, v.language)
    case 'ModelSelectionRequest': return new ModelSelectionRequestT(v.profile, v.purpose)
    case 'GuideGetRequest': return new GuideGetRequestT(v.session_id, v.language)
    case 'GuideAdvanceRequest': {
      const e = v.event ?? {}
      return new GuideAdvanceRequestT(v.session_id, v.language, new GuideInputT(e.kind, e.text, e.project, e.demo, e.title,
        e.connected, e.active, e.run, e.state, e.scripted ?? true, e.expected_step))
    }
    case 'AttachFoldersRequest': return new AttachFoldersRequestT(v.project, v.folders ?? [])
    case 'EnvironmentRequest': return new EnvironmentRequestT(v.project, enumValue(EnvironmentAction, v.action), v.origins ?? [], v.folders ?? [], v.stale_store ?? null)
    case 'HostRuntimeRequest': return new HostRuntimeRequestT(enumValue(RuntimeAction, v.action), v.project_id, v.project, v.command)
    case 'SettingsGetRequest': return new HSettingsGetRequest(v.profile)
    case 'SettingsUpdateRequest': return new HSettingsUpdateRequest(v.profile, v.revision ?? '',
      (v.changes ?? []).map((s: any) => new SettingT(s.key, s.value_json)),
      v.chatgpt_account ? new ExpectedAccountT(v.chatgpt_account.profile, v.chatgpt_account.client_id) : null)
    case 'ProviderSetRequest': return new HProviderSetRequest(v.profile, v.id, providerInput(v.provider), v.remove)
    case 'ProviderStatusRequest': return new HProviderStatusRequest(v.profile, v.id)
    case 'SignInBeginRequest': return new HSignInBeginRequest(v.profile, v.client_id, v.consent, v.issuer, v.resource)
    case 'SignInActionRequest': return new HSignInActionRequest(v.profile, v.attempt_id, v.action, v.client_id)
    case 'SignInStatusRequest': return new HSignInStatusRequest(v.profile, v.attempt_id)
    case 'ExecutionServicesRequest': return new H.ExecutionServicesRequestT()
    case 'ExecutionServicePutRequest': return new H.ExecutionServicePutRequestT(v.id, v.name, v.endpoint, v.workerPublicKey, v.verify)
    case 'ExecutionProbeRemeasureRequest': return new H.ExecutionProbeRemeasureRequestT(v.id ?? null)
    case 'ExecutionServiceRemoveRequest': return new H.ExecutionServiceRemoveRequestT(v.id)
    case 'ExecutionProjectAuthorizeRequest': return new ExecutionProjectAuthorizeRequestT(v.project_id, v.path, v.control_dir, v.guest)
    case 'ExecutionProjectRevokeRequest': return new ExecutionProjectRevokeRequestT(v.project_id)

    case 'HostRecentListRequest': return new HHostRecentListRequest()
    case 'HostProjectDefaultsRequest': return new HHostProjectDefaultsRequest()
    case 'HelloRequest': return new HHelloRequest(v.token)
    case 'RegisterProjectRequest': return new HRegisterProjectRequest(v.project_id, v.endpoint, v.token, v.title, v.path)
    case 'RegisterFolderRequest': return new HRegisterFolderRequest(v.project_id, v.resource_id, v.folder, v.title)
    case 'SkillsListRequest': return new HSkillsListRequest(v.project_path, v.folders, v.conversation_id)
    case 'ModelsListRequest': return new HModelsListRequest(v.provider_id, v.refresh, v.profile, v.account_profile)
    case 'HostProjectInfoRequest': return new HHostProjectInfoRequest(v.path)
    case 'HostProjectCreateRequest': return new HHostProjectCreateRequest(v.parent, v.name, v.flow)
    case 'HostProjectDuplicateRequest': return new HHostProjectDuplicateRequest(v.path, v.parent, v.name)
    case 'HostDemoListRequest': return new HHostDemoListRequest(v.language)
    case 'HostDemoCopyRequest': return new HHostDemoCopyRequest(v.id, v.parent, v.language)
    case 'HostRecentRememberRequest': return new HHostRecentRememberRequest(v.path)
    case 'HostRecentForgetRequest': return new HHostRecentForgetRequest(v.paths)
    case 'HostHomeRequest': return new H.HostHomeRequestT()
    case 'HostRevealRequest': return new H.HostRevealRequestT(v.path)
    case 'HostFolderListRequest': return new H.HostFolderListRequestT(v.path)
    case 'HostFolderCreateRequest': return new H.HostFolderCreateRequestT(v.parent, v.name)
    case 'HostActivityRequest': return new H.HostActivityRequestT()
    case 'HostPageRequest': return new H.HostPageRequestT(v.project, !!v.detached)
  }
  else switch (method) {
    case 'CallIdRequest': return new CallIdRequestT(v.conversation_id)
    case 'ToolCatalogRequest': return new AToolCatalogRequest()
    case 'HelloRequest': return new AHelloRequest(v.token)
    case 'ConfigureRequest': return new AConfigureRequest(provider(v.provider), v.conversation_id)
    case 'ConversationCreateRequest': return new AConversationCreateRequest(v.conversation_id, v.project_id, v.role)
    case 'ConversationListRequest': return new AConversationListRequest(v.project_id)
    case 'ConversationGetRequest': return new AConversationGetRequest(v.conversation_id)
    case 'ConversationDeleteRequest': return new AConversationDeleteRequest(v.conversation_id)
    case 'ConversationSendRequest': return new AConversationSendRequest(v.conversation_id, v.turn_id, v.text, v.selected_cards, v.selected_displays, v.skill_names, v.intent, v.attached_resources, v.display, v.scoped_skill)
    case 'ConversationActionRequest': return new AConversationActionRequest(v.conversation_id, enumValue(Action, v.action))
    case 'ConversationCancelRequest': return new AConversationCancelRequest(v.conversation_id)
    case 'ConversationRunWakeRequest': return new AConversationRunWakeRequest(v.conversation_id, !!v.run_wake)
    case 'EventsRequest': return new AEventsRequest(v.conversation_id, BigInt(v.after_seq ?? 0), v.limit)
    case 'ToolCallRequest': return new AToolCallRequest(v.conversation_id, v.project_id, v.name, v.arguments_json, v.op_id)
    case 'ToolCallCancelRequest': return new AToolCallCancelRequest(BigInt(v.request_id))
    case 'DecisionResolveRequest': return new ADecisionResolveRequest(v.request_id, v.allow)
    case 'DecisionListRequest': return new ADecisionListRequest(v.conversation_id, v.request_id, v.project_id, v.pending_only ?? false, BigInt(v.cursor ?? 0))
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
    return n
  }
  if (typeof value !== 'object') return value
  const out: Record<string, any> = {}
  const names: Record<string, string> = { projectId: 'project_id', resourceId: 'resource_id', conversationId: 'conversation_id', projectPath: 'project_path',
    trustedFolder: 'trusted_folder', sourceFolder: 'source_folder', attachmentFolder: 'attachment_folder', eventSeq: 'event_seq', nextSeq: 'next_seq', callId: 'call_id', contextSummary: 'contextSummary',
    inputJson: 'input', argumentsJson: 'arguments', resultJson: 'result', inputSchemaJson: 'inputSchema',
    contentJson: 'content', providerContentJson: 'providerContent', providerApi: 'providerApi', mediaType: 'media_type', clientId: 'client_id', attemptId: 'attempt_id', valueJson: 'value_json', capabilityFile: 'capability_file' }
  for (const [key, v] of Object.entries(value)) {
    const name = key === 'clientId' && 'signedIn' in value ? 'clientId' : names[key] ?? key
    if (v === null || v === undefined) {
      if (['title', 'text', 'projectId', 'conversationId', 'state'].includes(key)) out[name] = ''
      if (key === 'summary' || key === 'resultJson' && 'callId' in value) out[name] = null
      continue
    }
    if (key === 'resultJson' && 'callId' in value) { out[name] = JSON.parse(String(v)); continue }
    if (key === 'nextCursor') { out.next_cursor = String(v); continue }
    out[name] = ['seq', 'eventSeq', 'nextSeq'].includes(key) ? v : ['contentJson', 'providerContentJson', 'inputSchemaJson'].includes(key) ? JSON.parse(String(v)) : view(v)
  }
  return out
}
export function decodeReply(bytes: Uint8Array, host: boolean): { id: bigint; value: any; error?: CoreHostError; event?: { type: string; value: any } } {
  const buffer = new fb.ByteBuffer(bytes)
  const message = host ? HostMessage.getRootAsMessage(buffer).unpack() : AgentMessage.getRootAsMessage(buffer).unpack()
  if (host && message.payloadType === HostPayload.Event && message.requestId === 0n && message.payload) {
    const event = message.payload as any
    return { id: 0n, value: undefined, event: { type: HostEventPayload[event.valueType], value: view(event.value) } }
  }
  const reply = message.payload as any
  if (reply.valueType === (host ? H.ReplyPayload.Failure : AgentReplyPayload.Failure)) {
    const failure = reply.value
    const lock = failure.code === 'busy' ? failure.conversationLock : undefined
    const text = lock
      ? lock.application && lock.pid > 1
        ? L('{0}（PID {1}）がこのプロジェクトの会話を開いています。そのプロセスを終了してから再読込してください。', lock.application, lock.pid)
        : L('別の Tsugumi のホストサービスがこのプロジェクトの会話を開いています。そのホストサービスを終了してから再読込してください。')
      : failure.message
    return { id: message.requestId, value: undefined,
      error: new CoreHostError(failure.code, text, failure.requestId ?? undefined) }
  }
  let value = view(reply.value)
  if (host) {
    switch (reply.valueType) {
      case H.ReplyPayload.RecentList: value = value.projects; break
      case H.ReplyPayload.DemoList: value = value.demos; break
      case H.ReplyPayload.ModelList: value = value.models; break
      case H.ReplyPayload.DemoPaths: value = value.paths; break
    }
  } else {
    switch (reply.valueType) {
      case AgentReplyPayload.ConversationList: value = value.conversations; break
      case AgentReplyPayload.ToolCatalog: value = value.tools; break
    }
  }
  return { id: message.requestId, value }
}
