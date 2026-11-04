import Testing
@testable import TsugumiApp

@Test func everyTableRowCanBeReachedExactlyOnce() {
  for rows in [0, 1, 200, 201, 2001, 10_000] {
    let pages = TablePage(count: rows, requested: 0).count
    let visited = (0..<pages).flatMap { p -> [Int] in
      let page = TablePage(count: rows, requested: p)
      return Array(page.start..<page.end)
    }
    #expect(visited == Array(0..<rows))
    let shrunk = TablePage(count: 1, requested: pages - 1)
    #expect(shrunk.start == 0 && shrunk.end == 1)
  }
}
