# The real core serves its public RPC. Only the external Nix evaluator is
# replaced: this target supplies both evaluator and store symbols.
add_executable(tsugumid-mock
  ${_core_main}
  doubles/nix_eval.cc)
set_property(TARGET tsugumid-mock PROPERTY TSUGUMI_MOCK_NIX ON)
target_include_directories(tsugumid-mock PRIVATE ${_core_headers}
  "$<TARGET_PROPERTY:tsugumi_nix_store,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(tsugumid-mock PRIVATE tsugumi_core)
add_dependencies(tsugumid-mock tsugumid tsugumi-worker tsugumi-workerd tsugumi-rpc)

if(NOT TSUGUMI_INSTALLED_CORE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME core_rpc
    COMMAND ${Python3_EXECUTABLE} -m pytest -q -m "not environment" ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(core_rpc PROPERTIES
    TIMEOUT 300
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    ENVIRONMENT "TSUGUMI_BUILD_DIR=${PROJECT_BINARY_DIR};TSUGUMI_PY_BINDINGS=${TSUGUMI_GEN_DIR}/python;TSUGUMI_MOCK_DAEMON=$<TARGET_FILE:tsugumid-mock>;TSUGUMID=$<TARGET_FILE:tsugumid>;TSUGUMI_WORKER=$<TARGET_FILE:tsugumi-worker>;TSUGUMI_WORKERD=$<TARGET_FILE:tsugumi-workerd>;TSUGUMI_RPC_CLIENT=$<TARGET_FILE:tsugumi-rpc>;TSUGUMI_PROJECT_SCHEMA=${_test_project_schema}")
endif()
