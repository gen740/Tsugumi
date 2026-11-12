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
}

@Test func nixReferenceLinksHighlightTheHoveredCardOrThePickedLink() {
  let refs = [
    CardNixReference(source: "analysis", target: "source", name: "source", local: false, valid: true),
    CardNixReference(source: "report", target: "analysis", name: "analysis", local: true, valid: true),
    CardNixReference(source: "report", target: "missing", name: "missing", local: false, valid: false),
    CardNixReference(source: "report", target: "child/inner", name: "child/inner", local: false, valid: true),
  ]
  let links = visibleNixLinks(refs, scope: [], visible: ["source", "analysis", "report", "other", "child"])
  let ends = { (l: [NixLink]) in l.map { "\($0.source)>\($0.target)" } }
  // A reference to a Card that is not on the canvas has no link; a nested one meets its container.
  #expect(ends(links) == ["analysis>source", "report>analysis", "report>child"])
  #expect(ends(activeNixLinks(links, hovered: "analysis", selected: nil)) == ["analysis>source", "report>analysis"])
  #expect(ends(activeNixLinks(links, hovered: "source", selected: links[1].reference.id)) == ["analysis>source"])
  #expect(activeNixLinks(links, hovered: "other", selected: nil).isEmpty)
  #expect(ends(activeNixLinks(links, hovered: nil, selected: links[1].reference.id)) == ["report>analysis"])
  #expect(activeNixLinks(links, hovered: nil, selected: nil).isEmpty)
  #expect(refs[1].expression == "inputs.local.\"analysis\"")
}
