add_executable(tsugumi-workerd src/main.cc src/local_service.cc)
target_link_libraries(tsugumi-workerd PRIVATE tsugumi_worker_runtime tsugumi_worker_values)
target_compile_options(tsugumi-workerd PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
install(TARGETS tsugumi-workerd RUNTIME DESTINATION bin)
