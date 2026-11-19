import Foundation
import Observation
import SwiftUI
import TsugumiAgentClient
import TsugumiRPC

/// A UI snapshot of hostd's settings. Revisions are applied by the service;
/// failed edits are shown and the snapshot is read again, never blindly retried.
@MainActor @Observable
final class CorePreferences {
  static let shared = CorePreferences()
  private var values: [String: Any] = [:]
  private var revision: String?
  private var modelValues: [String: Any] = [:]
  private(set) var summarySelection: Tsugumi_Service_ProviderSelectionT?
  private var editGeneration = 0
  private(set) var error: String?
  @ObservationIgnored private var pending: Task<Void, Never>?
  init() {
    Task { [weak self] in
      _ = await CoreHostClient.shared.observeHost { [weak self] event in
        let type = event["type"] as? String
        if type == "Connected" || type == "SettingsChanged" {
          Task { @MainActor [weak self] in
            guard let self else { return }
            if type == "Connected" {
              self.editGeneration += 1
              self.revision = nil
              self.summarySelection = nil
            }
            await self.reload()
          }
        } else if event["type"] as? String == "ProviderChanged", event["profile"] as? String == "desktop" {
          Task { @MainActor [weak self] in
            await self?.refreshProviderAvailability()
            self?.summarySelection = try? await CoreHostClient.shared.modelSelection("summary")
          }
        }
      }
      await self?.reload()
    }
  }
  func reload() async {
    await pending?.value
    await loadSnapshot()
  }
  private func loadSnapshot() async {
    let generation = editGeneration
    do {
      let value = try await CoreHostClient.shared.call("SettingsGetRequest", ["profile": "desktop"])
      guard generation == editGeneration else { return await loadSnapshot() }
      let firstSnapshot = revision == nil
      let previousFolders = stringArray(forKey: "agent.trustedFolders") ?? []
      let nextRevision = value["revision"] as? String ?? ""
      if let revision, nextRevision <= revision { return }
      revision = nextRevision
      values = Dictionary(uniqueKeysWithValues: (value["values"] as? [[String: String]] ?? []).compactMap { item in
        guard let key = item["key"], let raw = item["value_json"], let data = raw.data(using: .utf8),
          let decoded = try? JSONSerialization.jsonObject(with: data, options: .fragmentsAllowed) else { return nil }
        return (key, decoded)
      })
      let newModels = values.filter { $0.key.hasPrefix("agent.provider") || $0.key.hasPrefix("agent.model") || $0.key.hasPrefix("agent.speechModel") }
      if summarySelection == nil || !NSDictionary(dictionary: modelValues).isEqual(to: newModels) {
        summarySelection = try await CoreHostClient.shared.modelSelection("summary")
      }
      modelValues = newModels
      if firstSnapshot { await refreshProviderAvailability() }
      AppPreferences.shared.syncHostSettings()
      if previousFolders != (stringArray(forKey: "agent.trustedFolders") ?? []) {
        for session in AppModel.shared.sessions.values { try? await session.refreshProjectTrust() }
      }
    } catch { self.error = String(describing: error) }
  }
  private func refreshProviderAvailability() async {
    async let anthropic = try? CoreHostClient.shared.usable(.anthropic)
    async let openAI = try? CoreHostClient.shared.usable(.openAI)
    async let chatGPT = try? CoreHostClient.shared.usable(.chatGPT)
    _ = await (anthropic, openAI, chatGPT)
  }
  func string(forKey key: String) -> String? { values[key] as? String }
  func bool(forKey key: String) -> Bool { values[key] as? Bool ?? false }
  func stringArray(forKey key: String) -> [String]? { values[key] as? [String] }
  func data(forKey key: String) -> Data? { values[key].flatMap { try? JSONSerialization.data(withJSONObject: $0, options: .fragmentsAllowed) } }
  func value<T>(_ key: String, fallback: T) -> T { values[key] as? T ?? fallback }
  func set(_ value: Any?, forKey key: String) {
    guard let value, let data = try? JSONSerialization.data(withJSONObject: value, options: .fragmentsAllowed), let raw = String(data: data, encoding: .utf8) else { return }
    if let old = values[key], let before = try? JSONSerialization.data(withJSONObject: old, options: [.fragmentsAllowed, .sortedKeys]),
      let after = try? JSONSerialization.data(withJSONObject: value, options: [.fragmentsAllowed, .sortedKeys]), before == after { return }
    values[key] = value
    let generation = editGeneration
    let previous = pending
    pending = Task {
      await previous?.value
      guard generation == editGeneration else { return }
      do {
        _ = try await CoreHostClient.shared.call("SettingsUpdateRequest", ["profile": "desktop", "revision": revision ?? "",
          "changes": [["key": key, "value_json": raw]]])
        error = nil
      } catch {
        editGeneration += 1
        revision = nil
        self.error = String(describing: error)
      }
      await loadSnapshot()
    }
  }
}

@propertyWrapper @MainActor
struct HostStorage<Value>: DynamicProperty {
  @State private var snapshot = CorePreferences.shared
  private let key: String
  private let fallback: Value
  init(wrappedValue: Value, _ key: String) { self.key = key; fallback = wrappedValue }
  var wrappedValue: Value {
    get { snapshot.value(key, fallback: fallback) }
    nonmutating set { snapshot.set(newValue, forKey: key) }
  }
  var projectedValue: Binding<Value> { Binding(get: { wrappedValue }, set: { wrappedValue = $0 }) }
}
