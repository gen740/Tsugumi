add_library(tsugumi_peer_tls STATIC core/src/util/peer_tls.cc)
target_include_directories(tsugumi_peer_tls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/src)
# The helper is defined when core is configured; headers are staged there.
target_link_libraries(tsugumi_peer_tls PUBLIC OpenSSL::SSL tsugumi_util)
target_compile_options(tsugumi_peer_tls PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
if(TARGET tsugumi-workerd)
  target_link_libraries(tsugumi-workerd PRIVATE tsugumi_peer_tls)
endif()

add_library(tsugumi_worker_authorization STATIC workerd/src/authorized_keys.cc)
target_include_directories(tsugumi_worker_authorization PUBLIC ${PROJECT_SOURCE_DIR}/workerd/include)
target_link_libraries(tsugumi_worker_authorization PUBLIC tsugumi_peer_tls)
target_link_libraries(tsugumi-workerd PRIVATE tsugumi_worker_authorization)
