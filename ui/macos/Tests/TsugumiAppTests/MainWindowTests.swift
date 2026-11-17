import Foundation
import SwiftUI
import Testing
@testable import TsugumiApp

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
  model.pendingWindowIntents = [.open(url), .newProject]
  #expect(model.takeWindowIntent() == .open(url))
  #expect(model.takeWindowIntent() == .newProject)
  #expect(model.takeWindowIntent() == nil)
}

/// ⌘N on Home opens the sheet of that window alone.
@MainActor @Test(.hostService) func aNewProjectOnHomeStaysInThatWindow() {
  let model = AppModel.shared
  let one = Workspace(), other = Workspace()
  model.newProject(from: one, openWindow: EnvironmentValues().openWindow)
  #expect(one.requestNewProject && !other.requestNewProject)
  #expect(model.pendingWindowIntents.isEmpty)
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
