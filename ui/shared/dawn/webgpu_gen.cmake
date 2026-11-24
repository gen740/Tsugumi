function(tsugumi_generate_webgpu_imports)
  cmake_parse_arguments(PARSE_ARGV 0 gen "" "OUT_DIR;WEBGPU_YML;VIEW_HEADER" "")
  if(gen_UNPARSED_ARGUMENTS OR NOT gen_OUT_DIR OR NOT gen_WEBGPU_YML OR NOT gen_VIEW_HEADER)
    message(FATAL_ERROR "Expected OUT_DIR, WEBGPU_YML and VIEW_HEADER")
  endif()

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(generator_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  cmake_path(ABSOLUTE_PATH gen_OUT_DIR BASE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" NORMALIZE)
  set(sources
    "${gen_OUT_DIR}/webgpu_imports.gen.h"
    "${gen_OUT_DIR}/webgpu_imports.gen.cc"
    "${gen_OUT_DIR}/webgpu_procs_dawn.gen.cc"
  )
  add_custom_command(
    OUTPUT ${sources}
    COMMAND "${Python3_EXECUTABLE}" "${generator_dir}/gen_webgpu.py"
            --webgpu-yml "${gen_WEBGPU_YML}" --view-header "${gen_VIEW_HEADER}"
            --cpp "${gen_OUT_DIR}"
    DEPENDS "${gen_WEBGPU_YML}" "${gen_VIEW_HEADER}" "${generator_dir}/gen_webgpu.py"
            "${generator_dir}/webgpu_idl.py" "${generator_dir}/cpp_reader.inc"
            "${generator_dir}/js_reader.inc"
    VERBATIM
  )
  # The decoder tests compile the generated source in a separate CMake directory.
  add_custom_target(tsugumi-webgpu-generated DEPENDS ${sources})
  set(TSUGUMI_WEBGPU_GEN_SOURCES ${sources} PARENT_SCOPE)
endfunction()
