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
  set(_core_main ${PROJECT_SOURCE_DIR}/core/src/main.cc)
  set(_core_headers ${PROJECT_SOURCE_DIR}/core/src)
  set(_test_mock "$<TARGET_FILE:tsugumid-mock>")
  set(_test_sdk ${PROJECT_SOURCE_DIR}/sdk)
  set(_test_project_schema ${TSUGUMI_GEN_DIR}/bfbs/tsugumi.bfbs)
endif()

# Also cover the daemon doubles when this directory builds against ci-core.
add_compile_options(-Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough
  -Wmissing-designated-field-initializers)

include(cmake/daemon-double.cmake)

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
include(cmake/document.cmake)

include(cmake/host-rpc.cmake)

include(cmake/client.cmake)

include(cmake/remote-result.cmake)

include(cmake/worker.cmake)

include(cmake/tls.cmake)

include(cmake/host.cmake)

include(cmake/host-edit-driver.cmake)

include(cmake/execution.cmake)

include(cmake/nix.cmake)

include(cmake/vm.cmake)
