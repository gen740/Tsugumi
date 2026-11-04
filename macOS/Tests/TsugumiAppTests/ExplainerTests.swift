import Foundation
import Testing
@testable import TsugumiApp

/// Help > Tsugumi を知る reads the repository's documents as they are and
/// sets its figures after phrases in them: an edit to README.md or docs/*.md
/// must not drop a section or a figure without anyone noticing.
@Test func everyDocumentSectionAndFigureIsFound() {
  for doc in Explainer.Docs.all {
    #expect(doc.missing.isEmpty, "\(doc.source): not found: \(doc.missing)")
  }
  let figures = Explainer.Docs.all.map { d in d.blocks.filter { if case .figure = $0 { return true }; return false }.count }
  #expect(figures == [4, 0, 1])
}

/// The Markdown the documents use: soft line breaks join Japanese lines
/// directly, and a closing ** at a line's end still closes (as on GitHub).
@Test func documentMarkdownJoinsLinesLikeGitHub() {
  let blocks = Explainer.MarkdownText.parse("""
    ## 見出し

    一行目。
    二行目。
    **強調。**
    続き。

    1. **一つ目。** 説明
    2. 二つ目

    | a | b |
    | --- | --- |
    | 1 | 2 |
    """)
  #expect(blocks.count == 4)
  if case .paragraph(let s) = blocks[1] { #expect(s == "一行目。二行目。**強調。** 続き。") } else { Issue.record("not a paragraph") }
  if case .list(let items, let ordered) = blocks[2] { #expect(ordered && items == ["**一つ目。** 説明", "二つ目"]) } else { Issue.record("not a list") }
  if case .table(let rows) = blocks[3] { #expect(rows == [["a", "b"], ["1", "2"]]) } else { Issue.record("not a table") }
}
