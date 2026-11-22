# Build test executables against the exact production archives. Private core
# headers are used by the evaluator double and socket-free unit tests,
# never by RPC tests.
set(TSUGUMI_INSTALLED_CORE ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(TSUGUMI_CORE_ROOT "" CACHE PATH "Installed production core")
set(TSUGUMI_CORE_TEST_SUPPORT "" CACHE PATH "Installed production test build inputs")
set(HOSQ_INCLUDE_DIR "" CACHE PATH "hosq include directory")
foreach(required
    "${TSUGUMI_CORE_ROOT}/bin/tsugumid"
    "${TSUGUMI_CORE_ROOT}/lib/libtsugumi_client.a"
    "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_core.a"
    "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_host_core.a"
    "${TSUGUMI_CORE_TEST_SUPPORT}/src/main.cc"
    "${HOSQ_INCLUDE_DIR}/hosq/hosq.hh")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "Missing installed core test input: ${required}")
  endif()
endforeach()

find_package(flatbuffers CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL 3 REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
pkg_check_modules(NIX REQUIRED IMPORTED_TARGET nix-expr nix-store nix-fetchers nix-flake nix-util)
add_library(hosq INTERFACE)
target_include_directories(hosq SYSTEM INTERFACE ${HOSQ_INCLUDE_DIR})

include(${CMAKE_CURRENT_LIST_DIR}/installed/json.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/installed/worker.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/installed/tls.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/installed/document.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/installed/nix.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/installed/daemon.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/installed/client.cmake)


include(${CMAKE_CURRENT_LIST_DIR}/installed/host.cmake)

foreach(program tsugumid tsugumi-hostd tsugumi-worker tsugumi-workerd tsugumi-rpc)
  add_executable(${program} IMPORTED)
  set_target_properties(${program} PROPERTIES
    IMPORTED_LOCATION "${TSUGUMI_CORE_ROOT}/bin/${program}")
endforeach()

set(_project_endpoint_source ${TSUGUMI_CORE_TEST_SUPPORT}/src/project_endpoint.cc ${TSUGUMI_CORE_TEST_SUPPORT}/src/server.cc)
set(_core_main ${TSUGUMI_CORE_TEST_SUPPORT}/src/main.cc)
set(_core_headers ${TSUGUMI_CORE_TEST_SUPPORT}/include/tsugumi-private)
set(_test_sdk ${TSUGUMI_CORE_ROOT}/share/tsugumi/sdk)
set(_test_project_schema ${TSUGUMI_CORE_ROOT}/share/tsugumi/protocol/tsugumi.bfbs)
enable_testing()

include_directories(${_core_headers})
