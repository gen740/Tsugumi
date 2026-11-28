import AppKit
import Foundation
import SwiftUI
import Testing
import TsugumiRPC
@testable import TsugumiApp

@MainActor struct ChatPresentationTests {
  @Test func operationApprovalSaysWhatHappensAndFoldsTheRequest() throws {
    let targets = try JSONSerialization.data(withJSONObject: [
      "project": "p", "arguments_digest": "DIGEST",
      "permissions": ["nix_source heasoft github:gen740/heasoft", "nix_source nixpkgs github:NixOS/nixpkgs/abc123", "network fetch"],
    ] as [String: Any])
    let raw = "operation_approval\nproject_edit\n" + String(decoding: targets, as: UTF8.self)
    let prompt = ChatController.PermissionPrompt(raw: [raw]) { _ in }
    let operation = try #require(prompt.operation)
    #expect(operation.name == "project_edit")
    let summary = operation.summary
    #expect(summary.count == 2)
    #expect(summary[0].contains("heasoft (github:gen740/heasoft), nixpkgs (github:NixOS/nixpkgs/abc123)"))
    #expect(summary[1].hasSuffix(": fetch"))
    #expect(!summary.joined().contains("DIGEST"))
    #expect(operation.detail.contains("\"arguments_digest\" : \"DIGEST\""))

    let lock = try #require(OperationApproval(["operation_approval\nnix_path_lock_update\n{\"revision\":3}\nProposed arguments: {\"name\":\"heasoft\"}"]))
    #expect(lock.summary.count == 1 && lock.summary[0].contains("heasoft"))
    #expect(lock.detail.contains("Proposed arguments: {"))
  }

  @Test func tableCellsWrapAtTheirCapInsideAHorizontalScroll() {
    let long = String(repeating: "word ", count: 80)
    let view = NSHostingView(rootView: ScrollView(.horizontal) { CappedWidth(width: 120) { Text(long) } }.frame(width: 400))
    let cell = NSHostingView(rootView: CappedWidth(width: 120) { Text(long) })
    #expect(cell.fittingSize.width <= 120)
    #expect(cell.fittingSize.height > 40)
    #expect(view.fittingSize.width <= 400)
  }

  @Test func aDecisionIsAnsweredOnceAndDeclinedOnRelease() throws {
    let raw = "operation_approval\nversion_init\n{}"
    var answer: Set<String> = ["unanswered"]
    let prompt = ChatController.PermissionPrompt(raw: [raw]) { answer = $0 }
    #expect(prompt.defaultSelection == [raw])
    var queue = ChatController.PermissionQueue()
    queue.append(prompt)
    queue.answer(prompt.id, allow: true)
    #expect(answer == [raw])
    queue.append(prompt)
    queue.release()
    #expect(answer.isEmpty)
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
