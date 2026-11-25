set(_tsugumi_host_rpc_host_sources
  agent/conversation_store_rpc_test.cc
  agent/rpc_test.cc
  agent/host_rpc_test.cc
  agent/view_rpc_test.cc
  agent/skills_rpc_test.cc
  agent/project_rpc_test.cc
  agent/exclude_rpc_test.cc
  agent_fixture_server.cc
)
if(NOT TSUGUMI_NATIVE_DARWIN_TESTS)
  list(APPEND _tsugumi_host_rpc_host_sources
    agent/models_rpc_test.cc
    agent/guide_rpc_test.cc
    agent/run_wait_rpc_test.cc
    agent/save_rpc_test.cc
    agent/layout_rpc_test.cc
    agent/edit_rpc_test.cc
    agent/demo_draft_save_rpc_test.cc
  )
endif()
