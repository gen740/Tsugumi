import AppKit
import Darwin
import WebKit

/// What moving the canvas costs, for the UI script's `perf` step: the camera
/// is moved along a fixed path, and after every move the main thread is left
/// to do what the move caused (SwiftUI's update, layout, the commit) before
/// the next. `idle` moves nothing: what the loop itself costs.
///
/// The app runs behind other windows, where nothing may reach the screen, so
/// this is the main thread's cost and the processes' CPU time, not the frame
/// rate (`visible` says whether the window was on screen).
@MainActor
enum CanvasPerf {
  typealias Camera = (x: Double, y: Double, zoom: Double)

  static func run(mode: String, steps: Int, window: NSWindow?, get: () -> Camera, set: (Camera) -> Void) async -> String {
    let start = get()
    // Behind other windows the app naps: timers are held back, and the loop would measure that.
    let awake = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .latencyCritical], reason: "perf")
    defer { ProcessInfo.processInfo.endActivity(awake) }
    let webs = webViews(in: window)
    let pids = Set(webs.compactMap { ($0.value(forKey: "_webProcessIdentifier") as? NSNumber)?.int32Value }.filter { $0 > 0 })
    let app0 = usage(getpid()).cpu
    let web0 = pids.reduce(0) { $0 + usage($1).cpu }
    var ms: [Double] = []
    let clock = ContinuousClock()
    let began = clock.now
    for i in 0..<steps {
      let t = clock.now
      let phase = Double(i) / Double(max(steps, 1)) * 4 * .pi
      var c = start
      switch mode {
      case "pan":
        c.x = start.x + 300 * sin(phase)
        c.y = start.y + 200 * (cos(phase) - 1)
      case "zoom":
        // Between 0.6 and 1.4 of where it was, around the canvas's origin.
        let z = start.zoom * (1 + 0.4 * sin(phase))
        c = (start.x * z / start.zoom, start.y * z / start.zoom, z)
      default: break
      }
      if mode != "idle" { set(c) }
      // The run loop goes idle here: the update and the commit happen before the timer.
      try? await Task.sleep(for: .milliseconds(1))
      ms.append(seconds(clock.now - t) * 1000)
    }
    let total = seconds(clock.now - began)
    set(start)
    let app = usage(getpid())
    let web = pids.map { usage($0) }
    ms.sort()
    func at(_ q: Double) -> Double { ms.isEmpty ? 0 : ms[min(ms.count - 1, Int(Double(ms.count) * q))] }
    return String(format: "mode=%@ steps=%d total=%.2fs mean=%.2fms p50=%.2fms p95=%.2fms max=%.2fms "
                    + "appcpu=%.2fs appcpu/step=%.2fms webviews=%d webprocs=%d webcpu=%.2fs appmem=%.0fMB webmem=%.0fMB visible=%@",
                  mode, steps, total, ms.reduce(0, +) / Double(max(ms.count, 1)), at(0.5), at(0.95), ms.last ?? 0,
                  app.cpu - app0, (app.cpu - app0) * 1000 / Double(max(steps, 1)), webs.count, pids.count, web.reduce(0) { $0 + $1.cpu } - web0,
                  app.memory / 1e6, web.reduce(0) { $0 + $1.memory } / 1e6,
                  (window?.occlusionState.contains(.visible) ?? false) ? "yes" : "no")
  }

  private static func seconds(_ d: Duration) -> Double {
    Double(d.components.seconds) + Double(d.components.attoseconds) / 1e18
  }

  private static func webViews(in window: NSWindow?) -> [WKWebView] {
    var found: [WKWebView] = []
    func walk(_ v: NSView) {
      if let w = v as? WKWebView { found.append(w) }
      v.subviews.forEach(walk)
    }
    if let root = window?.contentView?.superview ?? window?.contentView { walk(root) }
    return found
  }

  /// A process's CPU time (seconds) and memory footprint (bytes).
  private static func usage(_ pid: pid_t) -> (cpu: Double, memory: Double) {
    var info = rusage_info_v2()
    let ok = withUnsafeMutablePointer(to: &info) {
      $0.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { proc_pid_rusage(pid, RUSAGE_INFO_V2, $0) }
    }
    guard ok == 0 else { return (0, 0) }
    var base = mach_timebase_info_data_t()
    mach_timebase_info(&base)
    let ns = Double(info.ri_user_time + info.ri_system_time) * Double(base.numer) / Double(base.denom)
    return (ns / 1e9, Double(info.ri_phys_footprint))
  }
}
