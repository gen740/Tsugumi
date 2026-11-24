set(_tsugumi_remote_result_daemon_sources
  project_document_test.cc
  shell_test.cc
)
if(NOT TSUGUMI_NATIVE_DARWIN_TESTS)
  list(APPEND _tsugumi_remote_result_daemon_sources
    collect_value_test.cc
    history_test.cc
    metadata_test.cc
    layout_boundary_test.cc
    remote_execution_test.cc
    remote_lifetime_test.cc
    host_requirements_test.cc
  )
endif()
