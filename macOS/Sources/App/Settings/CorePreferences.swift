import Foundation
import Observation
import SwiftUI
import TsugumiAgentClient

/// A UI snapshot of agentd's settings. Revisions are applied by the service;
/// failed edits are shown and the snapshot is read again, never blindly retried.
@MainActor @Observable
final class CorePreferences {
  static let shared = CorePreferences()
  private var values: [String: Any] = [:]
  private var revision: UInt64 = 0
  private var editGeneration = 0
  private(set) var error: String?
  @ObservationIgnored private var pending: Task<Void, Never>?
  init() {
    Task { [weak self] in
      _ = await CoreAgentClient.shared.observeHost { [weak self] event in
        guard event["type"] as? String == "SettingsChanged", event["profile"] as? String == "desktop" else { return }
        Task { @MainActor [weak self] in await self?.reload() }
      }
      await self?.reload()
    }
  }
  func reload() async {
    await pending?.value
    await loadSnapshot()
  }
  private func loadSnapshot() async {
    do {
      let value = try await CoreAgentClient.shared.call("SettingsGetRequest", ["profile": "desktop"])
      revision = (value["revision"] as? NSNumber)?.uint64Value ?? 0
      values = Dictionary(uniqueKeysWithValues: (value["values"] as? [[String: String]] ?? []).compactMap { item in
        guard let key = item["key"], let raw = item["value_json"], let data = raw.data(using: .utf8),
          let decoded = try? JSONSerialization.jsonObject(with: data, options: .fragmentsAllowed) else { return nil }
        return (key, decoded)
      })
      AppPreferences.shared.syncHostSettings()
    } catch { self.error = String(describing: error) }
  }
  func string(forKey key: String) -> String? { values[key] as? String }
  func bool(forKey key: String) -> Bool { values[key] as? Bool ?? false }
  func integer(forKey key: String) -> Int { values[key] as? Int ?? 0 }
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
        _ = try await CoreAgentClient.shared.call("SettingsUpdateRequest", ["profile": "desktop", "revision": NSNumber(value: revision),
          "changes": [["key": key, "value_json": raw]]])
        error = nil
      } catch { editGeneration += 1; self.error = String(describing: error) }
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
