# These are the generators used by sdk/nix/default.nix, linked into hostd so
# describing an input needs neither a language runtime nor a child process.
set(TSUGUMI_FLATBUFFERS_SOURCE "$ENV{TSUGUMI_FLATBUFFERS_SOURCE}" CACHE PATH "Source of the SDK's pinned FlatBuffers")
if(NOT EXISTS "${TSUGUMI_FLATBUFFERS_SOURCE}/src/idl_gen_cpp.cpp")
  message(FATAL_ERROR "Set TSUGUMI_FLATBUFFERS_SOURCE to the SDK's FlatBuffers source (nix develop supplies it)")
endif()
set(_generators ${TSUGUMI_FLATBUFFERS_SOURCE}/src/code_generators.cpp
  ${TSUGUMI_FLATBUFFERS_SOURCE}/include/codegen/python.cc)
foreach(language cpp python go rust csharp)
  list(APPEND _generators ${TSUGUMI_FLATBUFFERS_SOURCE}/src/idl_gen_${language}.cpp)
endforeach()
add_library(tsugumi_sdk_describe STATIC input.cc grpc_unused.cc ${_generators})
target_include_directories(tsugumi_sdk_describe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(tsugumi_sdk_describe SYSTEM PRIVATE
  ${TSUGUMI_FLATBUFFERS_SOURCE}/src ${TSUGUMI_FLATBUFFERS_SOURCE}/include)
target_link_libraries(tsugumi_sdk_describe PUBLIC flatbuffers::flatbuffers)
# flatc creates namespace directories even with a custom FileSaver. Redirect
# only these embedded generator objects' directory calls to the memory sink.
target_compile_definitions(tsugumi_sdk_describe PRIVATE
  EnsureDirExists=TsugumiDescribeEnsureDirExists)
