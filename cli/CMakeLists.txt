# The tsugumi command: the Web UI launcher and the Claude Code plugin's MCP
# server. It reaches hostd only through Host and Agent RPC.
add_executable(tsugumi src/main.cc src/launch.cc src/mcp.cc src/agent_catalog.cc src/maintenance.cc)
target_link_libraries(tsugumi PRIVATE tsugumi_client tsugumi_rpc_schema tsugumi_json flatbuffers::flatbuffers)
target_compile_options(tsugumi PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
install(TARGETS tsugumi RUNTIME DESTINATION bin)
