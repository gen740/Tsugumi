set(TSUGUMI_PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocol)
set(TSUGUMI_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(TSUGUMI_SCHEMAS tsugumi_enums data worker local_worker remote_worker tsugumi service_common agent host vm_host)

# Bindings are generated with the standard flatc into the build tree only.
set(_gen_cpp "")
set(_gen_bfbs "")
foreach(schema ${TSUGUMI_SCHEMAS})
  set(_src ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs)
  list(APPEND _gen_cpp ${TSUGUMI_GEN_DIR}/cpp/tsugumi/protocol/${schema}_generated.h)
  add_custom_command(
    OUTPUT ${TSUGUMI_GEN_DIR}/cpp/tsugumi/protocol/${schema}_generated.h
    COMMAND ${TSUGUMI_FLATC} --cpp --scoped-enums --gen-mutable --gen-object-api
            --keep-prefix --include-prefix tsugumi/protocol
            -I ${TSUGUMI_PROTOCOL_DIR} -o ${TSUGUMI_GEN_DIR}/cpp/tsugumi/protocol ${_src}
    COMMAND ${TSUGUMI_FLATC} --python --gen-object-api --python-typing -I ${TSUGUMI_PROTOCOL_DIR}
            -o ${TSUGUMI_GEN_DIR}/python ${_src}
    COMMAND ${TSUGUMI_FLATC} --swift --gen-object-api -I ${TSUGUMI_PROTOCOL_DIR}
            -o ${TSUGUMI_GEN_DIR}/swift ${_src}
    DEPENDS ${_src} ${TSUGUMI_PROTOCOL_DIR}/service_common.fbs ${TSUGUMI_PROTOCOL_DIR}/host.fbs ${TSUGUMI_PROTOCOL_DIR}/data.fbs ${TSUGUMI_PROTOCOL_DIR}/tsugumi_enums.fbs
    COMMENT "flatc ${schema}.fbs"
    VERBATIM)
endforeach()
foreach(schema worker local_worker remote_worker tsugumi agent host vm_host)
  list(APPEND _gen_bfbs ${TSUGUMI_GEN_DIR}/bfbs/${schema}.bfbs)
  add_custom_command(
    OUTPUT ${TSUGUMI_GEN_DIR}/bfbs/${schema}.bfbs
    COMMAND ${TSUGUMI_FLATC} -b --schema --bfbs-comments --bfbs-builtins
            -I ${TSUGUMI_PROTOCOL_DIR} -o ${TSUGUMI_GEN_DIR}/bfbs
            ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs
    DEPENDS ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs ${TSUGUMI_PROTOCOL_DIR}/service_common.fbs ${TSUGUMI_PROTOCOL_DIR}/host.fbs ${TSUGUMI_PROTOCOL_DIR}/data.fbs
            ${TSUGUMI_PROTOCOL_DIR}/tsugumi_enums.fbs
    VERBATIM)
endforeach()
add_custom_target(tsugumi_protocol_gen DEPENDS ${_gen_cpp} ${_gen_bfbs})

add_library(tsugumi_protocol INTERFACE)
add_dependencies(tsugumi_protocol tsugumi_protocol_gen)
target_include_directories(tsugumi_protocol INTERFACE ${TSUGUMI_GEN_DIR}/cpp)
