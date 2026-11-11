import SwiftUI
import Testing
@testable import TsugumiApp

@Test func nixReferencesProjectNestedTargetsWithoutCrossingScopes() {
  #expect(visibleNixCard("child/inner", scope: [], visible: ["child", "sink"]) == "child")
  #expect(visibleNixCard("child/inner", scope: ["child"], visible: ["child/inner"]) == "child/inner")
  #expect(visibleNixCard("other/inner", scope: ["child"], visible: ["child/inner"]) == nil)
  #expect(visibleNixCard("missing", scope: [], visible: ["sink"]) == nil)
}

@Test func nixReferenceLinksFollowTheCamera() {
  let curve = nixCurve(CGRect(x: 0, y: 0, width: 100, height: 80), CGRect(x: 300, y: 0, width: 100, height: 80))
  let transformed = curve.applying(CGAffineTransform(a: 2, b: 0, c: 0, d: 2, tx: 20, ty: 30))
  #expect(transformed.currentPoint == CGPoint(x: 620, y: 110))
  #expect(recordedStoreName("/nix/store/" + String(repeating: "a", count: 32) + "-python-3.13") == "python-3.13")
}
