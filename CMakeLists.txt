cmake_minimum_required(VERSION 3.28)
if(APPLE)
  # The app (Virtualization.framework, SwiftUI) requires macOS 26 or later.
  set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "")
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TSUGUMI_VERSION LIMIT_COUNT 1)
project(Tsugumi VERSION ${TSUGUMI_VERSION} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TSUGUMI_BUILD_CORE "Build tsugumid and the Card worker" ON)
option(TSUGUMI_BUILD_TESTS "Build core RPC tests" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

find_program(TSUGUMI_FLATC flatc REQUIRED)

# hosq (header only, pinned by flake.lock) provides typed JSON schemas.
if(TSUGUMI_BUILD_CORE)
  set(HOSQ_INCLUDE_DIR "$ENV{HOSQ_INCLUDE_DIR}" CACHE PATH "hosq include directory")
  if(NOT EXISTS "${HOSQ_INCLUDE_DIR}/hosq/hosq.hh")
    message(FATAL_ERROR "hosq not found; set HOSQ_INCLUDE_DIR (nix develop does this)")
  endif()
  add_library(hosq INTERFACE)
  target_include_directories(hosq SYSTEM INTERFACE ${HOSQ_INCLUDE_DIR})
endif()

set(TSUGUMI_PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocol)
set(TSUGUMI_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(TSUGUMI_SCHEMAS tsugumi_enums data worker remote_worker tsugumi agent)

# Bindings are generated with the standard flatc into the build tree only.
set(_gen_cpp "")
set(_gen_bfbs "")
foreach(schema ${TSUGUMI_SCHEMAS})
  set(_src ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs)
  list(APPEND _gen_cpp ${TSUGUMI_GEN_DIR}/cpp/tsugumi/protocol/${schema}_generated.h)
  set(_cpp_object_api "")
  if(schema STREQUAL "remote_worker")
    set(_cpp_object_api --gen-object-api)
  endif()
  add_custom_command(
    OUTPUT ${TSUGUMI_GEN_DIR}/cpp/tsugumi/protocol/${schema}_generated.h
    COMMAND ${TSUGUMI_FLATC} --cpp --scoped-enums --gen-mutable ${_cpp_object_api}
            --keep-prefix --include-prefix tsugumi/protocol
            -I ${TSUGUMI_PROTOCOL_DIR} -o ${TSUGUMI_GEN_DIR}/cpp/tsugumi/protocol ${_src}
    COMMAND ${TSUGUMI_FLATC} --python --gen-object-api --python-typing -I ${TSUGUMI_PROTOCOL_DIR}
            -o ${TSUGUMI_GEN_DIR}/python ${_src}
    COMMAND ${TSUGUMI_FLATC} --swift -I ${TSUGUMI_PROTOCOL_DIR}
            -o ${TSUGUMI_GEN_DIR}/swift ${_src}
    DEPENDS ${_src} ${TSUGUMI_PROTOCOL_DIR}/data.fbs ${TSUGUMI_PROTOCOL_DIR}/tsugumi_enums.fbs
    COMMENT "flatc ${schema}.fbs"
    VERBATIM)
endforeach()
foreach(schema worker remote_worker tsugumi agent)
  list(APPEND _gen_bfbs ${TSUGUMI_GEN_DIR}/bfbs/${schema}.bfbs)
  add_custom_command(
    OUTPUT ${TSUGUMI_GEN_DIR}/bfbs/${schema}.bfbs
    COMMAND ${TSUGUMI_FLATC} -b --schema --bfbs-comments --bfbs-builtins
            -I ${TSUGUMI_PROTOCOL_DIR} -o ${TSUGUMI_GEN_DIR}/bfbs
            ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs
    DEPENDS ${TSUGUMI_PROTOCOL_DIR}/${schema}.fbs ${TSUGUMI_PROTOCOL_DIR}/data.fbs
            ${TSUGUMI_PROTOCOL_DIR}/tsugumi_enums.fbs
    VERBATIM)
endforeach()
add_custom_target(tsugumi_protocol_gen DEPENDS ${_gen_cpp} ${_gen_bfbs})

add_library(tsugumi_protocol INTERFACE)
add_dependencies(tsugumi_protocol tsugumi_protocol_gen)
target_include_directories(tsugumi_protocol INTERFACE ${TSUGUMI_GEN_DIR}/cpp)

find_package(flatbuffers CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_subdirectory(client)

if(TSUGUMI_BUILD_CORE)
  find_package(flatbuffers CONFIG REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
  # A project's versions are a git repository (core/src/versions.cc).
  pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
  # The embedded Nix evaluator: the same Nix version as the environment's daemon.
  pkg_check_modules(NIX REQUIRED IMPORTED_TARGET nix-expr nix-store nix-fetchers nix-flake nix-util)
  # Hashing: CommonCrypto on macOS, OpenSSL's libcrypto elsewhere.
  if(NOT APPLE)
    pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)
  endif()
  find_package(Threads REQUIRED)
  find_package(CURL REQUIRED)
  add_subdirectory(sdk)
  add_subdirectory(workerd)
  add_subdirectory(core)
endif()

if(TSUGUMI_BUILD_TESTS AND TSUGUMI_BUILD_CORE)
  enable_testing()
  add_subdirectory(tests/core)
endif()
