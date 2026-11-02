import SwiftUI

/// Shows an overlay while `item` is non-nil, but never as a flash: once shown
/// it stays at least `minimumVisible`, then fades out over `fadeOut`. While
/// it waits or fades it keeps the last item, so the text does not jump.
struct LoadingPresentation<Item: Equatable, Overlay: View>: ViewModifier {
  let item: Item?
  var minimumVisible: Duration = .milliseconds(300)
  var fadeIn: Double = 0.12
  var fadeOut: Double = 0.2
  @ViewBuilder let overlay: (Item) -> Overlay

  @State private var shown: Item?
  @State private var shownAt: ContinuousClock.Instant?
  @State private var hiding: Task<Void, Never>?

  func body(content: Content) -> some View {
    content
      .overlay {
        if let shown {
          overlay(shown)
            .transition(.opacity)
            // let clicks through as soon as loading is over, even while fading
            .allowsHitTesting(item != nil)
        }
      }
      .onAppear {
        // already loading on the first frame: show at once, no fade-in,
        // so the content underneath never shows through
        if let item { shown = item; shownAt = .now }
      }
      .onChange(of: item) { _, new in update(new) }
      .onDisappear { hiding?.cancel() }
  }

  private func update(_ new: Item?) {
    if let new {
      hiding?.cancel()
      hiding = nil
      if shown == nil {
        shownAt = .now
        withAnimation(.easeIn(duration: fadeIn)) { shown = new }
      } else {
        shown = new
      }
      return
    }
    guard shown != nil, hiding == nil else { return }
    let elapsed = shownAt.map { ContinuousClock.now - $0 } ?? minimumVisible
    let wait = max(.zero, minimumVisible - elapsed)
    hiding = Task { @MainActor in
      try? await Task.sleep(for: wait)
      guard !Task.isCancelled else { return }
      withAnimation(.easeOut(duration: fadeOut)) { shown = nil }
      hiding = nil
    }
  }
}

extension View {
  /// Generic form: any overlay, shown for `item` with the timing above.
  func loadingPresentation<Item: Equatable, Overlay: View>(
    _ item: Item?, minimumVisible: Duration = .milliseconds(300), fadeOut: Double = 0.2,
    @ViewBuilder overlay: @escaping (Item) -> Overlay
  ) -> some View {
    modifier(LoadingPresentation(item: item, minimumVisible: minimumVisible, fadeOut: fadeOut, overlay: overlay))
  }

  /// Covers this view with the Tsugumi loading card while `content` is non-nil.
  func tsugumiLoading(_ content: LoadingContent?, onCancel: (() -> Void)? = nil) -> some View {
    loadingPresentation(content) { c in
      // Part of the window: its own background, no floating panel.
      ZStack {
        Color(nsColor: .windowBackgroundColor)
        TsugumiLoadingCard(content: c, onCancel: onCancel)
      }
      .ignoresSafeArea()
    }
  }
}
