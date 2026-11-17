#pragma once
// The external scanner of tree-sitter-julia (src/scanner.c, unchanged). The
// grammar's parser.c comes from its own package; its manifest leaves this file out.
void *tree_sitter_julia_external_scanner_create(void);
