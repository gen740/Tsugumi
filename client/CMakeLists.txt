include(cmake/facade.cmake)

if(EMSCRIPTEN)
  include(cmake/wasm.cmake)
else()
  include(cmake/native.cmake)
endif()
