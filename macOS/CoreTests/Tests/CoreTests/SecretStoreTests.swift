import Foundation
import Security
import Testing
@testable import TsugumiAgentClient

@Test func secretStoreDefaultsRefuseSystemOperations() throws {
  #expect(ProcessInfo.processInfo.environment["TSUGUMI_TEST_NO_SECRET_STORE"] == "1")
  #expect(ProcessInfo.processInfo.environment["TSUGUMI_TEST_RUN_ROOT"] != nil)
  var systemCalls: [String] = []
  var refused: [String] = []
  // Use the default system boundary with a recording/refusing OS double. Even
  // a broken guard cannot reach Security in this regression test.
  let denied = KeychainStore { operation, _ in
    refused.append(operation)
    return runSystemKeychain(operation) {
      systemCalls.append(operation)
      return errSecInteractionNotAllowed
    }
  }
  #expect(!denied.exists(account: "fixture"))
  #expect(denied.load(account: "fixture") == nil)
  #expect(denied.loadWithoutAsking(account: "fixture") == nil)
  denied.delete(account: "fixture")
  #expect(throws: (any Error).self) { try denied.save("FAKE_KEY", account: "fixture") }
  #expect(refused == ["SecItemCopyMatching(attributes)", "SecItemCopyMatching(data)",
                     "SecItemCopyMatching(without asking)", "SecItemDelete", "SecItemUpdate"])
  #expect(systemCalls.isEmpty)
  print("Secret store guard: recorded system operations \(systemCalls)")

  // Exercise all adapter methods with an explicitly injected fake. A fake
  // never invokes the supplied OS operation, even for an account that exists.
  var fakeCalls: [String] = []
  let store = KeychainStore { operation, _ in
    fakeCalls.append(operation)
    return operation == "SecItemUpdate" ? errSecItemNotFound : errSecInteractionNotAllowed
  }
  #expect(!store.exists(account: "fixture"))
  #expect(store.load(account: "fixture") == nil)
  #expect(store.loadWithoutAsking(account: "fixture") == nil)
  store.delete(account: "fixture")
  #expect(throws: (any Error).self) { try store.save("FAKE_KEY", account: "fixture") }
  #expect(fakeCalls == ["SecItemCopyMatching(attributes)", "SecItemCopyMatching(data)",
                       "SecItemCopyMatching(without asking)", "SecItemDelete", "SecItemUpdate", "SecItemAdd"])

}
