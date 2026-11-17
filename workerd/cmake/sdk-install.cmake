# Cross-system Cards rebuild this light worker with their own nixpkgs/libc.
install(FILES src/native_main.cc DESTINATION share/tsugumi/sdk/worker/src)
install(FILES ${PROJECT_SOURCE_DIR}/core/src/schema.hh ${PROJECT_SOURCE_DIR}/core/src/schema.cc
  DESTINATION share/tsugumi/sdk/worker/core)
install(FILES ${PROJECT_SOURCE_DIR}/core/src/util/text.hh ${PROJECT_SOURCE_DIR}/core/src/util/text.cc
  DESTINATION share/tsugumi/sdk/worker/core/util)
install(FILES ${TSUGUMI_PROTOCOL_DIR}/worker.fbs ${TSUGUMI_PROTOCOL_DIR}/data.fbs
  ${TSUGUMI_PROTOCOL_DIR}/tsugumi_enums.fbs DESTINATION share/tsugumi/sdk/worker/protocol)
install(DIRECTORY ${HOSQ_INCLUDE_DIR}/hosq DESTINATION share/tsugumi/sdk/worker/include)
install(FILES ${HOSQ_INCLUDE_DIR}/../LICENSE DESTINATION share/tsugumi/sdk/worker/include/hosq RENAME LICENSE OPTIONAL)

install(FILES include/tsugumi/workerd/frame.hh include/tsugumi/workerd/display.hh DESTINATION share/tsugumi/sdk/worker/include/tsugumi/workerd)
