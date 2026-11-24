if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.28)
  if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "")
  endif()
  project(TsugumiCoreTests LANGUAGES CXX)
  include(cmake/installed-core.cmake)
else()
  set(_local_service_source ${PROJECT_SOURCE_DIR}/workerd/src/local_service.cc)
  set(_workerd_headers ${PROJECT_SOURCE_DIR}/workerd/src)
  set(_project_endpoint_source ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/project_endpoint.cc ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/server.cc)
  set(_core_main ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src/main.cc)
  set(_core_headers ${PROJECT_SOURCE_DIR}/tsugumid/runtime/src ${PROJECT_SOURCE_DIR}/hostd/private ${PROJECT_SOURCE_DIR}/hostd/agent/private)
  include_directories(${_core_headers})
  set(_test_mock "$<TARGET_FILE:tsugumid-mock>")
  set(_test_nix ${PROJECT_BINARY_DIR}/share/tsugumi/nix)
  set(_test_project_schema ${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs)
endif()

add_compile_options(-Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough
  -Wmissing-designated-field-initializers)

function(tsugumi_use_nix_double target)
  set_property(TARGET ${target} PROPERTY TSUGUMI_MOCK_NIX ON)
  target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/doubles/nix_eval.cc"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../tsugumid/runtime/src/nix_params.cc")
  target_compile_features(${target} PRIVATE cxx_std_23)
  target_link_libraries(${target} PRIVATE PkgConfig::NIX)
  target_include_directories(${target} PRIVATE
    "$<TARGET_PROPERTY:tsugumi_nix_store,INTERFACE_INCLUDE_DIRECTORIES>")
endfunction()

if(TSUGUMI_INSTALLED_CORE)
  # These fixtures have consumers outside the native RPC suites.
  if(TSUGUMI_TEST_TARGET STREQUAL "tsugumid-mock")
    include(cmake/daemon-double.cmake)
    install(TARGETS tsugumid-mock RUNTIME DESTINATION libexec/tsugumi-tests)
    return()
  elseif(TSUGUMI_TEST_TARGET STREQUAL "tsugumi_document_edit_driver")
    include(cmake/host-edit-driver.cmake)
    return()
  elseif(TSUGUMI_TEST_TARGET STREQUAL "tsugumi_vm_lifetime_tests")
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
    include(cmake/vm.cmake)
    return()
  endif()
  add_executable(tsugumid-mock IMPORTED)
  set_target_properties(tsugumid-mock PROPERTIES IMPORTED_LOCATION "${TSUGUMI_TEST_MOCK}")
  set(_test_mock "${TSUGUMI_TEST_MOCK}")
else()
  include(cmake/daemon-double.cmake)
endif()

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
if(NOT TSUGUMI_INSTALLED_CORE)
  include(cmake/host-edit-driver.cmake)
  include(cmake/vm.cmake)
endif()
if(TSUGUMI_TEST_TARGET STREQUAL "tsugumi_arm_shell_tests")
  include(cmake/host-rpc.cmake)
  return()
endif()
if(NOT TSUGUMI_NATIVE_DARWIN_TESTS)
  include(cmake/input-types.cmake)
endif()

include(cmake/host-rpc.cmake)

include(cmake/client.cmake)

include(cmake/remote-result.cmake)

include(cmake/worker.cmake)

include(cmake/tls.cmake)

include(cmake/host.cmake)

include(cmake/execution.cmake)

include(cmake/nix.cmake)
