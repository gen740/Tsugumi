import CoreGraphics
import Foundation

/// Keeps the canvas readable while the Agent works: what a Card's code shows
/// (its output frame) must not cover other Cards, frames or notes.
///
/// A Card and the frame drawn after it (below it when the flow runs left to
/// right, beside it when it runs top to bottom) are one block, so a frame
/// always stays with its Card. Blocks that collide are pushed apart along
/// that same axis, in the order they already have: each block goes just
/// past the blocks before it that it would touch. Rows (or columns) keep
/// their order and only the gaps that are too small grow; nothing moves when
/// nothing overlaps.
enum LayoutTidy {
  /// A block: a Card with its frame ("card:<id>"), a frame placed on its own
  /// ("frame:<id>") or a note ("note:<id>"), in world units.
  struct Box: Equatable {
    let id: String
    var rect: CGRect
  }

  struct Move: Equatable {
    let id: String
    let by: CGVector
  }

  /// Space kept between blocks.
  static let gap: CGFloat = 24

  /// Pairs of blocks that overlap (closer than the gap).
  static func overlaps(_ boxes: [Box]) -> [(String, String)] {
    var out: [(String, String)] = []
    for i in boxes.indices {
      for j in boxes.indices where j > i && hit(boxes[i].rect, boxes[j].rect) {
        out.append((boxes[i].id, boxes[j].id))
      }
    }
    return out
  }

  static func hit(_ a: CGRect, _ b: CGRect) -> Bool {
    a.insetBy(dx: -gap / 2, dy: -gap / 2).intersects(b.insetBy(dx: -gap / 2, dy: -gap / 2))
  }

  static func applying(_ moves: [Move], to boxes: [Box]) -> [Box] {
    boxes.map { b in
      guard let m = moves.first(where: { $0.id == b.id }) else { return b }
      return Box(id: b.id, rect: b.rect.offsetBy(dx: m.by.dx, dy: m.by.dy))
    }
  }

  /// The moves that make room: along y when the flow runs left to right
  /// (`across`), along x when it runs top to bottom.
  static func makeRoom(_ boxes: [Box], across: Bool) -> [Move] {
    guard !overlaps(boxes).isEmpty else { return [] }
    // Along the axis: where a block starts and ends; across it: its span.
    func lo(_ r: CGRect) -> CGFloat { across ? r.minY : r.minX }
    func hi(_ r: CGRect) -> CGFloat { across ? r.maxY : r.maxX }
    // In the same column (row): their spans across the axis really overlap
    // (blocks side by side, however close, are left as they are).
    func spans(_ a: CGRect, _ b: CGRect) -> Bool {
      across ? a.minX < b.maxX && b.minX < a.maxX : a.minY < b.maxY && b.minY < a.maxY
    }
    let order = boxes.indices.sorted { (lo(boxes[$0].rect), across ? boxes[$0].rect.minX : boxes[$0].rect.minY, boxes[$0].id)
                                     < (lo(boxes[$1].rect), across ? boxes[$1].rect.minX : boxes[$1].rect.minY, boxes[$1].id) }
    var placed: [CGRect] = []
    var moves: [Move] = []
    for i in order {
      var r = boxes[i].rect
      // Just past every earlier block it would touch (again after each push,
      // since the push may bring it to another).
      var pushed = true
      while pushed {
        pushed = false
        for p in placed where spans(p, r) && hi(p) + gap > lo(r) && lo(p) < hi(r) + gap {
          let d = hi(p) + gap - lo(r)
          r = across ? r.offsetBy(dx: 0, dy: d) : r.offsetBy(dx: d, dy: 0)
          pushed = true
        }
      }
      placed.append(r)
      let by = CGVector(dx: r.minX - boxes[i].rect.minX, dy: r.minY - boxes[i].rect.minY)
      if by.dx != 0 || by.dy != 0 { moves.append(Move(id: boxes[i].id, by: by)) }
    }
    return moves
  }
}
