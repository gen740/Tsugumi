# The root's boundary checker covers this target and its test binaries.
set(roots_view_host ui/macos/ViewHost PARENT_SCOPE)
set(allowed_view_host view_host protocol PARENT_SCOPE)
set(view_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(view_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(dawn_archive "${TSUGUMI_DAWN_DIR}/lib/libwebgpu_dawn.a")
set(wasmtime_dylib "${TSUGUMI_WASMTIME_LIB}/lib/libwasmtime.dylib")
include("${PROJECT_SOURCE_DIR}/ui/shared/dawn/webgpu_gen.cmake")
tsugumi_generate_webgpu_imports(
  OUT_DIR "${view_gen_dir}"
  WEBGPU_YML "${TSUGUMI_WEBGPU_HEADERS}/webgpu.yml"
  VIEW_HEADER "${PROJECT_SOURCE_DIR}/protocol/include/tsugumi/tsugumi_view.h"
)

add_library(tsugumi-view-host STATIC runtime.cc ${TSUGUMI_WEBGPU_GEN_SOURCES})
add_dependencies(tsugumi-view-host tsugumi-webgpu-generated)
set_target_properties(tsugumi-view-host PROPERTIES
  TSUGUMI_OWNER view_host
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/client"
)
target_include_directories(tsugumi-view-host
  PUBLIC "${view_dir}/include"
  PRIVATE "${view_dir}" "${view_gen_dir}" "${PROJECT_SOURCE_DIR}/protocol/include"
)
target_include_directories(tsugumi-view-host SYSTEM PRIVATE
  "${TSUGUMI_DAWN_DIR}/include" "${TSUGUMI_WASMTIME_DEV}/include"
)

find_program(TSUGUMI_VIEW_LIBTOOL libtool REQUIRED)
# Relink the archive when Dawn changes even if its headers are unchanged.
set_property(SOURCE runtime.cc APPEND PROPERTY OBJECT_DEPENDS "${dawn_archive}")
add_custom_command(TARGET tsugumi-view-host POST_BUILD
  COMMAND "${TSUGUMI_VIEW_LIBTOOL}" -static -o "$<TARGET_FILE:tsugumi-view-host>.merged"
          "$<TARGET_FILE:tsugumi-view-host>" "${dawn_archive}"
  COMMAND "${CMAKE_COMMAND}" -E rename "$<TARGET_FILE:tsugumi-view-host>.merged"
          "$<TARGET_FILE:tsugumi-view-host>"
  VERBATIM
)
configure_file("${wasmtime_dylib}" "${PROJECT_BINARY_DIR}/client/libwasmtime.dylib" COPYONLY)

target_link_libraries(tsugumi-view-host PRIVATE "${wasmtime_dylib}")
foreach(framework IN ITEMS Metal QuartzCore IOSurface IOKit CoreGraphics Foundation)
  find_library(TSUGUMI_VIEW_${framework} "${framework}" REQUIRED)
  target_link_libraries(tsugumi-view-host PRIVATE "${TSUGUMI_VIEW_${framework}}")
endforeach()

install(TARGETS tsugumi-view-host ARCHIVE DESTINATION lib)
install(FILES "${wasmtime_dylib}" DESTINATION lib)
install(FILES include/tsugumi_view_host.h DESTINATION include/tsugumi)

if(TSUGUMI_BUILD_VIEW_HOST_TESTS)
  find_package(GTest CONFIG REQUIRED)
  include(tests/load.cmake)
  set_property(TARGET tsugumi-view-load-tests PROPERTY TSUGUMI_OWNER view_host)
  add_subdirectory(tests)
  set_property(TARGET tsugumi-view-decode-tests PROPERTY TSUGUMI_OWNER view_host)
  add_test(NAME tsugumi-view-decode-tests COMMAND tsugumi-view-decode-tests)
  install(TARGETS tsugumi-view-decode-tests RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
