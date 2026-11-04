import SwiftUI
import TsugumiAgent

/// What a main window lets the guide's buttons do (MainWindow sets it).
struct GuideWindowActions {
  /// Opens a project in this window (leaving the open one first).
  var openProject: (URL) -> Void
  /// Back to Home.
  var home: () -> Void
  /// The new project sheet on Home.
  var newProject: () -> Void
}

private struct GuideWindowKey: EnvironmentKey { static let defaultValue: GuideWindowActions? = nil }

extension EnvironmentValues {
  var guideWindow: GuideWindowActions? {
    get { self[GuideWindowKey.self] }
    set { self[GuideWindowKey.self] = newValue }
  }
}

// MARK: - The stage

/// Where things are in one main window, for the bird that flies in it: the
/// frame of what the guide points at, and of its corner (window coordinates).
@MainActor
@Observable
final class GuideStage {
  /// The pointed-at view and its frame, while it is on the screen.
  var perch: (id: String, frame: CGRect)?
  /// The bird's home in this window now (zero: none shown, e.g. between Home and a project).
  var corner: CGRect = .zero {
    didSet { if corner != .zero { lastCorner = corner } }
  }
  /// Where its home last was: a flight starts there when no home is shown.
  private(set) var lastCorner: CGRect = .zero
  /// The bird has left its home (no home shows a bird meanwhile, in any window).
  var away = false {
    didSet { if away != oldValue { GuideController.shared.awayWindows += away ? 1 : -1 } }
  }
  /// Where the loading card's bird stands while the card is shown (its feet and size).
  var loadingBird: (feet: CGPoint, scale: CGFloat)?
  /// The guide's bird is flying into the loading card: the card draws no bird until it lands.
  var handoff = false
}

/// Tsugumi's flights in a window: the stage its targets and homes report to,
/// and the bird that flies over it.
struct GuideWindowLayer: ViewModifier {
  @State private var stage = GuideStage()

  func body(content: Content) -> some View {
    content.overlay { GuideFlyer() }.environment(stage)
      // The window closes with the bird in it (a Card's settings): it is back at
      // its home in the project's window, and what it said there is done.
      .onDisappear {
        if let p = stage.perch, GuideController.shared.points(at: p.id) { GuideController.shared.dismissBubble() }
        stage.perch = nil
        stage.away = false
      }
  }
}

// MARK: - Pointing

/// A ring the guide draws around a part of the screen it talks about; the
/// bird flies to it (its frame goes to the window's stage).
private struct GuideTarget: ViewModifier {
  let id: String
  let radius: CGFloat
  @Environment(GuideStage.self) private var stage: GuideStage?

  func body(content: Content) -> some View {
    content.overlay {
      if GuideController.shared.points(at: id) {
        GuideRing(radius: radius).allowsHitTesting(false).accessibilityHidden(true)
          .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { stage?.perch = (id, $0) }
          .onDisappear { if stage?.perch?.id == id { stage?.perch = nil } }
      }
    }
  }
}

private struct GuideRing: View {
  let radius: CGFloat
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var pulse = false

  var body: some View {
    let still = AppPreferences.shared.tsugumiStill(reduceMotion: reduceMotion)
    RoundedRectangle(cornerRadius: radius + 4)
      .strokeBorder(TsugumiPalette.rufous, lineWidth: 2.5)
      .padding(-5)
      .shadow(color: TsugumiPalette.rufous.opacity(0.5), radius: pulse ? 6 : 2)
      .scaleEffect(pulse ? 1.04 : 1)
      .onAppear {
        guard !still else { return }
        withAnimation(.easeInOut(duration: 0.8).repeatForever(autoreverses: true)) { pulse = true }
      }
  }
}

extension View {
  /// Lets the guide point at this view (guide_show "highlight") and fly to it.
  func guideTarget(_ id: String, radius: CGFloat = 8) -> some View { modifier(GuideTarget(id: id, radius: radius)) }
}

// MARK: - The corner

/// Tsugumi on Home, in the bottom-right corner (in a project it is the chat's,
/// GuideNest). It speaks in a bubble beside it (the demo offer, short answers,
/// choices to tap), flies out to what it talks about (GuideFlyer), and a click
/// opens its conversation above it. Shown only with a model connection.
struct GuideCorner: View {
  let screen: GuideController.Screen
  var session: ProjectSession? = nil
  var tab: WorkflowTab? = nil
  @Environment(AppModel.self) private var model
  @Environment(\.guideWindow) private var window
  @Environment(\.openWindow) private var openWindow
  @Environment(\.openSettings) private var openSettings
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @Environment(GuideStage.self) private var stage: GuideStage?

  private var guide: GuideController { .shared }

  var body: some View {
    VStack(alignment: .trailing, spacing: 8) {
      if guide.available, window != nil {
        if guide.open {
          GuidePanel(screen: screen, session: session, tab: tab, perform: perform)
            .transition(.opacity.combined(with: .scale(scale: 0.92, anchor: .bottomTrailing)))
        } else if guide.awayWindows == 0 {
          if guide.offersDemo {
            GuideOfferBubble(perform: perform)
          } else if let b = guide.bubble, guide.speakingChat == nil {
            GuideBubbleView(bubble: b, perform: perform)
          } else if guide.failed, !guide.open {
            Text("うまく答えられませんでした").font(.caption)
              .padding(.horizontal, 9).padding(.vertical, 5)
              .background(.regularMaterial, in: Capsule())
          }
        }
        Button {
          withAnimation(.easeOut(duration: 0.2)) { guide.open.toggle() }
        } label: {
          GuideAvatar(mood: mood, still: AppPreferences.shared.tsugumiStill(reduceMotion: reduceMotion))
        }
        .buttonStyle(.plain)
        .opacity(guide.awayWindows > 0 ? 0 : 1)
        .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { stage?.corner = $0 }
        .onDisappear { stage?.corner = .zero }
        .help(String(localized: "ガイドのツグミに聞く（\(AppPreferences.shared.shortcut(.toggleGuide).label)）"))
        .accessibilityLabel(String(localized: "ガイドのツグミ"))
        .accessibilityValue(guide.activity ?? (guide.open ? String(localized: "会話を表示中") : ""))
        .accessibilityHint(guide.open ? String(localized: "会話を閉じます") : String(localized: "質問できる会話を開きます"))
      }
    }
    .animation(.easeOut(duration: 0.2), value: guide.open)
    .animation(.easeOut(duration: 0.2), value: guide.bubble)
    .onChange(of: runKey, initial: true) { if let session { guide.observe(session, tab: tab) } }
    .onChange(of: contextKey, initial: true) { guide.setContext(screen, session: session, tab: tab) }
  }

  private var perform: GuidePerformer {
    GuidePerformer(model: model, window: window, session: session, tab: tab, openWindow: openWindow, openSettings: openSettings)
  }

  private var contextKey: String { "\(screen)|\(session?.identity.id ?? "")|\(tab?.id.uuidString ?? "")" }

  /// What changes in the project that the guide follows during a tour.
  private var runKey: String {
    guard let s = session else { return "" }
    return "\(s.connected)|\(s.currentRunId ?? "")|\(s.runStatus.map { String(describing: $0.state) } ?? "")"
  }

  private var mood: GuideAvatar.Mood {
    if guide.busy { return .thinking }
    if guide.failed { return .worried }
    if let s = session, s.runActive, let t = guide.tour, GuideController.same(t.path, s.identity.url) { return .watching }
    return .idle
  }
}

// MARK: - The bubble

/// What the bird says, with the replies to tap and the buttons that act.
struct GuideBubbleView: View {
  let bubble: GuideBubble
  let perform: GuidePerformer
  @State private var reply = ""
  private var guide: GuideController { .shared }

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack(alignment: .top, spacing: 6) {
        if bubble.thinking {
          HStack(spacing: 6) {
            ProgressView().controlSize(.mini)
            Text(guide.activity.map { $0 + "…" } ?? String(localized: "考えています…")).foregroundStyle(.secondary)
          }
        } else {
          Text(bubble.say ?? String(localized: "ここです")).fixedSize(horizontal: false, vertical: true)
        }
        Spacer(minLength: 0)
        Button { guide.dismissBubble() } label: { Image(systemName: "xmark").font(.caption2.bold()).foregroundStyle(.secondary) }
          .buttonStyle(.plain).help("閉じる（ツグミは元の場所に戻ります）").accessibilityLabel("閉じる")
      }
      .font(.callout)
      if !bubble.thinking && (!bubble.choices.isEmpty || !bubble.actions.isEmpty) {
        GuideChoices(choices: bubble.choices, actions: bubble.actions, perform: perform)
      }
      if !bubble.thinking {
        GuideReplyField(text: $reply) { guide.choose($0) }
      }
    }
    .padding(10)
    .frame(width: 300, alignment: .leading)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(TsugumiPalette.rufous.opacity(0.45)))
    .shadow(color: .black.opacity(0.18), radius: 8, y: 3)
    .accessibilityElement(children: .contain)
    .accessibilityLabel(String(localized: "ツグミの吹き出し"))
  }
}

/// Replies to tap (they are sent as the user's message) and buttons that act.
struct GuideChoices: View {
  let choices: [String]
  let actions: [GuideAction]
  let perform: GuidePerformer
  /// Where a tapped choice goes (default: whichever Tsugumi offered the bubble).
  var onChoice: ((String) -> Void)? = nil
  private var guide: GuideController { .shared }

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      if !actions.isEmpty {
        HStack(spacing: 6) {
          ForEach(Array(actions.enumerated()), id: \.element.id) { i, a in
            if i == 0 {
              Button(a.label) { perform(a) }.buttonStyle(.borderedProminent).disabled(!perform.enabled(a))
            } else {
              Button(a.label) { perform(a) }.disabled(!perform.enabled(a))
            }
          }
        }
        .controlSize(.small)
      }
      if !choices.isEmpty {
        GuideFlow(spacing: 6) {
          ForEach(choices, id: \.self) { c in
            Button { if let onChoice { onChoice(c) } else { guide.choose(c) } } label: { GuideChip(text: c) }
            .buttonStyle(.plain).disabled(guide.busy || (guide.speakingChat?.busy ?? false))
            .accessibilityHint("この返事を送ります")
          }
        }
      }
    }
  }
}

/// Choices wrap onto as many lines as they need.
struct GuideFlow: Layout {
  var spacing: CGFloat = 6

  func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
    let width = proposal.width ?? 260
    var x: CGFloat = 0, y: CGFloat = 0, line: CGFloat = 0, widest: CGFloat = 0
    for v in subviews {
      let s = v.sizeThatFits(.unspecified)
      if x > 0 && x + s.width > width { y += line + spacing; x = 0; line = 0 }
      x += s.width + spacing
      line = max(line, s.height)
      widest = max(widest, x - spacing)
    }
    return CGSize(width: min(width, widest), height: y + line)
  }

  func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
    var x = bounds.minX, y = bounds.minY, line: CGFloat = 0
    for v in subviews {
      let s = v.sizeThatFits(.unspecified)
      if x > bounds.minX && x + s.width > bounds.maxX { y += line + spacing; x = bounds.minX; line = 0 }
      v.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(s))
      x += s.width + spacing
      line = max(line, s.height)
    }
  }
}

/// The first greeting: what are you interested in? The answer (typed, or a
/// suggestion tapped) has Tsugumi pick a demo and fly to it.
private struct GuideOfferBubble: View {
  let perform: GuidePerformer
  @State private var text = ""
  @FocusState private var focused: Bool
  private var guide: GuideController { .shared }
  static let suggestions = [String(localized: "データの集計とレポート"), String(localized: "ライブ表示"),
                            String(localized: "C++ と Python"), String(localized: "天文・地震・タンパク質のデータ")]

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("こんにちは、ツグミです。何に興味がありますか？ ぴったりのデモを探してご案内します。").fixedSize(horizontal: false, vertical: true)
      GuideFlow(spacing: 6) {
        ForEach(Self.suggestions, id: \.self) { s in
          Button { guide.answerInterest(s) } label: { GuideChip(text: s) }.buttonStyle(.plain)
        }
      }
      GuideReplyField(text: $text, placeholder: String(localized: "興味のあること（例: 画像処理）")) { guide.answerInterest($0) }
        .focused($focused)
      HStack {
        Button("定番のデモを見る") { perform(GuideAction(kind: .openDemo, label: "", demo: "report")) }
        Button("あとで") { guide.answerDemoOffer() }
      }
      .controlSize(.small)
    }
    .font(.callout)
    .padding(10)
    .frame(width: 280, alignment: .leading)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(TsugumiPalette.rufous.opacity(0.45)))
    .shadow(color: .black.opacity(0.18), radius: 8, y: 3)
    .onAppear { focused = true }
  }
}

/// A reply to tap, as a small capsule.
struct GuideChip: View {
  let text: String
  var body: some View {
    Text(text).font(.callout).padding(.horizontal, 10).padding(.vertical, 4)
      .background(Capsule().fill(TsugumiPalette.rufous.opacity(0.14)))
      .overlay(Capsule().strokeBorder(TsugumiPalette.rufous.opacity(0.5)))
  }
}

/// A one-line answer to Tsugumi in its bubble (Return sends it).
struct GuideReplyField: View {
  @Binding var text: String
  var placeholder = String(localized: "ツグミに返事")
  let send: (String) -> Void

  var body: some View {
    HStack(spacing: 4) {
      TextField(placeholder, text: $text)
        .textFieldStyle(.roundedBorder).controlSize(.small)
        .onSubmit(go)
        .accessibilityLabel("ツグミへの返事")
      Button(action: go) { Image(systemName: "arrow.up.circle.fill").foregroundStyle(TsugumiPalette.rufous) }
        .buttonStyle(.plain).disabled(text.trimmingCharacters(in: .whitespaces).isEmpty)
        .help("送信").accessibilityLabel("送信")
    }
  }

  private func go() {
    let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !t.isEmpty else { return }
    text = ""
    send(t)
  }
}

// MARK: - Tsugumi of the chat

/// Where Tsugumi waits in a project: at the top left of the chat (or on the
/// chat button while the chat is closed). It flies out from here (GuideFlyer)
/// and comes back; it thinks while the chat answers.
struct GuideNest: View {
  var busy = false
  var size: CGFloat = 30
  @Environment(GuideStage.self) private var stage: GuideStage?
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    TimelineView(.animation(minimumInterval: 1 / 30, paused: AppPreferences.shared.tsugumiStill(reduceMotion: reduceMotion))) { ctx in
      let t = AppPreferences.shared.tsugumiStill(reduceMotion: reduceMotion) ? 0 : ctx.date.timeIntervalSinceReferenceDate
      Canvas { g, s in
        var pose = BirdPose()
        pose.blink = t.truncatingRemainder(dividingBy: 4.2) < 0.12 ? 1 : 0
        if busy { pose.tilt = 0.12 * sin(t * 3.2) } else { pose.squash = CGFloat(sin(t * 1.6) * 0.02) }
        TsugumiBird.draw(g, at: CGPoint(x: s.width / 2, y: s.height - 2), scale: size / 640, pose: pose)
      }
    }
    .frame(width: size * 1.05, height: size * 0.75)
    .opacity(GuideController.shared.awayWindows > 0 ? 0 : 1)
    .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { stage?.corner = $0 }
    .onDisappear { stage?.corner = .zero }
    .help("ツグミ")
    .accessibilityLabel("ツグミ")
    .accessibilityValue(busy ? String(localized: "考えています") : "")
  }
}

/// The replies and buttons Tsugumi offered with the chat's last answer.
struct ChatChoices: View {
  @Bindable var chat: ChatController
  var tab: WorkflowTab?
  @Environment(AppModel.self) private var model
  @Environment(\.guideWindow) private var window
  @Environment(\.openWindow) private var openWindow
  @Environment(\.openSettings) private var openSettings

  var body: some View {
    if !chat.busy && (!chat.choices.isEmpty || !chat.offered.isEmpty) {
      GuideChoices(choices: chat.choices, actions: chat.offered,
                   perform: GuidePerformer(model: model, window: window, session: chat.session, tab: tab,
                                           openWindow: openWindow, openSettings: openSettings),
                   onChoice: { chat.send($0) })
        .padding(.horizontal, 12).padding(.vertical, 6)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
  }
}

// MARK: - The flight

/// The bird away from its home, over the whole window: it flies to what the
/// guide points at, perches beside it with its bubble, and flies home when
/// the bubble is closed. When a project loads it flies into the loading
/// card's bird and grows to its size (the card's bird is it), and comes out
/// of it again when loading ends. In a window without a home (a Card's
/// settings) it appears beside the target. Without motion it appears there
/// instead of flying.
struct GuideFlyer: View {
  @Environment(GuideStage.self) private var stage
  @Environment(\.guideWindow) private var window
  @Environment(\.openWindow) private var openWindow
  @Environment(\.openSettings) private var openSettings
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var feet: CGPoint?
  @State private var scale: CGFloat = 0.1
  @State private var flyingUntil = Date.distantPast
  @State private var arrived = false
  /// Landed in the loading card: the card's own bird is shown instead.
  @State private var inCard = false
  /// Facing left (mirrored); in the loading card it faces as the card's bird does.
  @State private var facingLeft = true
  @State private var bubbleSize = CGSize(width: 300, height: 90)

  private var guide: GuideController { .shared }
  private var model: AppModel { .shared }
  private var still: Bool { AppPreferences.shared.tsugumiStill(reduceMotion: reduceMotion) }
  static let birdScale: CGFloat = 0.1

  /// The pointed-at frame while the bubble stands there (window coordinates).
  private var perch: CGRect? {
    guard guide.available, let b = guide.bubble, let t = b.target, let p = stage.perch, guide.points(at: p.id) || p.id == t else { return nil }
    return p.frame
  }

  /// Where the bird goes: into the loading card, beside its target, or home.
  /// Home is where its home is shown now (nil: none yet; the bird waits where it is).
  private enum Goal: Equatable { case loading(CGPoint, CGFloat), perch(CGPoint, left: Bool), home(CGPoint?) }

  private func goal(_ o: CGPoint, _ size: CGSize) -> Goal {
    if guide.available, window != nil, let l = stage.loadingBird { return .loading(l.feet, l.scale) }
    if let r = perch { return .perch(feetPoint(r, origin: o, size: size), left: r.midX > o.x + size.width / 2) }
    return .home(stage.corner == .zero ? nil : CGPoint(x: stage.corner.midX, y: stage.corner.maxY - 4))
  }

  var body: some View {
    GeometryReader { geo in
      let o = geo.frame(in: .global).origin
      let size = geo.size
      ZStack(alignment: .topLeading) {
        if stage.away, let f = feet, !inCard {
          let left = facingLeft
          TimelineView(.animation(minimumInterval: 1 / 30, paused: still)) { ctx in
            let t = ctx.date.timeIntervalSinceReferenceDate
            let flying = ctx.date < flyingUntil
            Canvas { g, s in
              var pose = BirdPose()
              if flying { pose.flap = (sin(t * 22) + 1) / 2; pose.tilt = -0.1 }
              else if !still { pose.hop = CGFloat(max(0, sin(t * 2.4)) * 2); pose.blink = t.truncatingRemainder(dividingBy: 4.2) < 0.12 ? 1 : 0 }
              TsugumiBird.draw(g, at: CGPoint(x: s.width / 2, y: s.height - 8), scale: scale, pose: pose)
            }
            .scaleEffect(x: left ? -1 : 1, y: 1)
          }
          .frame(width: 900 * scale, height: 640 * scale)
          .shadow(color: .black.opacity(0.25), radius: 3, y: 2)
          .position(x: f.x - o.x, y: f.y - o.y - 320 * scale + 8)
          .allowsHitTesting(false)
          .accessibilityHidden(true)
          if arrived, perch != nil, let b = guide.bubble {
            GuideBubbleView(bubble: b, perform: GuidePerformer(model: model, window: window, session: guide.flyerSession,
                                                               tab: guide.flyerTab, openWindow: openWindow, openSettings: openSettings))
              .onGeometryChange(for: CGSize.self) { $0.size } action: { bubbleSize = $0 }
              .position(bubbleCenter(feet: CGPoint(x: f.x - o.x, y: f.y - o.y), left: left, in: size))
              .transition(.opacity.combined(with: .scale(scale: 0.9)))
          }
        }
      }
      .frame(width: size.width, height: size.height, alignment: .topLeading)
      .onChange(of: goal(o, size), initial: true) { _, g in fly(to: g) }
    }
    // Tsugumi opens a Card's settings to show it (in the window of that project).
    .onChange(of: guide.openRequest, initial: true) { _, r in
      guard let r, window != nil, let s = guide.flyerSession else { return }
      guide.openRequest = nil
      let path = String(r.dropFirst(5)).split(separator: "/").map(String.init)
      let k = s.instance(key(path)) != nil ? key(path) : key((guide.flyerTab?.scope ?? []) + path)
      guard s.instance(k) != nil else { return }
      openWindow(id: "card", value: CardRef(path: s.identity.url.path, card: k))
    }
  }

  private func fly(to goal: Goal) {
    let last = stage.lastCorner == .zero ? nil : CGPoint(x: stage.lastCorner.midX, y: stage.lastCorner.maxY - 4)
    let duration = still ? 0 : 0.9
    func go(_ p: CGPoint, scale s: CGFloat, then: @escaping @MainActor () -> Void) {
      flyingUntil = Date().addingTimeInterval(duration)
      withAnimation(still ? nil : .easeInOut(duration: duration)) { feet = p; scale = s }
      Task { @MainActor in
        try? await Task.sleep(for: .seconds(duration))
        if feet == p { then() }
      }
    }
    switch goal {
    case .loading(let p, let s):
      // From wherever it is (its home, a demo's card) into the loading card's bird.
      if !stage.away {
        guard let start = last else {
          // Nowhere to come from: it is the card's bird at once.
          stage.away = true
          inCard = true
          return
        }
        feet = start
        scale = Self.birdScale
        stage.away = true
      }
      inCard = false
      arrived = false
      facingLeft = false
      stage.handoff = true
      // What it said by the demo's card is done.
      if guide.bubble?.target?.hasPrefix("demo:") == true { guide.dismissBubble() }
      go(p, scale: s) { inCard = true; stage.handoff = false }
    case .perch(let p, let left):
      if !stage.away || inCard {
        // Out of the loading card where it was, from home, or (no home) right there.
        if !inCard { feet = last ?? p; scale = Self.birdScale }
        inCard = false
        stage.away = true
      }
      arrived = false
      facingLeft = left
      go(p, scale: Self.birdScale) { withAnimation(.easeOut(duration: 0.2)) { arrived = true } }
    case .home(let home):
      guard stage.away else { return }
      arrived = false
      stage.handoff = false
      if inCard {
        // Out of the loading card: from the card's bird, as it is.
        inCard = false
        facingLeft = false
      }
      // No home shown yet (Home has gone, the chat not there yet): it waits where it is.
      guard let home else { return }
      facingLeft = home.x < (feet?.x ?? home.x)
      go(home, scale: Self.birdScale) { stage.away = false; facingLeft = true }
    }
  }

  /// Where the bird stands: on the top edge of the target, at its right end
  /// (its left end near the window's right edge), inside the window.
  private func feetPoint(_ r: CGRect, origin o: CGPoint, size: CGSize) -> CGPoint {
    let right = r.maxX + 40 < o.x + size.width - 40
    let x = right ? r.maxX + 18 : r.minX - 18
    return CGPoint(x: min(max(x, o.x + 40), o.x + size.width - 40), y: min(max(r.minY + 10, o.y + 70), o.y + size.height - 10))
  }

  private func bubbleCenter(feet f: CGPoint, left: Bool, in size: CGSize) -> CGPoint {
    let w = bubbleSize.width, h = bubbleSize.height
    var x = f.x + (left ? w / 2 - 30 : -w / 2 + 30)
    var y = f.y + 14 + h / 2
    if y + h / 2 > size.height - 8 { y = f.y - 70 - h / 2 }
    x = min(max(x, w / 2 + 8), size.width - w / 2 - 8)
    y = min(max(y, h / 2 + 8), size.height - h / 2 - 8)
    return CGPoint(x: x, y: y)
  }

}

/// What a button of the guide does, with the app's own controls in this window.
@MainActor
struct GuidePerformer {
  let model: AppModel
  let window: GuideWindowActions?
  let session: ProjectSession?
  let tab: WorkflowTab?
  let openWindow: OpenWindowAction
  let openSettings: OpenSettingsAction
  private var guide: GuideController { .shared }

  func callAsFunction(_ a: GuideAction) {
    switch a.kind {
    case .openDemo:
      openDemo(a.demo ?? "report")
    case .showDemos:
      guide.demosRequested = true
      if session != nil { window?.home() }
    case .run:
      guard let s = session, let tab, s.connected, !s.runActive else { return }
      RunActions.runAll(s, scope: tab.scope)
    case .stop:
      guard let s = session, s.runActive else { return }
      Task { await s.cancelRun() }
    case .openCard:
      guard let s = session, let id = a.card else { return }
      let scope = tab?.scope ?? []
      let k = s.instance(key(scope + [id])) != nil ? key(scope + [id]) : key([id])
      guard s.instance(k) != nil else { return }
      openWindow(id: "card", value: CardRef(path: s.identity.url.path, card: k))
    case .openSettings:
      UserDefaults.standard.set(ApplicationSettings.Category.api.rawValue, forKey: "settings.category")
      openSettings()
    case .newProject:
      window?.newProject()
    case .home:
      window?.home()
    }
  }

  /// A button that cannot do anything now is shown but disabled (a run while one runs, ...).
  func enabled(_ a: GuideAction) -> Bool {
    switch a.kind {
    case .run: return session.map { $0.connected && !$0.runActive } ?? false
    case .stop: return session?.runActive ?? false
    case .openCard: return session != nil
    case .openDemo, .home, .newProject: return !(session?.runActive ?? false)
    default: return true
    }
  }

  private func openDemo(_ id: String) {
    guard let demo = model.demos.first(where: { $0.id == id }) ?? model.demos.first(where: { $0.id == "report" }) else { return }
    if session?.runActive == true {
      model.error = String(localized: "実行中はデモを開けません。停止するか終了を待ってください。")
      return
    }
    do {
      let url = try model.copyDemo(demo)
      // The bird stays by the demo's card until the loading card comes, then flies into it.
      guide.startTour(demo, copy: url)
      window?.openProject(url)
    } catch {
      model.error = "\(error)"
    }
  }
}

// MARK: - The bird

/// The guide's bird in a round badge. It breathes and blinks; while the
/// guide thinks it tilts its head, while a tour's run goes it hops, and when
/// something went wrong it droops. Still when motion is reduced.
struct GuideAvatar: View {
  enum Mood { case idle, thinking, watching, worried }
  let mood: Mood
  let still: Bool

  var body: some View {
    TimelineView(.animation(minimumInterval: 1 / 30, paused: still)) { ctx in
      let t = still ? 0 : ctx.date.timeIntervalSinceReferenceDate
      Canvas { g, size in
        TsugumiBird.draw(g, at: CGPoint(x: size.width * 0.47, y: size.height * 0.82), scale: 0.066, pose: pose(t))
      }
      .scaleEffect(x: -1, y: 1)  // facing the window's content
    }
    .frame(width: 52, height: 52)
    .background(Circle().fill(TsugumiPalette.cream))
    .overlay(Circle().strokeBorder(mood == .worried ? Color.orange : TsugumiPalette.rufous.opacity(0.7), lineWidth: 1.5))
    .shadow(color: .black.opacity(0.2), radius: 4, y: 2)
    .contentShape(Circle())
  }

  private func pose(_ t: Double) -> BirdPose {
    var p = BirdPose()
    let blinkPhase = t.truncatingRemainder(dividingBy: 4.2)
    p.blink = blinkPhase < 0.12 ? 1 : 0
    switch mood {
    case .idle:
      p.squash = CGFloat(sin(t * 1.6) * 0.02)
    case .thinking:
      p.tilt = 0.12 * sin(t * 3.2)
    case .watching:
      p.hop = CGFloat(max(0, sin(t * 5.5)) * 4)
      p.flap = max(0, sin(t * 11)) * 0.5
    case .worried:
      p.tilt = -0.12
      p.squash = 0.06
    }
    return p
  }
}

// MARK: - The panel

/// The conversation with the guide: its greeting and demo offer, the tour
/// being shown, the replies with their buttons, and a field to ask in.
private struct GuidePanel: View {
  let screen: GuideController.Screen
  let session: ProjectSession?
  let tab: WorkflowTab?
  let perform: GuidePerformer
  var docked = false
  @Environment(\.guideWindow) private var window
  @Environment(\.openSettings) private var openSettings
  @State private var text = ""
  @FocusState private var focused: Bool

  private var guide: GuideController { .shared }

  var body: some View {
    VStack(spacing: 0) {
      header
      Divider()
      if let t = guide.tour { tourBar(t) }
      ScrollViewReader { proxy in
        ScrollView {
          LazyVStack(alignment: .leading, spacing: 10) {
            if guide.offersDemo { offer }
            if guide.items.isEmpty && !guide.offersDemo { hints }
            ForEach(guide.items) { item in row(item).id(item.id) }
            if let s = guide.status { statusRow(s) }
            Color.clear.frame(height: 1).id("end")
          }
          .padding(12)
        }
        .onChange(of: guide.items.last?.text) { proxy.scrollTo("end", anchor: .bottom) }
        .onChange(of: guide.items.count) { proxy.scrollTo("end", anchor: .bottom) }
      }
      Divider()
      input
    }
    .modifier(GuidePanelFrame(docked: docked))
    .onExitCommand { guide.open = false }
    .onAppear { focused = !guide.offersDemo }
    .accessibilityElement(children: .contain)
    .accessibilityLabel(String(localized: "ガイドのツグミとの会話"))
  }

  private var header: some View {
    HStack(spacing: 8) {
      VStack(alignment: .leading, spacing: 1) {
        Text("ツグミ").font(.headline)
        Text(guide.activity.map { $0 + "…" } ?? String(localized: "使い方の質問に答えます"))
          .font(.caption).foregroundStyle(.secondary).lineLimit(1)
      }
      Spacer()
      Menu {
        Button("新しい会話") { guide.newConversation() }.disabled(guide.busy)
        Button("デモを見る…") { perform(GuideAction(kind: .showDemos, label: "")) }
        Button("API の設定…") { perform(GuideAction(kind: .openSettings, label: "")) }
      } label: { Image(systemName: "ellipsis.circle") }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
        .accessibilityLabel("その他")
      Button { guide.open = false } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary) }
        .buttonStyle(.plain)
        .help("閉じる（Esc）。ツグミは右下に残ります")
        .accessibilityLabel("閉じる")
    }
    .padding(.horizontal, 12).padding(.vertical, 9)
  }

  private func tourBar(_ t: GuideController.Tour) -> some View {
    let here = session.map { GuideController.same(t.path, $0.identity.url) } ?? false
    return HStack(spacing: 6) {
      Image(systemName: "sparkles").foregroundStyle(TsugumiPalette.rufous)
      Text(here ? String(localized: "デモ「\(t.title)」を案内中") : String(localized: "デモ「\(t.title)」の案内は中断中"))
        .font(.caption).lineLimit(1)
      Spacer()
      if !here {
        Button("続ける") { if let url = guide.resumeTour() { window?.openProject(url) } }
          .controlSize(.small).disabled(session?.runActive == true)
      }
      Button("終える") { guide.endTour() }.controlSize(.small)
    }
    .padding(.horizontal, 12).padding(.vertical, 6)
    .background(TsugumiPalette.rufous.opacity(0.1))
  }

  private var offer: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("こんにちは、ツグミです。Tsugumi の使い方を、ここからいつでもご案内します。")
      Text("まず、動くデモを一緒に見てみますか？ デモは編集できるコピーとして開き、実際に実行しながら Card・接続・結果の見方をお見せします。")
        .foregroundStyle(.secondary)
      HStack {
        Button("見る") { perform(GuideAction(kind: .openDemo, label: "", demo: "report")) }
          .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
        Button("あとで") {
          guide.answerDemoOffer()
          focused = true
        }
      }
    }
    .font(.callout)
    .padding(10)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(TsugumiPalette.cream.opacity(0.35), in: RoundedRectangle(cornerRadius: 10))
  }

  private var hints: some View {
    VStack(alignment: .leading, spacing: 6) {
      Text("いま見ている画面について、気軽に聞いてください。").font(.callout).foregroundStyle(.secondary)
      ForEach(hintQuestions, id: \.self) { q in
        Button(q) { send(q) }.buttonStyle(.link).font(.callout)
      }
    }
  }

  private var hintQuestions: [String] {
    switch screen {
    case .project:
      return [String(localized: "この Card は何をしている？"), String(localized: "いま何が起きている？"),
              String(localized: "グラフを表示するには？"), String(localized: "デモを見せて")]
    default:
      return [String(localized: "Tsugumi で何ができる？"), String(localized: "デモを見せて"), String(localized: "新しいプロジェクトの作り方は？")]
    }
  }

  @ViewBuilder private func row(_ item: GuideController.Item) -> some View {
    switch item.role {
    case "user":
      HStack {
        Spacer(minLength: 40)
        Text(item.text).font(.callout).textSelection(.enabled)
          .padding(.horizontal, 10).padding(.vertical, 6)
          .background(Color.accentColor.opacity(0.15), in: RoundedRectangle(cornerRadius: 10))
      }
    case "note":
      Label(item.text, systemImage: "info.circle").font(.caption).foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .center)
    default:
      VStack(alignment: .leading, spacing: 8) {
        if !item.text.isEmpty { MarkdownView(text: item.text, fontSize: 13) }
        if !item.actions.isEmpty {
          HStack(spacing: 6) {
            ForEach(Array(item.actions.enumerated()), id: \.element.id) { i, a in
              if i == 0 {
                Button(a.label) { perform(a) }.buttonStyle(.borderedProminent).disabled(!enabled(a))
              } else {
                Button(a.label) { perform(a) }.disabled(!enabled(a))
              }
            }
          }
          .controlSize(.small)
        }
      }
    }
  }

  private func statusRow(_ s: String) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Label(s, systemImage: guide.failed ? "exclamationmark.triangle" : "info.circle")
        .font(.caption).foregroundStyle(guide.failed ? Color.orange : .secondary).textSelection(.enabled)
      if guide.failed {
        HStack {
          Button("再試行") { guide.retry() }.disabled(guide.busy)
          Button("API の設定を開く") { perform(GuideAction(kind: .openSettings, label: "")) }
        }
        .controlSize(.small)
      }
    }
  }

  /// A button that cannot do anything now is shown but disabled (a run while one runs, ...).
  private func enabled(_ a: GuideAction) -> Bool {
    switch a.kind {
    case .run: return session.map { $0.connected && !$0.runActive } ?? false
    case .stop: return session?.runActive ?? false
    case .openCard: return session != nil
    case .openDemo, .home, .newProject: return !(session?.runActive ?? false)
    default: return true
    }
  }

  private var input: some View {
    HStack(alignment: .bottom, spacing: 6) {
      TextField("ツグミに質問", text: $text, axis: .vertical)
        .textFieldStyle(.plain).lineLimit(1...4)
        .focused($focused)
        .onSubmit { send(text) }
        .accessibilityLabel("ツグミへの質問")
      if guide.busy {
        Button { guide.cancel() } label: { Image(systemName: "stop.circle.fill") }
          .buttonStyle(.plain).help("回答を止める").accessibilityLabel("回答を止める")
      } else {
        Button { send(text) } label: { Image(systemName: "arrow.up.circle.fill") }
          .buttonStyle(.plain).foregroundStyle(TsugumiPalette.rufous)
          .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
          .help("送信").accessibilityLabel("送信")
      }
    }
    .font(.title3)
    .padding(10)
  }

  private func send(_ q: String) {
    guard !guide.busy, !q.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
    if guide.offersDemo { guide.answerDemoOffer() }
    guide.ask(q, screen: screen, session: session, tab: tab)
    text = ""
  }
}

// MARK: - First start

/// The first start: choose Anthropic or OpenAI and enter that provider's API
/// key (one is enough), checked by connecting, or start without a guide.
/// The key goes to the Keychain the same way as in Settings > API.
struct GuideSetupView: View {
  let onDone: () -> Void
  @State private var provider = UserDefaults.standard.string(forKey: "agent.provider") == "openai" ? "openai" : "anthropic"
  @State private var key = ""
  @State private var working = false
  @State private var message: String?
  @State private var saved = false

  private var config: ProviderConfig { provider == "openai" ? .openAI : .anthropic }

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      HStack(spacing: 12) {
        GuideAvatar(mood: .idle, still: true)
        VStack(alignment: .leading, spacing: 2) {
          Text("ガイドのツグミを呼ぶ準備").font(.title2.bold())
          Text("右下のツグミが、使い方の質問に答えたりデモを案内したりします。").foregroundStyle(.secondary)
        }
      }
      Text("ツグミとチャットの Agent は、あなたの API キーで OpenAI か Anthropic のモデルを使います。どちらか一方のキーがあれば十分です。キーはこの Mac のキーチェーンにだけ保存し、プロジェクト・会話の記録・ログには残しません。")
        .font(.callout).fixedSize(horizontal: false, vertical: true)
      Picker("使うモデル", selection: $provider) {
        Text("Anthropic").tag("anthropic")
        Text("OpenAI").tag("openai")
      }
      .pickerStyle(.segmented)
      .onChange(of: provider) { message = nil; saved = false }
      SecureField(provider == "openai" ? "OpenAI の API キー" : "Anthropic の API キー", text: $key)
        .textFieldStyle(.roundedBorder)
        .onSubmit(connect)
      if let message {
        Label(message, systemImage: "exclamationmark.triangle").font(.caption).foregroundStyle(.orange)
          .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
      }
      Text("キーがなくても、Tsugumi はすべて UI から使えます（その間ツグミは現れません）。あとから設定の「API」でキーを登録すると現れます。")
        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      HStack {
        Button("スキップして始める") {
          GuideController.shared.finishSetup()
          onDone()
        }
        Spacer()
        if working { ProgressView().controlSize(.small) }
        if saved && message != nil {
          Button("再試行", action: check).disabled(working)
        }
        Button("接続して始める", action: connect)
          .keyboardShortcut(.defaultAction).buttonStyle(.borderedProminent)
          .disabled(working || key.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
      }
    }
    .padding(24)
    .frame(width: 500)
    .tint(TsugumiPalette.rufous)
  }

  private func connect() {
    let k = key.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !k.isEmpty, !working else { return }
    do {
      try Keychain.save(k, account: config.name)
    } catch {
      message = String(localized: "キーを保存できませんでした: \(String(describing: error))")
      return
    }
    key = ""
    saved = true
    UserDefaults.standard.set(provider, forKey: "agent.provider")
    check()
  }

  /// A connection check: the provider lists its models with the key.
  private func check() {
    working = true
    message = nil
    Task {
      do {
        _ = try await MessagesProvider(config: AgentSettings.current()).models()
        working = false
        GuideController.shared.finishSetup()
        onDone()
      } catch {
        working = false
        message = String(localized: "接続を確かめられませんでした（\(String(describing: error))）。キーを入れ直すか、ネットワークを確かめて再試行してください。")
      }
    }
  }
}

/// A floating panel above the bird, or a docked one filling its place in a column.
private struct GuidePanelFrame: ViewModifier {
  let docked: Bool

  func body(content: Content) -> some View {
    if docked {
      content.frame(maxWidth: .infinity, maxHeight: .infinity)
    } else {
      content
        .frame(width: 340, height: 440)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(.secondary.opacity(0.25)))
        .shadow(color: .black.opacity(0.18), radius: 12, y: 4)
    }
  }
}
