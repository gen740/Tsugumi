# Tree-sitter grammars without a Swift package

Used only by the app's editor for syntax colors (not by Cards or SDKs).

- TreeSitterNix: `src/` copied from github.com/nix-community/tree-sitter-nix
  at e49c2f317e896eb08967717e758a46d6eea038b2 (MIT, LICENSE).
- TreeSitterPython: `src/` copied from github.com/tree-sitter/tree-sitter-python
  v0.25.0 (MIT, LICENSE).
- TreeSitterJuliaScanner: only `src/scanner.c` (and the `parser.h` it includes)
  of github.com/tree-sitter/tree-sitter-julia v0.25.0 (MIT, LICENSE). The
  grammar itself is the SwiftPM dependency.
- TreeSitterFBS: FlatBuffers schemas, written here (`grammar.js`). After
  changing it, regenerate `src/` in that folder with
  `nix shell nixpkgs#tree-sitter nixpkgs#nodejs -c tree-sitter generate`.

For FBS, `src/parser.c`, `src/grammar.json` and `src/node-types.json` are
generated from that Tsugumi grammar. The `src/tree_sitter/` support headers
in the grammar folders come from Tree-sitter, not from our grammar; they
retain Tree-sitter's MIT notice in [LICENSE.tree-sitter](LICENSE.tree-sitter).
The copied Nix and Python parsers/scanners keep the licenses in their own
folders, regardless of whether parts were generated upstream.

Highlight queries are in `Sources/App/Resources/TreeSitter/`.
Their [source notes](../App/Resources/TreeSitter/SOURCES.md) and
[license copies](../../../third-party/tree-sitter/README.md) apply even
though the queries are stored separately from the grammar packages.
