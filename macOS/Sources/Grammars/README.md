# Tree-sitter grammars without a Swift package

Used only by the app's editor for syntax colors (not by Cards or SDKs).

- TreeSitterNix: `src/` copied from github.com/nix-community/tree-sitter-nix
  at e49c2f317e896eb08967717e758a46d6eea038b2 (MIT, LICENSE).
- TreeSitterPython: `src/` copied from github.com/tree-sitter/tree-sitter-python
  v0.25.0 (MIT, LICENSE); its Swift package does not build its scanner when
  used as a dependency.
- TreeSitterFBS: FlatBuffers schemas, written here (`grammar.js`). After
  changing it, regenerate `src/` in that folder with
  `nix shell nixpkgs#tree-sitter nixpkgs#nodejs -c tree-sitter generate`.

Highlight queries are in `Sources/App/Resources/TreeSitter/`.
