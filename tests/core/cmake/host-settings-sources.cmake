set(_tsugumi_host_settings_sources
  host/binary_caches_test.cc
)
if(NOT TSUGUMI_NATIVE_DARWIN_TESTS)
  list(APPEND _tsugumi_host_settings_sources
    agent/model_catalog_test.cc
    agent/settings_store_test.cc
  )
endif()
