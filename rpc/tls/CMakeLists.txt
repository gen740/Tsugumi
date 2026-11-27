add_library(tsugumi_peer_tls STATIC src/peer_tls.cc src/authorized_keys.cc src/dashboard_http.cc)
target_include_directories(tsugumi_peer_tls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_peer_tls PUBLIC OpenSSL::SSL tsugumi_util)
