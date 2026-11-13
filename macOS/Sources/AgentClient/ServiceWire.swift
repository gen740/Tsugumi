import FlatBuffers
import Foundation
import TsugumiRPC

/// Generated FlatBuffers objects are adapted to the application's presentation
/// values. Only the model's open-ended content blocks are decoded from JSON.
enum ServiceWire {
  static func hostMethod(_ method: String, _ fields: [String: Any]) -> Bool {
    if method == "ConfigureRequest" { return fields["conversation_id"] == nil }
    return hostMethods.contains(method)
  }
  private static let hostMethods: Set<String> = [
"ExecutionServicesRequest", "ExecutionServicePutRequest", "ExecutionServiceRemoveRequest", "ExecutionProjectAuthorizeRequest", "ExecutionProjectRevokeRequest", "RegisterProjectRequest", "RegisterFolderRequest", "SkillsListRequest", "ModelsListRequest", "CredentialImportRequest", "CredentialTokenRequest", "CredentialForgetRequest", "HostProjectDefaultsRequest", "HostProjectInfoRequest", "HostProjectCreateRequest", "HostProjectDuplicateRequest", "HostDemoListRequest", "HostDemoCopyRequest", "HostRecentListRequest", "HostRecentRememberRequest", "HostRecentForgetRequest"]
  private static func registration(_ fields: [String: Any]) -> Tsugumi_Host_CredentialRegistrationT {
    let value = Tsugumi_Host_CredentialRegistrationT()
    value.clientId = fields["client_id"] as? String ?? ""
    value.accessToken = fields["access_token"] as? String
    value.refreshToken = fields["refresh_token"] as? String ?? ""
    value.tokenEndpoint = fields["token_endpoint"] as? String ?? ""
    value.revocationEndpoint = fields["revocation_endpoint"] as? String
    value.resource = fields["resource"] as? String
    value.expiresAtMs = (fields["expires_at_ms"] as? NSNumber)?.uint64Value ?? 0
    value.idToken = fields["id_token"] as? String
    value.scope = fields["scope"] as? String
    return value
  }
  static func request(_ method: String, _ fields: [String: Any], host: Bool) throws -> (UInt8, NativeObject) {
    if host {
      switch method {
      case "ExecutionServicesRequest": return (Tsugumi_Host_RequestPayload.executionservicesrequest.rawValue, Tsugumi_Host_ExecutionServicesRequestT())
      case "ExecutionServicePutRequest":
        let v = Tsugumi_Host_ExecutionServicePutRequestT()
        v.id = fields["id"] as? String ?? ""
        v.name = fields["name"] as? String ?? ""
        v.endpoint = fields["endpoint"] as? String ?? ""
        v.workerPublicKey = fields["workerPublicKey"] as? String ?? ""
        v.verify = fields["verify"] as? Bool ?? false
        return (Tsugumi_Host_RequestPayload.executionserviceputrequest.rawValue, v)
      case "ExecutionServiceRemoveRequest":
        let v = Tsugumi_Host_ExecutionServiceRemoveRequestT()
        v.id = fields["id"] as? String ?? ""
        return (Tsugumi_Host_RequestPayload.executionserviceremoverequest.rawValue, v)
      case "ExecutionProjectAuthorizeRequest":
        let v = Tsugumi_Host_ExecutionProjectAuthorizeRequestT()
        v.projectId = fields["project_id"] as? String ?? ""
        v.path = fields["path"] as? String ?? ""
        v.controlDir = fields["control_dir"] as? String ?? ""
        v.guest = fields["guest"] as? Bool ?? false
        return (Tsugumi_Host_RequestPayload.executionprojectauthorizerequest.rawValue, v)
      case "ExecutionProjectRevokeRequest":
        let v = Tsugumi_Host_ExecutionProjectRevokeRequestT()
        v.projectId = fields["project_id"] as? String ?? ""
        return (Tsugumi_Host_RequestPayload.executionprojectrevokerequest.rawValue, v)
      case "HostRecentListRequest": return (Tsugumi_Host_RequestPayload.hostrecentlistrequest.rawValue, Tsugumi_Host_HostRecentListRequestT())
      case "HostProjectDefaultsRequest": return (Tsugumi_Host_RequestPayload.hostprojectdefaultsrequest.rawValue, Tsugumi_Host_HostProjectDefaultsRequestT())
      case "HelloRequest":
        let v = Tsugumi_Service_HelloRequestT()
        v.token = fields["token"] as? String ?? ""
        v.clientId = fields["client_id"] as? String ?? ""
        v.protocol_ = (fields["protocol"] as? NSNumber)?.uint32Value ?? 1
        return (Tsugumi_Host_RequestPayload.hellorequest.rawValue, v)
      case "ConfigureRequest":
        let v = Tsugumi_Host_ConfigureRequestT()
        guard let provider = fields["provider"] as? Tsugumi_Service_ProviderT else {
          throw CoreAgentFailure(code: "invalid_request", message: "A provider configuration is required", requestID: nil)
        }
        v.provider = provider
        return (Tsugumi_Host_RequestPayload.configurerequest.rawValue, v)
      case "RegisterProjectRequest":
        let v = Tsugumi_Host_RegisterProjectRequestT()
        v.projectId = fields["project_id"] as? String ?? ""
        v.endpoint = fields["endpoint"] as? String ?? ""
        v.token = fields["token"] as? String
        v.title = fields["title"] as? String
        v.path = fields["path"] as? String
        return (Tsugumi_Host_RequestPayload.registerprojectrequest.rawValue, v)
      case "RegisterFolderRequest":
        let v = Tsugumi_Host_RegisterFolderRequestT()
        v.projectId = fields["project_id"] as? String ?? ""
        v.resourceId = fields["resource_id"] as? String ?? ""
        v.folder = fields["folder"] as? String ?? ""
        v.title = fields["title"] as? String
        return (Tsugumi_Host_RequestPayload.registerfolderrequest.rawValue, v)
      case "SkillsListRequest":
        let v = Tsugumi_Host_SkillsListRequestT()
        v.conversationId = fields["conversation_id"] as? String
        v.projectPath = fields["project_path"] as? String
        v.folders = fields["folders"] as? [String] ?? []
        return (Tsugumi_Host_RequestPayload.skillslistrequest.rawValue, v)
      case "ModelsListRequest":
        let v = Tsugumi_Host_ModelsListRequestT()
        v.providerId = fields["provider_id"] as? String ?? ""
        v.refresh = fields["refresh"] as? Bool ?? false
        v.provider = fields["provider"] as? Tsugumi_Service_ProviderT
        return (Tsugumi_Host_RequestPayload.modelslistrequest.rawValue, v)
      case "CredentialImportRequest":
        let v = Tsugumi_Host_CredentialImportRequestT()
        v.registration = registration(fields["registration"] as? [String: Any] ?? [:])
        v.replace = fields["replace"] as? Bool ?? false
        return (Tsugumi_Host_RequestPayload.credentialimportrequest.rawValue, v)
      case "CredentialTokenRequest":
        let v = Tsugumi_Host_CredentialTokenRequestT()
        v.clientId = fields["client_id"] as? String ?? ""
        v.staleToken = fields["stale_token"] as? String
        return (Tsugumi_Host_RequestPayload.credentialtokenrequest.rawValue, v)
      case "CredentialForgetRequest":
        let v = Tsugumi_Host_CredentialForgetRequestT()
        v.clientId = fields["client_id"] as? String ?? ""
        return (Tsugumi_Host_RequestPayload.credentialforgetrequest.rawValue, v)
      case "HostProjectInfoRequest":
        let v = Tsugumi_Host_HostProjectInfoRequestT()
        v.path = fields["path"] as? String ?? ""
        return (Tsugumi_Host_RequestPayload.hostprojectinforequest.rawValue, v)
      case "HostProjectCreateRequest":
        let v = Tsugumi_Host_HostProjectCreateRequestT()
        v.parent = fields["parent"] as? String ?? ""
        v.name = fields["name"] as? String ?? ""
        v.flow = fields["flow"] as? String
        return (Tsugumi_Host_RequestPayload.hostprojectcreaterequest.rawValue, v)
      case "HostProjectDuplicateRequest":
        let v = Tsugumi_Host_HostProjectDuplicateRequestT()
        v.path = fields["path"] as? String ?? ""
        v.parent = fields["parent"] as? String ?? ""
        v.name = fields["name"] as? String ?? ""
        return (Tsugumi_Host_RequestPayload.hostprojectduplicaterequest.rawValue, v)
      case "HostDemoListRequest":
        let v = Tsugumi_Host_HostDemoListRequestT()
        v.language = fields["language"] as? String
        return (Tsugumi_Host_RequestPayload.hostdemolistrequest.rawValue, v)
      case "HostDemoCopyRequest":
        let v = Tsugumi_Host_HostDemoCopyRequestT()
        v.id = fields["id"] as? String ?? ""
        v.parent = fields["parent"] as? String ?? ""
        v.language = fields["language"] as? String
        return (Tsugumi_Host_RequestPayload.hostdemocopyrequest.rawValue, v)
      case "HostRecentRememberRequest":
        let v = Tsugumi_Host_HostRecentRememberRequestT()
        v.path = fields["path"] as? String ?? ""
        return (Tsugumi_Host_RequestPayload.hostrecentrememberrequest.rawValue, v)
      case "HostRecentForgetRequest":
        let v = Tsugumi_Host_HostRecentForgetRequestT()
        v.paths = fields["paths"] as? [String] ?? []
        return (Tsugumi_Host_RequestPayload.hostrecentforgetrequest.rawValue, v)
      default: throw CoreAgentFailure(code: "protocol", message: "Unknown Host request: \(method)", requestID: nil)
      }
    } else {
      switch method {
      case "DemoListRequest": return (Tsugumi_Agent_RequestPayload.demolistrequest.rawValue, Tsugumi_Agent_DemoListRequestT())
      case "ToolCatalogRequest": return (Tsugumi_Agent_RequestPayload.toolcatalogrequest.rawValue, Tsugumi_Agent_ToolCatalogRequestT())
      case "HelloRequest":
        let v = Tsugumi_Service_HelloRequestT()
        v.token = fields["token"] as? String ?? ""
        v.clientId = fields["client_id"] as? String ?? ""
        v.protocol_ = (fields["protocol"] as? NSNumber)?.uint32Value ?? 1
        return (Tsugumi_Agent_RequestPayload.hellorequest.rawValue, v)
      case "ConfigureRequest":
        let v = Tsugumi_Agent_ConfigureRequestT()
        guard let provider = fields["provider"] as? Tsugumi_Service_ProviderT else {
          throw CoreAgentFailure(code: "invalid_request", message: "A provider configuration is required", requestID: nil)
        }
        v.provider = provider
        v.conversationId = fields["conversation_id"] as? String ?? ""
        return (Tsugumi_Agent_RequestPayload.configurerequest.rawValue, v)
      case "ConversationCreateRequest":
        let v = Tsugumi_Agent_ConversationCreateRequestT()
        v.conversationId = fields["conversation_id"] as? String
        v.projectId = fields["project_id"] as? String
        v.role = fields["role"] as? String
        return (Tsugumi_Agent_RequestPayload.conversationcreaterequest.rawValue, v)
      case "ConversationListRequest":
        let v = Tsugumi_Agent_ConversationListRequestT()
        v.projectId = fields["project_id"] as? String
        return (Tsugumi_Agent_RequestPayload.conversationlistrequest.rawValue, v)
      case "ConversationGetRequest":
        let v = Tsugumi_Agent_ConversationGetRequestT()
        v.conversationId = fields["conversation_id"] as? String ?? ""
        return (Tsugumi_Agent_RequestPayload.conversationgetrequest.rawValue, v)
      case "ConversationDeleteRequest":
        let v = Tsugumi_Agent_ConversationDeleteRequestT()
        v.conversationId = fields["conversation_id"] as? String ?? ""
        return (Tsugumi_Agent_RequestPayload.conversationdeleterequest.rawValue, v)
      case "ConversationSearchRequest":
        let v = Tsugumi_Agent_ConversationSearchRequestT()
        v.projectId = fields["project_id"] as? String
        v.text = fields["text"] as? String ?? ""
        return (Tsugumi_Agent_RequestPayload.conversationsearchrequest.rawValue, v)
      case "ConversationSendRequest":
        let v = Tsugumi_Agent_ConversationSendRequestT()
        v.conversationId = fields["conversation_id"] as? String ?? ""
        v.turnId = fields["turn_id"] as? String ?? ""
        v.text = fields["text"] as? String ?? ""
        v.selectedCards = fields["selected_cards"] as? [String] ?? []
        v.selectedDisplays = fields["selected_displays"] as? [String] ?? []
        v.skillNames = fields["skill_names"] as? [String] ?? []
        v.intent = fields["intent"] as? String
        v.attachedResources = fields["attached_resources"] as? [String] ?? []
        return (Tsugumi_Agent_RequestPayload.conversationsendrequest.rawValue, v)
      case "ConversationActionRequest":
        let v = Tsugumi_Agent_ConversationActionRequestT()
        v.conversationId = fields["conversation_id"] as? String ?? ""
        v.action = Tsugumi_Agent_Action(rawValue: (fields["action"] as? NSNumber)?.uint8Value ?? (["Resume": UInt8(0), "Retry": UInt8(1), "Continue": UInt8(2), "Compact": UInt8(3), "Reset": UInt8(4)][fields["action"] as? String ?? ""] ?? 0)) ?? .resume
        return (Tsugumi_Agent_RequestPayload.conversationactionrequest.rawValue, v)
      case "ConversationCancelRequest":
        let v = Tsugumi_Agent_ConversationCancelRequestT()
        v.conversationId = fields["conversation_id"] as? String ?? ""
        return (Tsugumi_Agent_RequestPayload.conversationcancelrequest.rawValue, v)
      case "EventsRequest":
        let v = Tsugumi_Agent_EventsRequestT()
        v.conversationId = fields["conversation_id"] as? String ?? ""
        v.afterSeq = (fields["after_seq"] as? NSNumber)?.uint64Value ?? 0
        v.limit = (fields["limit"] as? NSNumber)?.uint32Value ?? 200
        return (Tsugumi_Agent_RequestPayload.eventsrequest.rawValue, v)
      case "ToolCallRequest":
        let v = Tsugumi_Agent_ToolCallRequestT()
        v.conversationId = fields["conversation_id"] as? String ?? ""
        v.projectId = fields["project_id"] as? String
        v.name = fields["name"] as? String ?? ""
        v.argumentsJson = fields["arguments_json"] as? String ?? ""
        v.opId = fields["op_id"] as? String ?? ""
        return (Tsugumi_Agent_RequestPayload.toolcallrequest.rawValue, v)
      case "AccessRequest":
        let v = Tsugumi_Agent_AccessRequestT()
        v.projectId = fields["project_id"] as? String ?? ""
        v.conversationId = fields["conversation_id"] as? String
        v.access = Tsugumi_Agent_Access(rawValue: (fields["access"] as? NSNumber)?.uint8Value ?? (["Read": UInt8(0), "Write": UInt8(1), "Run": UInt8(2), "CardPermissions": UInt8(3), "Shell": UInt8(4)][fields["access"] as? String ?? ""] ?? 0)) ?? .read
        v.scope = Tsugumi_Agent_GrantScope(rawValue: (fields["scope"] as? NSNumber)?.uint8Value ?? (["Conversation": UInt8(0), "Server": UInt8(1)][fields["scope"] as? String ?? ""] ?? 0)) ?? .conversation
        v.reason = fields["reason"] as? String
        return (Tsugumi_Agent_RequestPayload.accessrequest.rawValue, v)
      case "AccessResolveRequest":
        let v = Tsugumi_Agent_AccessResolveRequestT()
        v.requestId = fields["request_id"] as? String ?? ""
        v.allow = fields["allow"] as? Bool ?? false
        return (Tsugumi_Agent_RequestPayload.accessresolverequest.rawValue, v)
      case "AccessRevokeRequest":
        let v = Tsugumi_Agent_AccessRevokeRequestT()
        v.grantId = fields["grant_id"] as? String ?? ""
        return (Tsugumi_Agent_RequestPayload.accessrevokerequest.rawValue, v)
      case "AccessListRequest":
        let v = Tsugumi_Agent_AccessListRequestT()
        v.conversationId = fields["conversation_id"] as? String
        return (Tsugumi_Agent_RequestPayload.accesslistrequest.rawValue, v)
      case "DemoReadRequest":
        let v = Tsugumi_Agent_DemoReadRequestT()
        v.path = fields["path"] as? String ?? ""
        return (Tsugumi_Agent_RequestPayload.demoreadrequest.rawValue, v)
      case "VersionSummarizeRequest":
        let v = Tsugumi_Agent_VersionSummarizeRequestT()
        v.projectId = fields["project_id"] as? String ?? ""
        v.commit = fields["commit"] as? String ?? ""
        return (Tsugumi_Agent_RequestPayload.versionsummarizerequest.rawValue, v)
      case "ConversationSummarizeRequest":
        let v = Tsugumi_Agent_ConversationSummarizeRequestT()
        v.conversationId = fields["conversation_id"] as? String ?? ""
        guard let provider = fields["provider"] as? Tsugumi_Service_ProviderT else {
          throw CoreAgentFailure(code: "invalid_request", message: "A provider configuration is required", requestID: nil)
        }
        v.provider = provider
        return (Tsugumi_Agent_RequestPayload.conversationsummarizerequest.rawValue, v)
      default: throw CoreAgentFailure(code: "protocol", message: "Unknown Agent request: \(method)", requestID: nil)
      }
    }
  }

  static func encode(_ method: String, fields: [String: Any], id: UInt64, host: Bool) throws -> Data {
    let (tag, value) = try request(method, fields, host: host)
    var b = FlatBufferBuilder(initialSize: 1024)
    if host {
      let request = Tsugumi_Host_RequestT()
      request.value = Tsugumi_Host_RequestPayloadUnion(value, type: Tsugumi_Host_RequestPayload(rawValue: tag)!)
      let message = Tsugumi_Host_MessageT()
      message.requestId = id
      message.payload = Tsugumi_Host_PayloadUnion(request, type: .request)
      var object: Tsugumi_Host_MessageT? = message
      let offset = Tsugumi_Host_Message.pack(&b, obj: &object)
      Tsugumi_Host_Message.finish(&b, end: offset)
    } else {
      let request = Tsugumi_Agent_RequestT()
      request.value = Tsugumi_Agent_RequestPayloadUnion(value, type: Tsugumi_Agent_RequestPayload(rawValue: tag)!)
      let message = Tsugumi_Agent_MessageT()
      message.requestId = id
      message.payload = Tsugumi_Agent_PayloadUnion(request, type: .request)
      var object: Tsugumi_Agent_MessageT? = message
      let offset = Tsugumi_Agent_Message.pack(&b, obj: &object)
      Tsugumi_Agent_Message.finish(&b, end: offset)
    }
    return b.data
  }

  static func decode(_ data: Data, host: Bool) throws -> [String: Any] {
    var buffer = ByteBuffer(data: data)
    let value: NativeObject?
    if host {
      let root: Tsugumi_Host_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSH1")
      value = (Tsugumi_Host_MessageT(root).payload?.value as? Tsugumi_Host_ReplyT)?.value?.value
    } else {
      let root: Tsugumi_Agent_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSA1")
      value = (Tsugumi_Agent_MessageT(root).payload?.value as? Tsugumi_Agent_ReplyT)?.value?.value
    }
    guard let value else { throw CoreAgentFailure(code: "protocol", message: "Missing service reply", requestID: nil) }
    if let failure = value as? Tsugumi_Service_FailureT {
      throw CoreAgentFailure(code: failure.code, message: failure.message, requestID: failure.requestId)
    }
    // Generated native objects normalize omitted Host vectors to empty arrays.
    let result = try view(value)
    return result as? [String: Any] ?? ["value": result]
  }

  static func conversation(_ v: Tsugumi_Agent_ConversationT) throws -> Conversation {
    func messages(_ values: [Tsugumi_Agent_ModelMessageT?]) throws -> [ChatMessage] {
      try values.compactMap { v in
        guard let v else { return nil }
        let blocks = try JSONDecoder().decode([ContentBlock].self, from: Data((v.contentJson ?? "[]").utf8))
        return ChatMessage(role: v.role ?? "user", content: blocks, display: v.display)
      }
    }
    var result = Conversation()
    result.id = v.id
    result.title = v.title ?? ""
    result.updatedAt = Date(timeIntervalSince1970: Double(v.updatedAt) / 1000)
    result.messages = try messages(v.messages)
    result.archive = try messages(v.archive)
    result.summary = v.summary
    result.tools = v.tools.compactMap { v in
      guard let v else { return nil }
      return ToolRecord(id: v.id ?? "", name: v.name ?? "", arguments: v.argumentsJson ?? "{}", opId: v.opId,
        state: ToolRecord.State(rawValue: v.state ?? "") ?? .pending, result: v.resultJson, isError: v.isError)
    }
    if let counts = v.usage {
      result.usage.inputTokens = Int(clamping: counts.inputTokens)
      result.usage.outputTokens = Int(clamping: counts.outputTokens)
      result.usage.cacheReadTokens = Int(clamping: counts.cacheReadTokens)
      result.usage.cacheWriteTokens = Int(clamping: counts.cacheWriteTokens)
    }
    return result
  }

  private static func view(_ value: NativeObject) throws -> Any {
    switch value {
    case let v as Tsugumi_Host_ExecutionServicesT:
      return ["services": try v.services.compactMap { try $0.map(view) }, "hostPublicKey": v.hostPublicKey] as [String: Any]
    case let v as Tsugumi_Host_ExecutionServiceT:
      return ["id": v.id, "name": v.name, "endpoint": v.endpoint, "workerPublicKey": v.workerPublicKey, "capabilities": try view(v.capabilities)] as [String: Any]
    case let v as Tsugumi_Host_ExecutionCapabilitiesT:
      return ["serverId": v.serverId, "nativeSystem": v.nativeSystem, "systems": v.systems.compactMap { $0 },
              "isolation": v.isolation ?? "pid_namespace", "resourceLimits": v.resourceLimits,
              "maxConcurrentJobs": v.maxConcurrentJobs, "workerContract": v.workerContract] as [String: Any]
    case let v as Tsugumi_Host_ExecutionAuthorizationT:
      return ["endpoint": v.endpoint, "capability_file": v.capabilityFile, "available": v.available] as [String: Any]
    case let v as Tsugumi_Service_HelloResultT:
      var out: [String: Any] = [:]
      out["protocol"] = v.protocol_
      out["server_id"] = v.serverId
      out["principal"] = v.principal
      return out
    case let v as Tsugumi_Service_FailureT:
      var out: [String: Any] = [:]
      out["code"] = v.code
      out["message"] = v.message
      out["request_id"] = v.requestId
      return out
    case let v as Tsugumi_Service_AcknowledgementT:
      var out: [String: Any] = [:]
      out["accepted"] = v.accepted
      out["revoked"] = v.revoked
      return out
    case let v as Tsugumi_Agent_UsageT:
      var out: [String: Any] = [:]
      out["inputTokens"] = v.inputTokens
      out["outputTokens"] = v.outputTokens
      out["cacheReadTokens"] = v.cacheReadTokens
      out["cacheWriteTokens"] = v.cacheWriteTokens
      return out
    case let v as Tsugumi_Agent_ConversationT:
      return v
    case let v as Tsugumi_Agent_ConversationResultT:
      var out: [String: Any] = [:]
      out["conversation"] = try view(v.conversation)
      out["busy"] = v.busy
      out["resumable"] = v.resumable
      out["continuable"] = v.continuable
      out["project_id"] = v.projectId
      out["event_seq"] = v.eventSeq
      return out
    case let v as Tsugumi_Agent_ConversationEntryT:
      var out: [String: Any] = [:]
      out["id"] = v.id
      out["project_id"] = v.projectId
      out["title"] = v.title ?? ""
      out["updatedAt"] = Double(v.updatedAt)
      out["busy"] = v.busy
      return out
    case let v as Tsugumi_Agent_ConversationListT:
      return try v.conversations.compactMap { item in try item.map { try view($0) } }
    case let v as Tsugumi_Agent_SearchMatchT:
      var out: [String: Any] = [:]
      out["session"] = v.session
      out["title"] = v.title ?? ""
      out["snippet"] = v.snippet
      out["updatedAt"] = Double(v.updatedAt)
      return out
    case let v as Tsugumi_Agent_SearchResultT:
      return try v.matches.compactMap { item in try item.map { try view($0) } }
    case let v as Tsugumi_Agent_EventT:
      var out: [String: Any] = [:]
      out["kind"] = v.kind
      out["text"] = v.text ?? ""
      out["message"] = v.message
      out["input"] = v.inputJson
      out["id"] = v.id
      out["name"] = v.name
      out["ok"] = v.ok
      out["index"] = Int(v.index)
      out["asked"] = v.asked
      out["event"] = try v.event.map { try view($0) }
      return out
    case let v as Tsugumi_Agent_SequencedEventT:
      var out: [String: Any] = [:]
      out["seq"] = v.seq
      out["event"] = try view(v.event)
      return out
    case let v as Tsugumi_Agent_EventsPageT:
      var out: [String: Any] = [:]
      out["events"] = try v.events.compactMap { try $0.map { try view($0) } }
      out["next_seq"] = v.nextSeq
      out["busy"] = v.busy
      return out
    case let v as Tsugumi_Agent_ToolT:
      var out: [String: Any] = [:]
      out["name"] = v.name
      out["description"] = v.description
      out["inputSchema"] = try JSONSerialization.jsonObject(with: Data((v.inputSchemaJson ?? "{}").utf8))
      return out
    case let v as Tsugumi_Agent_ToolCatalogT:
      return try v.tools.compactMap { item in try item.map { try view($0) } }
    case let v as Tsugumi_Agent_ImageT:
      var out: [String: Any] = [:]
      out["media_type"] = v.mediaType
      out["data"] = v.data
      return out
    case let v as Tsugumi_Agent_ToolResultT:
      var out: [String: Any] = [:]
      out["text"] = v.text ?? ""
      out["isError"] = v.isError
      out["images"] = try v.images.compactMap { try $0.map { try view($0) } }
      return out
    case let v as Tsugumi_Agent_AccessResultT:
      var out: [String: Any] = [:]
      out["id"] = v.id
      return out
    case let v as Tsugumi_Agent_AccessEntryT:
      var out: [String: Any] = [:]
      out["id"] = v.id
      out["project_id"] = v.projectId
      out["conversation_id"] = v.conversationId
      out["access"] = ["Read", "Write", "Run", "CardPermissions", "Shell"][Int(v.access.rawValue)]
      out["scope"] = ["Conversation", "Server"][Int(v.scope.rawValue)]
      out["reason"] = v.reason
      out["state"] = v.state
      return out
    case let v as Tsugumi_Agent_GrantT:
      var out: [String: Any] = [:]
      out["id"] = v.id
      out["project_id"] = v.projectId
      out["conversation_id"] = v.conversationId
      out["access"] = ["Read", "Write", "Run", "CardPermissions", "Shell"][Int(v.access.rawValue)]
      out["active"] = v.active
      return out
    case let v as Tsugumi_Agent_AccessListT:
      var out: [String: Any] = [:]
      out["requests"] = try v.requests.compactMap { try $0.map { try view($0) } }
      out["grants"] = try v.grants.compactMap { try $0.map { try view($0) } }
      return out
    case let v as Tsugumi_Agent_DemoListT:
      return v.paths
    case let v as Tsugumi_Agent_TextResultT:
      var out: [String: Any] = [:]
      out["text"] = v.text ?? ""
      out["usage"] = try v.usage.map { try view($0) }
      return out
    case let v as Tsugumi_Host_CredentialTokenT:
      var out: [String: Any] = [:]
      out["access_token"] = v.accessToken
      out["client_id"] = v.clientId
      return out
    case let v as Tsugumi_Host_ProjectDefaultsT:
      var out: [String: Any] = [:]
      out["nixpkgsPin"] = v.nixpkgsPin
      return out
    case let v as Tsugumi_Host_ProjectInfoT:
      var out: [String: Any] = [:]
      out["path"] = v.path
      out["name"] = v.name
      return out
    case let v as Tsugumi_Host_ProjectLocationT:
      var out: [String: Any] = [:]
      out["path"] = v.path
      return out
    case let v as Tsugumi_Host_RecentProjectT:
      var out: [String: Any] = [:]
      out["path"] = v.path
      out["name"] = v.name
      out["openedAt"] = v.openedAt
      return out
    case let v as Tsugumi_Host_RecentListT:
      return try v.projects.compactMap { item in try item.map { try view($0) } }
    case let v as Tsugumi_Host_DemoT:
      var out: [String: Any] = [:]
      out["id"] = v.id
      out["path"] = v.path
      out["title"] = v.title
      out["description"] = v.description
      out["tags"] = v.tags
      out["field"] = v.field
      return out
    case let v as Tsugumi_Host_DemoListT:
      return try v.demos.compactMap { item in try item.map { try view($0) } }
    case let v as Tsugumi_Host_SkillT:
      return ["name": v.name, "description": v.description ?? "", "path": v.path,
              "source_folder": v.sourceFolder ?? "", "status": v.status]
    case let v as Tsugumi_Host_SkillFolderT:
      return ["path": v.path, "status": v.status]
    case let v as Tsugumi_Host_SkillsListT:
      return ["skills": try v.skills.compactMap { try $0.map { try view($0) } },
              "folders": try v.folders.compactMap { try $0.map { try view($0) } }]
    case let v as Tsugumi_Host_ModelT:
      var out: [String: Any] = [:]
      out["id"] = v.id
      out["name"] = v.name
      return out
    case let v as Tsugumi_Host_ModelListT:
      return try v.models.compactMap { item in try item.map { try view($0) } }
    default: throw CoreAgentFailure(code: "protocol", message: "Unexpected service reply type", requestID: nil)
    }
  }
}
