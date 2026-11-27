# tsugumi-cached: the own Nix binary cache server (docs/design/BinaryCacheServer.md).
# Host-wide like hostd's execution broker, but its own process: it links the
# Nix libraries, which the Mac's native hostd does not have.
add_library(tsugumi_cache STATIC src/cache.cc src/http.cc)
target_compile_features(tsugumi_cache PRIVATE cxx_std_23)
target_include_directories(tsugumi_cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(tsugumi_cache PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_link_libraries(tsugumi_cache PRIVATE PkgConfig::NIX tsugumi_util)
add_executable(tsugumi-cached src/main.cc)
target_compile_features(tsugumi-cached PRIVATE cxx_std_23)
target_compile_options(tsugumi-cached PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough)
target_link_libraries(tsugumi-cached PRIVATE tsugumi_cache tsugumi_peer_tls tsugumi_frame tsugumi_util PkgConfig::NIX Threads::Threads)
install(TARGETS tsugumi-cached RUNTIME DESTINATION bin)
