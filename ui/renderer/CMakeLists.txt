# Dawn based Display renderer (macOS host only, independent of the core).
# Produces libtsugumi_renderer.dylib with a small C API used by the Swift bridge.
include(FetchContent)

set(TSUGUMI_DAWN_TAG "v20261023.194543" CACHE STRING "Pinned Dawn release tag")
set(DAWN_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_METAL ON CACHE BOOL "" FORCE)
set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL OFF CACHE BOOL "" FORCE)
set(DAWN_USE_GLFW OFF CACHE BOOL "" FORCE)
set(DAWN_USE_X11 OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC CACHE STRING "" FORCE)
# The optional C++20 module interface needs dependency scanning, which the
# project disables; the renderer uses the regular webgpu_cpp.h header.
set(DAWN_SUPPORTS_CXX_MODULES FALSE CACHE INTERNAL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_SPV_READER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_SPV_WRITER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_GLSL_WRITER OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)

FetchContent_Declare(dawn
  GIT_REPOSITORY https://github.com/google/dawn.git
  GIT_TAG ${TSUGUMI_DAWN_TAG}
  GIT_SHALLOW TRUE
  # Dawn fetches the few dependencies it needs itself (DAWN_FETCH_DEPENDENCIES);
  # recursive submodules would pull in unrelated Chromium projects.
  GIT_SUBMODULES ""
  GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(dawn)

add_library(tsugumi_renderer SHARED src/renderer.cc src/metal_layer.mm)
target_include_directories(tsugumi_renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_renderer PRIVATE webgpu_dawn
  "-framework Metal" "-framework QuartzCore" "-framework IOSurface" "-framework Foundation")
set_target_properties(tsugumi_renderer PROPERTIES
  INSTALL_NAME_DIR "@rpath"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
