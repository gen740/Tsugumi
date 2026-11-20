# Cross-system Cards rebuild this light worker with their own nixpkgs/libc.
install(FILES src/native_main.cc DESTINATION share/tsugumi/sdk/worker/src)
install(FILES ${PROJECT_SOURCE_DIR}/util/src/json.cc ${PROJECT_SOURCE_DIR}/util/src/text.cc DESTINATION share/tsugumi/sdk/worker/util/src)
install(FILES ${PROJECT_SOURCE_DIR}/util/include/tsugumi/util/flatbuffer_schema.hh ${PROJECT_SOURCE_DIR}/util/include/tsugumi/util/json.hh ${PROJECT_SOURCE_DIR}/util/include/tsugumi/util/json_object.hh ${PROJECT_SOURCE_DIR}/util/include/tsugumi/util/text.hh DESTINATION share/tsugumi/sdk/worker/util/include/tsugumi/util)
install(FILES ${TSUGUMI_PROTOCOL_DIR}/worker.fbs ${TSUGUMI_PROTOCOL_DIR}/data.fbs
  ${TSUGUMI_PROTOCOL_DIR}/tsugumi_enums.fbs DESTINATION share/tsugumi/sdk/worker/protocol)
install(DIRECTORY ${HOSQ_INCLUDE_DIR}/hosq DESTINATION share/tsugumi/sdk/worker/include
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hh")

install(FILES include/tsugumi/workerd/frame.hh include/tsugumi/workerd/display.hh DESTINATION share/tsugumi/sdk/worker/include/tsugumi/workerd)
