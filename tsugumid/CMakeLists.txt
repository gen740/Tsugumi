# Project runtime; definitions and shared accounting have independent targets.
include(cmake/nix.cmake)
if(APPLE)
  add_subdirectory(runtime/pack)
endif()
include(cmake/daemon.cmake)
