add_library(tsugumi_worker_values STATIC src/values.cc src/value_wire.cc src/module_source.cc)
target_include_directories(tsugumi_worker_values PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_worker_values PUBLIC tsugumi_document tsugumi_util)

set(TSUGUMI_BWRAP "" CACHE FILEPATH "bubblewrap used for sandbox: true Cards")

# One implementation of worker ownership and invocation, shared by the core and
# remote launcher. Embed these objects in the core archive for installed users;
# the engine does not link back to the core's scheduling or service layer.
add_library(tsugumi_worker_runtime OBJECT src/worker_proc.cc src/worker_pool.cc src/worker_execution.cc src/local_transport.cc src/local_client.cc)
target_include_directories(tsugumi_worker_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(tsugumi_worker_runtime PUBLIC tsugumi_trace_codec tsugumi_workerd_support tsugumi_worker_values Threads::Threads)
target_compile_definitions(tsugumi_worker_runtime PRIVATE TSUGUMI_BWRAP="${TSUGUMI_BWRAP}")
target_compile_options(tsugumi_worker_runtime PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
