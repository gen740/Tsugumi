# Supervisor of the shared VM (Linux guest only): one tsugumid per project.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(tsugumi-vmd src/vmd_main.cc)
  target_include_directories(tsugumi-vmd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(tsugumi-vmd PRIVATE tsugumi_runtime_support tsugumi_nix_store tsugumi_worker_protocol)
  install(TARGETS tsugumi-vmd RUNTIME DESTINATION bin)
endif()
