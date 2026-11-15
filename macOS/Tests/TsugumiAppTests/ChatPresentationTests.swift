import Foundation
import Testing
@testable import TsugumiApp

@MainActor struct ChatPresentationTests {
  @Test func accessNamesTheProjectFolderCapabilityAndLifetime() throws {
    let value = ["title": "Collatz", "path": "/work/copy-b", "access": "Read", "scope": "Conversation", "reason": "Read project content"]
    let data = try JSONSerialization.data(withJSONObject: value)
    let text = ProjectAccessLabel.text("project_access " + String(decoding: data, as: UTF8.self))
    #expect(text.contains("Collatz") && text.contains("/work/copy-b"))
    #expect(text.contains(String(localized: "内容の読み取り")))
    #expect(text.contains(String(localized: "この会話の間")))
    #expect(!text.contains("[Conversation]") && !text.contains("Read project content"))
  }

  @Test func scopedSkillPromptNamesOnlyTheDeclaredCardSets() throws {
    let task: [String: Any] = ["name": "review", "skill_path": "builtin:review", "read_cards": ["outer/a", "b"], "write_cards": [String](), "run_cards": [String]()]
    let data = try JSONSerialization.data(withJSONObject: ["task": task])
    let text = ProjectAccessLabel.text("project_access helper_scope\n" + String(decoding: data, as: UTF8.self))
    #expect(text.contains("$review") && text.contains("read: outer/a, b"))
    #expect(text.contains("write: ") && text.contains("run: "))
    #expect(!text.contains("この会話の間"))
  }

  @Test func accessChoicesDefaultToNeededAndAllowAnIndependentSubset() throws {
    func item(_ access: String, needed: Bool) throws -> String {
      let data = try JSONSerialization.data(withJSONObject: ["access": access, "needed_now": needed])
      return "project_access " + String(decoding: data, as: UTF8.self)
    }
    let read = try item("Read", needed: true)
    let write = try item("Write", needed: false)
    let run = try item("Run", needed: false)
    var answer: Set<String> = []
    let prompt = ChatController.PermissionPrompt(raw: [read, write, run]) { answer = $0 }
    #expect(prompt.defaultSelection == [read])
    var queue = ChatController.PermissionQueue()
    queue.append(prompt)
    queue.answer(prompt.id, allow: true, selected: [read, write])
    #expect(answer == [read, write])
    queue.append(prompt)
    queue.answer(prompt.id, allow: false, selected: [read, write, run])
    #expect(answer.isEmpty)
  }

  @Test func everyOfferedToolHasAHumanLabel() throws {
    let data = try Data(contentsOf: SyncTests.root.appendingPathComponent("agent/Shared/agent.json"))
    let tools = try #require(JSONSerialization.jsonObject(with: data) as? [String: Any])
    let requests = (tools["requests"] as? [String] ?? []) + (tools["approvalRequests"] as? [String] ?? [])
    let names = requests.map { request in
      request.dropLast("Request".count).enumerated().map { index, c in
        (c.isUppercase && index > 0 ? "_" : "") + c.lowercased()
      }.joined()
    } + ["demo_list", "demo_read", "guide_show", "delegate", "web_search", "operation_approval"]
    for name in names {
      let step = ToolStep(name: name, args: "{}")
      #expect(!step.running.contains(name), "Missing running label: \(name)")
      #expect(!step.done.contains(name), "Missing completed label: \(name)")
    }
  }
}

@MainActor struct PermissionPresentationTests {
  @Test func unknownAndExecutionServicePermissionsAreVisibleAndNeedAnExplicitChoice() throws {
    let execution = "execution_service a remote-west", future = "future_permission b exact target"
    let request = try #require(CardPermission.Request(execution))
    #expect(request.kind.rawValue == "execution_service")
    #expect(request.card == "a" && request.folder == "remote-west")
    let unknown = try #require(CardPermission.Request(future))
    #expect(unknown.kind.rawValue == "future_permission" && unknown.folder == "exact target")
    #expect(unknown.kind.label == "future_permission")
    let prompt = ChatController.PermissionPrompt(raw: [execution, future]) { _ in }
    #expect(!prompt.defaultSelection.contains(future))
    var approved: Set<String> = []
    let queued = ChatController.PermissionPrompt(raw: [execution, future]) { approved = $0 }
    var queue = ChatController.PermissionQueue()
    queue.append(queued)
    queue.answer(queued.id, allow: true, selected: [execution])
    #expect(approved == [execution])
    queue.append(queued)
    queue.answer(queued.id, allow: true, selected: [future])
    #expect(approved == [future])
  }
}
