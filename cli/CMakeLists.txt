# How a local client reaches hostd: the tsugumi command, its MCP server and the
# Linux desktop UI start or find the same host with `tsugumi-hostd --ensure`.
add_library(tsugumi_host_launch STATIC src/launch.cc)
target_include_directories(tsugumi_host_launch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_host_launch PUBLIC tsugumi_client flatbuffers::flatbuffers PRIVATE tsugumi_json)
target_compile_options(tsugumi_host_launch PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)

# The tsugumi command: the Web UI launcher and the Claude Code plugin's MCP
# server. It reaches hostd only through Host and Agent RPC.
add_executable(tsugumi src/main.cc src/mcp.cc src/agent_catalog.cc src/agent_tools.cc src/commands.cc src/maintenance.cc)
target_link_libraries(tsugumi PRIVATE tsugumi_host_launch tsugumi_client tsugumi_rpc_schema tsugumi_json flatbuffers::flatbuffers)
target_compile_options(tsugumi PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
install(TARGETS tsugumi RUNTIME DESTINATION bin)
