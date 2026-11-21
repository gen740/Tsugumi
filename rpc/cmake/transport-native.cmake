set(TSUGUMI_CLIENT_CURL_LIBRARY "" CACHE FILEPATH "Explicit native libcurl (for the macOS system TLS backend)")
if(TSUGUMI_CLIENT_CURL_LIBRARY)
  # The core's dependencies may already have found libcurl: point that target
  # at the explicit library instead of defining a second one.
  if(NOT TARGET CURL::libcurl)
    add_library(CURL::libcurl UNKNOWN IMPORTED GLOBAL)
  endif()
  set_target_properties(CURL::libcurl PROPERTIES IMPORTED_LOCATION "${TSUGUMI_CLIENT_CURL_LIBRARY}")
elseif(NOT TARGET CURL::libcurl)
  find_package(CURL REQUIRED)
endif()
find_package(Threads REQUIRED)
add_library(tsugumi_client_transport STATIC src/transport_native.cc src/frame_c.cc)
target_include_directories(tsugumi_client_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsugumi_client_transport PUBLIC tsugumi_frame PRIVATE tsugumi_crypto CURL::libcurl Threads::Threads)

install(TARGETS tsugumi_client_transport ARCHIVE DESTINATION lib)
