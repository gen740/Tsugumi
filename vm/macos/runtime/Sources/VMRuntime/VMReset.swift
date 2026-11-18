import Foundation

/// Apply the host's file retention request without choosing any retained data.
public enum VMReset {
  public struct Report {
    public var kept: [String] = []
    public var discarded: [String] = []
  }
  public static func apply(directory: URL, keepFiles: [String], discardOtherFiles: Bool) throws -> Report {
    let files = FileManager.default
    var report = Report()
    guard files.fileExists(atPath: directory.path) else { return report }
    for name in try files.contentsOfDirectory(atPath: directory.path).sorted() {
      if keepFiles.contains(name) || !discardOtherFiles {
        report.kept.append(name)
      } else {
        try files.removeItem(at: directory.appendingPathComponent(name))
        report.discarded.append(name)
      }
    }
    return report
  }
}
