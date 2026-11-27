import Foundation
import SwiftUI
import Testing
@testable import TsugumiApp
import TsugumiTestSupport

/// Where a project goes among the main windows, each with its own project.
@Test func aProjectOpensWhereItIsThenOnTheAskingHomeThenInANewWindow() {
  let home = UUID(), a = UUID(), b = UUID()
  let windows = [MainWindowRouting.Window(id: home, project: nil),
                 .init(id: a, project: "A"), .init(id: b, project: "B")]
  // Open already, from anywhere: that window comes to the front, never a second one.
  #expect(MainWindowRouting.target(project: "A", from: b, windows: windows) == .front(a))
  #expect(MainWindowRouting.target(project: "A", from: home, windows: windows) == .front(a))
  #expect(MainWindowRouting.target(project: "A", from: nil, windows: windows) == .front(a))
  #expect(MainWindowRouting.target(project: "A", from: a, windows: windows) == .front(a))
  // Not open: Home takes it, a project's window keeps its own.
  #expect(MainWindowRouting.target(project: "C", from: home, windows: windows) == .here)
  #expect(MainWindowRouting.target(project: "C", from: a, windows: windows) == .newWindow)
  // No main window in front (a Card's settings, or none at all).
  #expect(MainWindowRouting.target(project: "C", from: nil, windows: windows) == .newWindow)
  #expect(MainWindowRouting.target(project: "C", from: nil, windows: []) == .newWindow)
  // A new project: as any project that is open nowhere.
  #expect(MainWindowRouting.target(project: nil, from: home, windows: windows) == .here)
  #expect(MainWindowRouting.target(project: nil, from: b, windows: windows) == .newWindow)
}

/// The bird out in one project's window leaves the others' birds at home.
@MainActor @Test(.hostService) func theBirdIsAwayOnlyForItsOwnProject() {
  let guide = GuideController.shared
  let stage = GuideStage()
  stage.project = "/a"
  stage.away = true
  #expect(guide.birdAway("/a") && !guide.birdAway("/b") && !guide.birdAway(nil))
  // A window that takes a project on Home while the bird flies into its loading card.
  let home = GuideStage()
  home.away = true
  #expect(guide.birdAway(nil))
  home.project = "/c"
  #expect(guide.birdAway("/c") && !guide.birdAway(nil))
  home.away = false
  stage.away = false
  #expect(!guide.birdAway("/a") && !guide.birdAway("/c"))
}

/// What the guide says on Home stays on Home; no chat is speaking here.
@MainActor @Test(.hostService) func theGuideOfHomeSpeaksOnHomeOnly() {
  #expect(GuideController.shared.speaks(in: nil))
  #expect(!GuideController.shared.speaks(in: "/a"))
}

/// A new window takes what it was opened for, in order, once.
@MainActor @Test(.hostService) func aNewWindowTakesItsIntentOnce() {
  let model = AppModel.shared
  let url = URL(fileURLWithPath: "/tmp/x")
  model.pendingWindowIntents = [.open(url), .newProject(nil)]
  #expect(model.takeWindowIntent() == .open(url))
  #expect(model.takeWindowIntent() == .newProject(nil))
  #expect(model.takeWindowIntent() == nil)
}

/// ⌘N on Home opens the sheet of that window alone.
@MainActor @Test(.hostService) func aNewProjectOnHomeStaysInThatWindow() {
  let model = AppModel.shared
  let one = Workspace(), other = Workspace()
  model.newProject(from: one, openWindow: EnvironmentValues().openWindow)
  #expect(one.requestNewProject && !other.requestNewProject)
  #expect(one.newProjectFolder == nil)
  #expect(model.pendingWindowIntents.isEmpty)
  // Offered for a folder that is not a project: the save panel starts there.
  let folder = URL(fileURLWithPath: "/tmp/data")
  model.newProject(from: other, in: folder, openWindow: EnvironmentValues().openWindow)
  #expect(other.requestNewProject && other.newProjectFolder == folder)
  #expect(model.pendingWindowIntents.isEmpty)
}

/// What Finder's "Open With" (or `open -a`) hands over: a project folder or its
/// tsugumi.json stands for the project; anything else is not one.
@Test func anOpenedItemIsAProjectOnlyWithItsTsugumiJSON() throws {
  let root = testTemporaryDirectory.appendingPathComponent("open-external-\(UUID())")
  let project = root.appendingPathComponent("project"), plain = root.appendingPathComponent("plain")
  defer { try? FileManager.default.removeItem(at: root) }
  for folder in [project, plain] { try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true) }
  let document = project.appendingPathComponent("tsugumi.json"), note = plain.appendingPathComponent("notes.txt")
  try Data("{}".utf8).write(to: document)
  try Data().write(to: note)
  let path = { (url: URL?) in url?.standardizedFileURL.path }
  #expect(path(AppModel.projectFolder(project)) == path(project))
  #expect(path(AppModel.projectFolder(document)) == path(project))
  #expect(AppModel.projectFolder(plain) == nil)
  #expect(AppModel.projectFolder(note) == nil)
  #expect(AppModel.projectFolder(root.appendingPathComponent("missing")) == nil)
  // A new project is offered in the folder itself, or in a file's folder.
  #expect(path(AppModel.folderForNewProject(plain)) == path(plain))
  #expect(path(AppModel.folderForNewProject(note)) == path(plain))
}

/// A folder that is not a project is said so in the window that received it:
/// no project, no second window.
@MainActor @Test(.hostService) func aFolderThatIsNotAProjectOpensNoWindow() throws {
  let model = AppModel.shared
  let plain = testTemporaryDirectory.appendingPathComponent("not-a-project-\(UUID())")
  try FileManager.default.createDirectory(at: plain, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: plain) }
  let home = Workspace()
  model.pendingWindowIntents = []
  model.openExternal(plain, from: home, openWindow: EnvironmentValues().openWindow)
  #expect(home.notAProject == plain && home.session == nil)
  #expect(model.pendingWindowIntents.isEmpty)
  #expect(model.mainWindows.contains { $0 === home })
  model.windowClosed(home)
}

/// A closed window leaves the list; the others stay.
@MainActor @Test(.hostService) func closingAWindowLeavesTheOthers() {
  let model = AppModel.shared
  let one = Workspace(), other = Workspace()
  model.register(one)
  model.register(one)
  model.register(other)
  #expect(model.mainWindows.filter { $0 === one }.count == 1)
  model.windowClosed(one)
  #expect(!model.mainWindows.contains { $0 === one } && model.mainWindows.contains { $0 === other })
  model.windowClosed(other)
}

/// New projects start where demo copies go, made when it is missing.
@Test func theNewProjectPanelStartsInTheProjectsFolder() throws {
  let root = testTemporaryDirectory.appendingPathComponent("projects-\(UUID())")
  defer { try? FileManager.default.removeItem(at: root) }
  let folder = root.appendingPathComponent("Documents/Tsugumi")
  #expect(AppModel.projectsFolder(["TSUGUMI_PROJECTS_DIR": folder.path]) == folder)
  #expect(AppModel.preparedProjectsFolder(["TSUGUMI_PROJECTS_DIR": folder.path]) == folder)
  var directory: ObjCBool = false
  #expect(FileManager.default.fileExists(atPath: folder.path, isDirectory: &directory) && directory.boolValue)
  // A place that cannot be a folder leaves the panel where it would start.
  let file = root.appendingPathComponent("file")
  try Data().write(to: file)
  #expect(AppModel.preparedProjectsFolder(["TSUGUMI_PROJECTS_DIR": file.appendingPathComponent("Tsugumi").path]) == nil)
  #expect(AppModel.projectsFolder([:]).path.hasSuffix("/Documents/Tsugumi"))
}
