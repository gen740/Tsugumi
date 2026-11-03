import Foundation
import Testing
@testable import TsugumiLauncher

/// The folders a project's Cards name are found in its definitions (and its
/// externals'), writable only when a Card writes there.
@Test func hostFoldersAreReadFromTheCards() throws {
  let fm = FileManager.default
  let root = fm.temporaryDirectory.appendingPathComponent("hf-\(UUID().uuidString)")
  let data = root.appendingPathComponent("data")
  try fm.createDirectory(at: data, withIntermediateDirectories: true)
  func card(_ rel: String, _ json: String) throws {
    let f = root.appendingPathComponent("p/" + rel)
    try fm.createDirectory(at: f.deletingLastPathComponent(), withIntermediateDirectories: true)
    try Data(json.utf8).write(to: f)
  }
  try card("definitions/a/_card.json", #"{"hostFolders":[{"path":"\#(data.path)","name":"d"}]}"#)
  try card("externals/x/definitions/b/_card.json", #"{"hostFolders":[{"path":"\#(data.path)","name":"d","write":true}]}"#)
  try card("definitions/c/_card.json", #"{"hostFolders":[{"path":"/no/such/folder","name":"n"}]}"#)
  let wanted = HostFolders.wanted(project: root.appendingPathComponent("p"), extra: [])
  #expect(wanted == [HostFolders.Folder(path: data.path, write: true)])
  #expect(HostFolders.tag("/a").count == 16)
  try? fm.removeItem(at: root)
}
