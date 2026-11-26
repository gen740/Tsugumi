set(_tsugumi_host_environment_sources
  agent/native_terminal_test.cc
  agent/native_environment_test.cc
  agent/web_pages_test.cc
)
if(NOT TSUGUMI_NATIVE_DARWIN_TESTS)
  list(APPEND _tsugumi_host_environment_sources
    agent/environment_adapter_test.cc
  )
endif()
