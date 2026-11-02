// FlatBuffers schema (.fbs) grammar for syntax highlighting in Tsugumi's
// editor. Regenerate src/ with: tree-sitter generate (see README.md here).
function commaSep(rule) {
  return optional(seq(rule, repeat(seq(',', rule)), optional(',')));
}

module.exports = grammar({
  name: 'fbs',
  extras: $ => [/\s/, $.comment],
  word: $ => $.identifier,
  rules: {
    source_file: $ => repeat($._declaration),
    _declaration: $ => choice($.include, $.namespace_decl, $.attribute_decl, $.table_decl, $.struct_decl,
      $.enum_decl, $.union_decl, $.root_type_decl, $.file_identifier_decl, $.file_extension_decl, $.rpc_decl),
    include: $ => seq('include', $.string, ';'),
    namespace_decl: $ => seq('namespace', $.qualified, ';'),
    attribute_decl: $ => seq('attribute', choice($.string, $.identifier), ';'),
    table_decl: $ => seq('table', field('name', $.identifier), optional($.metadata), '{', repeat($.field_decl), '}'),
    struct_decl: $ => seq('struct', field('name', $.identifier), optional($.metadata), '{', repeat($.field_decl), '}'),
    field_decl: $ => seq(field('name', $.identifier), ':', $.type, optional(seq('=', $._scalar)), optional($.metadata), ';'),
    enum_decl: $ => seq('enum', field('name', $.identifier), ':', $.type, optional($.metadata), '{', commaSep($.enum_value), '}'),
    union_decl: $ => seq('union', field('name', $.identifier), optional($.metadata), '{', commaSep($.union_value), '}'),
    enum_value: $ => seq(field('name', $.identifier), optional(seq('=', $.number))),
    union_value: $ => seq(optional(seq(field('label', $.identifier), ':')), $.qualified, optional(seq('=', $.number))),
    root_type_decl: $ => seq('root_type', $.qualified, ';'),
    file_identifier_decl: $ => seq('file_identifier', $.string, ';'),
    file_extension_decl: $ => seq('file_extension', $.string, ';'),
    rpc_decl: $ => seq('rpc_service', field('name', $.identifier), '{',
      repeat(seq($.identifier, '(', $.qualified, ')', ':', $.qualified, optional($.metadata), ';')), '}'),
    type: $ => choice(seq('[', $.type, optional(seq(':', $.number)), ']'), $.primitive_type, $.qualified),
    primitive_type: _ => choice('bool', 'byte', 'ubyte', 'short', 'ushort', 'int', 'uint', 'float', 'long', 'ulong',
      'double', 'int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64', 'float32', 'float64', 'string'),
    metadata: $ => seq('(', commaSep(seq(field('key', $.identifier), optional(seq(':', $._scalar)))), ')'),
    _scalar: $ => choice($.number, $.boolean, $.string, $.identifier, 'null'),
    boolean: _ => choice('true', 'false'),
    qualified: $ => prec.left(seq($.identifier, repeat(seq('.', $.identifier)))),
    identifier: _ => /[A-Za-z_][A-Za-z0-9_]*/,
    number: _ => /[-+]?(0x[0-9a-fA-F]+|[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?|inf|nan)/,
    string: _ => /"([^"\\]|\\.)*"/,
    comment: _ => token(choice(seq('//', /.*/), seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'))),
  },
});
