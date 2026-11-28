if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(tsugumi_remote_store STATIC src/nix_store.cc)
  target_compile_features(tsugumi_remote_store PRIVATE cxx_std_23)
  target_link_libraries(tsugumi_remote_store PRIVATE PkgConfig::NIX tsugumi_worker_values tsugumi_nix_store tsugumi_workerd_support)
  target_sources(tsugumi-workerd PRIVATE src/remote_main.cc src/jobs_command.cc
    src/invocation.cc src/dashboard.cc src/worker_settings.cc)
  include(cmake/dashboard.cmake)
  # The settings file's schema, which its FlatBuffers JSON parser reads.
  set(worker_settings_schema ${CMAKE_CURRENT_BINARY_DIR}/worker_settings_fbs.cc)
  add_custom_command(
    OUTPUT ${worker_settings_schema}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${TSUGUMI_PROTOCOL_DIR}/worker_settings.fbs
      -DOUTPUT=${worker_settings_schema} -DSYMBOL=tsugumi_worker_settings_fbs
      -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${TSUGUMI_PROTOCOL_DIR}/worker_settings.fbs ${PROJECT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)
  target_sources(tsugumi-workerd PRIVATE ${worker_settings_schema})
  target_compile_definitions(tsugumi-workerd PRIVATE _GNU_SOURCE
    TSUGUMI_WORKERD_VERSION="${PROJECT_VERSION}")
  target_link_libraries(tsugumi-workerd PRIVATE tsugumi_worker_values tsugumi_nix_store tsugumi_workerd_support tsugumi_remote_store)
endif()
