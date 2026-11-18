import Foundation
import Testing

@testable import TsugumiApp

/// Go, Rust, Julia and R (grammar + bundled highlights.scm). `grammar()` hides a
/// grammar or query that fails to load behind nil, so each language is shown to
/// color real source: a token is "kind:text".
private func tokens(_ text: String, _ lang: String) -> Set<String> {
  let spans = TreeSitterHighlighter.spans(text, language: lang) ?? []
  return Set(spans.map { r, t in "\(t):" + (text as NSString).substring(with: r) })
}


@Test func treeSitterNamesTheNewLanguages() {
  for (name, grammar) in [("go", "go"), ("rs", "rust"), ("rust", "rust"), ("jl", "julia"), ("julia", "julia"), ("r", "r"), ("R", "r"), ("Go", "go")] {
    #expect(TreeSitterHighlighter.grammarName(name) == grammar, "\(name)")
  }
}

@Test func treeSitterColorsGo() {
  let t = tokens("// note\npackage main\n\nimport \"fmt\"\n\ntype Point struct{ X int }\n\nfunc main() {\n\tfmt.Println(\"hi\", 42, true)\n\tfor i := 0; i < 3; i++ {\n\t}\n}\n", "go")
  #expect(t.contains("comment:// note") && t.contains("keyword:package") && t.contains("keyword:func") && t.contains("keyword:struct"))
  #expect(t.contains("string:\"hi\"") && t.contains("number:42") && t.contains("type:Point") && t.contains("property:X"))
  #expect(t.contains("function:Println") && t.contains("keyword:for"))
}

@Test func treeSitterColorsRust() {
  let t = tokens("// note\nuse std::fmt;\n\n#[derive(Debug)]\nstruct Point { x: i32 }\n\nfn main() -> Result<(), String> {\n    let s = \"hi\";\n    println!(\"{}\", 42);\n    Ok(())\n}\n", "rs")
  #expect(t.contains("comment:// note") && t.contains("keyword:use") && t.contains("keyword:struct") && t.contains("keyword:fn") && t.contains("keyword:let"))
  #expect(t.contains("string:\"hi\"") && t.contains("type:Point") && t.contains("property:x") && t.contains("type:i32"))
  // Rust's query calls its number literals constant.builtin, so they color as constants.
  #expect(t.contains("constant:42") && t.contains("function:main") && t.contains("function:println") && t.contains("property:#[derive(Debug)]"))
}

@Test func treeSitterColorsJulia() {
  let t = tokens("# note\nusing Printf\n\nfunction main(inputs, report)\n    x = 1.5\n    if x > 1 && true\n        @printf(\"%d\", 2)\n    end\n    return Dict{String,Any}(\"result\" => Dict(\"value\" => x, \"tag\" => :sym))\nend\n", "jl")
  #expect(t.contains("comment:# note") && t.contains("keyword:using") && t.contains("keyword:function") && t.contains("keyword:end"))
  #expect(t.contains("keyword:if") && t.contains("keyword:return") && t.contains("number:1.5") && t.contains("constant:true"))
  #expect(t.contains("string:\"%d\"") && t.contains("function:main") && t.contains("type:Dict"))
}

@Test func treeSitterColorsR() {
  let t = tokens("# note\nlibrary(ggplot2)\nmain <- function(inputs, report) {\n  x <- c(1, 2.5, TRUE)\n  if (x[1] > 1) print(\"hi\") else NULL\n  for (i in 1:3) next\n}\n", "R")
  #expect(t.contains("comment:# note") && t.contains("keyword:function") && t.contains("keyword:if") && t.contains("keyword:else") && t.contains("keyword:for"))
  #expect(t.contains("string:\"hi\"") && t.contains("number:2.5") && t.contains("function:library") && t.contains("function:print"))
  #expect(t.contains("constant:TRUE") && t.contains("constant:NULL"))
}
