# Tree-sitter notices retained for copied code

These are unmodified upstream license texts for the language highlight
queries copied into `macOS/Sources/App/Resources/TreeSitter/`. Grammar
versions and query provenance are recorded in
[SOURCES.md](../../macOS/Sources/App/Resources/TreeSitter/SOURCES.md).

| Code / queries | Upstream version | License text in this repository |
| --- | --- | --- |
| C; C part of C++ query | tree-sitter-c 0.23.6 (`ec69f91b23dd8630d68710b911d77127146ff7ef`) | [LICENSE](tree-sitter-c/LICENSE) |
| C++ | tree-sitter-cpp 0.23.4 (`f41e1a044c8a84ea9fa8577fdd2eab92ec96de02`) | [LICENSE](tree-sitter-cpp/LICENSE) |
| JSON | tree-sitter-json 0.24.8 (`ee35a6ebefcef0c5c416c0d1ccec7370cfca5a24`) | [LICENSE](tree-sitter-json/LICENSE) |
| Bash | tree-sitter-bash 0.25.1 (`a06c2e4415e9bc0346c6b86d401879ffb44058f7`) | [LICENSE](tree-sitter-bash/LICENSE) |
| Go | tree-sitter-go 0.25.0 (`1547678a9da59885853f5f5cc8a99cc203fa2e2c`) | [LICENSE](tree-sitter-go/LICENSE) |
| Rust | tree-sitter-rust 0.24.2 (`77a3747266f4d621d0757825e6b11edcbf991ca5`) | [LICENSE](tree-sitter-rust/LICENSE) |
| Julia | tree-sitter-julia 0.25.0 (`e0f9dcd180fdcfcfa8d79a3531e11d99e79321d3`) | [LICENSE](tree-sitter-julia/LICENSE) |
| Julia external scanner | tree-sitter-julia 0.25.0 (`e0f9dcd180fdcfcfa8d79a3531e11d99e79321d3`) | [LICENSE](../../macOS/Sources/Grammars/TreeSitterJuliaScanner/LICENSE) |
| R | tree-sitter-r 1.3.0 (`346d3707b8c9301f1051e8f6e32666e67529f7d2`) | [LICENSE](tree-sitter-r/LICENSE) |
| Nix parser / scanner / query | tree-sitter-nix `e49c2f317e896eb08967717e758a46d6eea038b2` | [LICENSE](../../macOS/Sources/Grammars/TreeSitterNix/LICENSE) |
| Python parser / scanner / query | tree-sitter-python 0.25.0 | [LICENSE](../../macOS/Sources/Grammars/TreeSitterPython/LICENSE) |
| Tree-sitter support headers | Tree-sitter; notice copied from runtime 0.25.10 (`da6fe9beb4f7f67beb75914ca8e0d48ae48d6406`) | [LICENSE.tree-sitter](../../macOS/Sources/Grammars/LICENSE.tree-sitter) |

The support-header notice identifies the upstream project; it does not
claim that all vendored grammars were regenerated with CLI 0.25.10.
The original generator version is not recorded for every vendored file.

These files preserve notices in the source repository. The app carries the
same texts, taken from the checkouts it was built from, in
`Contents/Resources/licenses/` ([bundle-licenses.sh](../../macOS/tools/bundle-licenses.sh)).

When updating a grammar or its queries, refresh its license from that same
upstream revision and preserve copyright holders verbatim. Do not substitute
Tsugumi's own MIT notice for an upstream notice.
