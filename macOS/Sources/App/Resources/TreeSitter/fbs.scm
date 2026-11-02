(comment) @comment
(string) @string
(number) @number
(boolean) @constant.builtin
["include" "namespace" "attribute" "table" "struct" "enum" "union" "root_type"
 "file_identifier" "file_extension" "rpc_service" "null"] @keyword
(primitive_type) @type.builtin
(table_decl name: (identifier) @type)
(struct_decl name: (identifier) @type)
(enum_decl name: (identifier) @type)
(union_decl name: (identifier) @type)
(rpc_decl name: (identifier) @type)
(field_decl name: (identifier) @property)
(enum_value name: (identifier) @constant)
(metadata key: (identifier) @attribute)
