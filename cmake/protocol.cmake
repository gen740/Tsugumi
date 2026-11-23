set(TSUGUMI_PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocol)
set(TSUGUMI_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(TSUGUMI_SCHEMAS card_enums artifact tsugumi_enums rpc_trace worker_types data card worker local_worker remote_worker tsugumi service_common agent host vm_host vm_guest runtime_state nix_evaluator version_state)
# Project document schemas. Their fields keep the documents' camelCase keys,
# which flatc warns about.
set(TSUGUMI_DOCUMENT_SCHEMAS project_document project_lock card_document comments_document layout_document)
list(APPEND TSUGUMI_SCHEMAS ${TSUGUMI_DOCUMENT_SCHEMAS})

function(schema_inputs schema result)
  set(source ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
  set(inputs ${source})
  file(STRINGS ${source} includes REGEX "^include ")
  foreach(include IN LISTS includes)
    string(REGEX REPLACE "^include \"([^\"]+)\\.fbs\";" "\\1" dependency "${include}")
    schema_inputs(${dependency} nested)
    list(APPEND inputs ${nested})
  endforeach()
  list(REMOVE_DUPLICATES inputs)
  set(${result} ${inputs} PARENT_SCOPE)
endfunction()

# Bindings are generated with the standard flatc into the build tree only.
set(_gen_cpp "")
set(_gen_bfbs "")
foreach(schema ${TSUGUMI_SCHEMAS})
  set(_src ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs)
  schema_inputs(${schema} _inputs)
  list(APPEND _gen_cpp ${TSUGUMI_GEN_DIR}/cpp/tsugumi/protocol/${schema}_generated.h)
  set(_quiet "")
  if(schema IN_LIST TSUGUMI_DOCUMENT_SCHEMAS)
    set(_quiet --no-warnings)
  endif()
  add_custom_command(
    OUTPUT ${TSUGUMI_GEN_DIR}/cpp/tsugumi/protocol/${schema}_generated.h
    COMMAND ${TSUGUMI_FLATC} ${_quiet} --cpp --scoped-enums --gen-mutable --gen-object-api
            --keep-prefix --include-prefix tsugumi/protocol
            -I ${TSUGUMI_PROTOCOL_DIR} -o ${TSUGUMI_GEN_DIR}/cpp/tsugumi/protocol ${_src}
    COMMAND ${TSUGUMI_FLATC} ${_quiet} --python --gen-object-api --python-typing -I ${TSUGUMI_PROTOCOL_DIR}
            -o ${TSUGUMI_GEN_DIR}/python ${_src}
    DEPENDS ${_inputs}
    COMMENT "flatc ${schema}.fbs"
    VERBATIM)
endforeach()
foreach(schema tsugumi agent host worker local_worker remote_worker vm_host)
  schema_inputs(${schema} _inputs)
  list(APPEND _gen_bfbs ${TSUGUMI_GEN_DIR}/bfbs/${schema}.bfbs)
  add_custom_command(
    OUTPUT ${TSUGUMI_GEN_DIR}/bfbs/${schema}.bfbs
    COMMAND ${TSUGUMI_FLATC} -b --schema --bfbs-comments --bfbs-builtins
            -I ${TSUGUMI_PROTOCOL_DIR} -o ${TSUGUMI_GEN_DIR}/bfbs
            ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs
    DEPENDS ${_inputs}
    VERBATIM)
endforeach()
add_custom_target(tsugumi_protocol_gen DEPENDS ${_gen_cpp} ${_gen_bfbs})

add_library(tsugumi_protocol INTERFACE)
add_dependencies(tsugumi_protocol tsugumi_protocol_gen)
target_include_directories(tsugumi_protocol INTERFACE ${TSUGUMI_GEN_DIR}/cpp ${TSUGUMI_PROTOCOL_DIR}/include)
