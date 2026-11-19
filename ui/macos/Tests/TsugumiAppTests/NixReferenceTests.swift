import SwiftUI
import Testing
@testable import TsugumiApp

@Test func nixReferencesProjectNestedTargetsWithoutCrossingScopes() {
  #expect(visibleNixCard("child/inner", scope: [], visible: ["child", "sink"]) == "child")
  #expect(visibleNixCard("child/inner", scope: ["child"], visible: ["child/inner"]) == "child/inner")
  #expect(visibleNixCard("other/inner", scope: ["child"], visible: ["child/inner"]) == nil)
  #expect(visibleNixCard("missing", scope: [], visible: ["sink"]) == nil)
}

@Test func nixReferenceLinksFollowTheCamera() throws {
  let source = CGRect(x: 0, y: 0, width: 100, height: 80)
  let target = source.offsetBy(dx: 300, dy: 0)
  let camera = CGAffineTransform(a: 2, b: 0, c: 0, d: 2, tx: 20, ty: 30)
  let end = try #require(nixCurve(source, target).applying(camera).currentPoint)
  let expected = CGPoint(x: target.minX, y: target.midY).applying(camera)
  #expect(hypot(end.x - expected.x, end.y - expected.y) < 0.01)
}

@Test func nixReferenceLinksHighlightTheHoveredCardOrThePickedLink() throws {
  let refs = [
    CardNixReference(source: "analysis", target: "source", name: "source", valid: true),
    CardNixReference(source: "report", target: "analysis", name: "analysis", valid: true),
    CardNixReference(source: "report", target: "missing", name: "missing", valid: false),
    CardNixReference(source: "report", target: "child/inner", name: "child/inner", valid: true),
  ]
  let links = visibleNixLinks(refs, scope: [], visible: ["source", "analysis", "report", "other", "child"])
  let ends = { (l: [NixLink]) in Set(l.map { "\($0.source)>\($0.target)" }) }
  let picked = try #require(links.first { $0.source == "report" && $0.target == "analysis" })
  // A reference to a Card that is not on the canvas has no link; a nested one meets its container.
  #expect(ends(links) == ["analysis>source", "report>analysis", "report>child"])
  #expect(ends(activeNixLinks(links, hovered: "analysis", selected: nil)) == ["analysis>source", "report>analysis"])
  #expect(ends(activeNixLinks(links, hovered: "source", selected: picked.reference.id)) == ["analysis>source"])
  #expect(activeNixLinks(links, hovered: "other", selected: nil).isEmpty)
  #expect(ends(activeNixLinks(links, hovered: nil, selected: picked.reference.id)) == ["report>analysis"])
  #expect(activeNixLinks(links, hovered: nil, selected: nil).isEmpty)
  let special = CardNixReference(source: "report", target: "analysis", name: "analysis result", valid: true)
  #expect(special.expression == #"inputs."analysis result""#)
}
