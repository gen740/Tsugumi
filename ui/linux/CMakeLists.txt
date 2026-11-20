# The Linux desktop UI: GTK 4 windows that draw what hostd and the project's
# tsugumid give them over Host and project RPC (the shared C++ client).
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

set(_warnings -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)

# The English strings (ui/shared/i18n/en.json) are compiled in.
set(_strings_source ${CMAKE_CURRENT_BINARY_DIR}/english_strings.cc)
add_custom_command(
  OUTPUT ${_strings_source}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${PROJECT_SOURCE_DIR}/ui/shared/i18n/en.json
          -DOUTPUT=${_strings_source} -DSYMBOL=tsugumi_english_strings -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${PROJECT_SOURCE_DIR}/ui/shared/i18n/en.json ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  VERBATIM)

# What the windows show, without GTK: tested on any system.
add_library(tsugumi_linux_ui_model STATIC
  src/strings.cc src/canvas_scene.cc src/log_window.cc ${_strings_source})
target_include_directories(tsugumi_linux_ui_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tsugumi_linux_ui_model PRIVATE tsugumi_json)
target_compile_options(tsugumi_linux_ui_model PRIVATE ${_warnings})

add_library(tsugumi_linux_ui STATIC
  src/main_thread.cc src/host_link.cc src/project_session.cc
  src/canvas_view.cc src/home_view.cc src/log_view.cc src/project_view.cc src/app_window.cc)
target_link_libraries(tsugumi_linux_ui
  PUBLIC tsugumi_linux_ui_model tsugumi_host_launch tsugumi_client flatbuffers::flatbuffers PkgConfig::GTK4
  PRIVATE tsugumi_json)
target_compile_options(tsugumi_linux_ui PRIVATE ${_warnings})

add_executable(tsugumi-gtk src/main.cc)
target_link_libraries(tsugumi-gtk PRIVATE tsugumi_linux_ui)
target_compile_options(tsugumi-gtk PRIVATE ${_warnings})
install(TARGETS tsugumi-gtk RUNTIME DESTINATION bin)
install(FILES dev.tsugumi.Tsugumi.desktop DESTINATION share/applications)

add_executable(tsugumi_linux_ui_tests tests/model_test.cc)
target_link_libraries(tsugumi_linux_ui_tests PRIVATE tsugumi_linux_ui_model tsugumi_json)
target_compile_options(tsugumi_linux_ui_tests PRIVATE ${_warnings})

# Needs a display: the Nix check runs it on GTK's Broadway backend.
add_executable(tsugumi_gtk_smoke_test tests/gtk_smoke_test.cc)
target_link_libraries(tsugumi_gtk_smoke_test PRIVATE tsugumi_linux_ui)
target_compile_options(tsugumi_gtk_smoke_test PRIVATE ${_warnings})
