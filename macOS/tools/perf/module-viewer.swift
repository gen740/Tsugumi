// Runs the shared chart viewer in offscreen WKWebViews, without opening a window.
// swift macOS/tools/perf/module-viewer.swift <directory containing viewer.html and libraries>
import AppKit
import WebKit

@MainActor
func checkViewer(_ directory: URL) async throws {
  let started = ContinuousClock.now
  for cycle in 0..<3 {
    let configuration = WKWebViewConfiguration()
    configuration.websiteDataStore = .nonPersistent()
    let web = WKWebView(frame: CGRect(x: 0, y: 0, width: 640, height: 400), configuration: configuration)
    web.loadFileURL(directory.appendingPathComponent("viewer.html"), allowingReadAccessTo: directory)
    var ready = false
    for _ in 0..<400 {
      if (try? await web.callAsyncJavaScript("return typeof tsugumiRender === 'function'", contentWorld: .page)) as? Bool == true {
        ready = true; break
      }
      try await Task.sleep(for: .milliseconds(25))
    }
    guard ready else { throw NSError(domain: "Viewer", code: 1, userInfo: [NSLocalizedDescriptionKey: "Viewer did not load"]) }
    let answer = try await web.callAsyncJavaScript("""
      const first = tsugumiRender({name:'plotly', json:JSON.stringify({data:[{x:[0,1],y:[1,2]}]}), interactive:true});
      const latest = tsugumiRender({name:'plotly', json:JSON.stringify({data:[{x:[0,1],y:[3,4]}]}), interactive:true});
      await Promise.all([first, latest]);
      if (document.body.dataset.state !== 'ok' || document.getElementById('view').data[0].y[0] !== 3) throw Error('stale Plotly result');
      const plot = await Plotly.toImage(document.getElementById('view'), {format:'png',width:640,height:400});
      if (!plot.startsWith('data:image/png;')) throw Error('Plotly snapshot failed');
      const spec = {mark:'point',data:{values:[{x:1,y:2},{x:2,y:3}]},encoding:{x:{field:'x',type:'quantitative'},y:{field:'y',type:'quantitative'}}};
      await tsugumiRender({name:'vega-lite',json:JSON.stringify(spec),interactive:false});
      const original = vegaEmbed; let released = 0;
      window.vegaEmbed = async (...args) => { const result = await original(...args); const end = result.finalize; result.finalize = () => { released++; end(); }; return result; };
      for (let i=0; i<5; i++) { spec.data.values[0].y=i; await tsugumiRender({name:'vega-lite',json:JSON.stringify(spec),interactive:true}); }
      await tsugumiRender({name:'plotly',json:JSON.stringify({data:[{x:[0,1],y:[5,6]}]}),interactive:false});
      if (released !== 5) throw Error('Vega views were not finalized: '+released);
      if (document.body.dataset.state !== 'ok') throw Error(document.getElementById('error').textContent);
      return {cycle:cycle, finalized:released, pngBytes:plot.length};
      """, arguments: ["cycle": cycle], contentWorld: .page)
    print("WKWebView: \(answer ?? "nil")")
    web.stopLoading()
    web.configuration.userContentController.removeAllScriptMessageHandlers()
  }
  print("WKWebView repeated open/update/close passed in \(started.duration(to: .now))")
}

let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
NSApplication.shared.setActivationPolicy(.prohibited)
Task { @MainActor in
  do { try await checkViewer(directory); exit(0) }
  catch { fputs("\(error)\n", stderr); exit(1) }
}
RunLoop.main.run()
