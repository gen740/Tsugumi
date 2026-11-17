/// A bounded slice of a table. Clamp the page when a refreshed result shrinks.
struct TablePage {
  static let size = 200
  let index: Int, count: Int, start: Int, end: Int
  init(count rows: Int, requested: Int) {
    count = max(1, (rows + Self.size - 1) / Self.size)
    index = min(max(0, requested), count - 1)
    start = index * Self.size
    end = min(rows, start + Self.size)
  }
}
