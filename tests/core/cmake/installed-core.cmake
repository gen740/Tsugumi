# Build test executables against the exact production archives. Private core
# headers are used only by the external evaluator double, never by RPC tests.
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
    "${TSUGUMI_CORE_TEST_SUPPORT}/src/main.cc"
    "${HOSQ_INCLUDE_DIR}/hosq/hosq.hh")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "Missing installed core test input: ${required}")
  endif()
endforeach()

find_package(flatbuffers CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
add_library(hosq INTERFACE)
target_include_directories(hosq SYSTEM INTERFACE ${HOSQ_INCLUDE_DIR})

add_library(tsugumi_json STATIC IMPORTED)
set_target_properties(tsugumi_json PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_json.a")
target_link_libraries(tsugumi_json INTERFACE hosq)

add_library(tsugumi_core STATIC IMPORTED)
set_target_properties(tsugumi_core PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_TEST_SUPPORT}/lib/libtsugumi_core.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_core INTERFACE tsugumi_json flatbuffers::flatbuffers
  PkgConfig::SQLITE3 PkgConfig::LIBGIT2 Threads::Threads)
if(NOT APPLE)
  pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)
  target_link_libraries(tsugumi_core INTERFACE PkgConfig::CRYPTO)
endif()

add_library(tsugumi_client STATIC IMPORTED)
set_target_properties(tsugumi_client PROPERTIES
  IMPORTED_LOCATION "${TSUGUMI_CORE_ROOT}/lib/libtsugumi_client.a"
  INTERFACE_INCLUDE_DIRECTORIES "${TSUGUMI_CORE_ROOT}/include;${TSUGUMI_CORE_TEST_SUPPORT}/include")
target_link_libraries(tsugumi_client INTERFACE flatbuffers::flatbuffers CURL::libcurl Threads::Threads)
foreach(program tsugumid tsugumi-agentd tsugumi-worker tsugumi-rpc)
  add_executable(${program} IMPORTED)
  set_target_properties(${program} PROPERTIES
    IMPORTED_LOCATION "${TSUGUMI_CORE_ROOT}/bin/${program}")
endforeach()

set(_core_main ${TSUGUMI_CORE_TEST_SUPPORT}/src/main.cc)
set(_core_headers ${TSUGUMI_CORE_TEST_SUPPORT}/include/tsugumi-core)
set(_test_mock ${CMAKE_INSTALL_PREFIX}/libexec/tsugumi-tests/tsugumid-mock)
set(_test_sdk ${TSUGUMI_CORE_ROOT}/share/tsugumi/sdk)
set(_test_agent_schema ${TSUGUMI_CORE_ROOT}/share/tsugumi/protocol/agent.bfbs)
set(_test_project_schema ${TSUGUMI_CORE_ROOT}/share/tsugumi/protocol/tsugumi.bfbs)
enable_testing()
