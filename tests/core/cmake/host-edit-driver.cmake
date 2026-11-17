# Lets native client fixtures exercise the real pure host edit without sockets.
add_executable(tsugumi_document_edit_driver agent/document_edit_driver.cc)
target_link_libraries(tsugumi_document_edit_driver PRIVATE tsugumi_host_core)
target_compile_options(tsugumi_document_edit_driver PRIVATE -Wall -Wextra -Werror -Wformat=2 -Wimplicit-fallthrough -Wmissing-designated-field-initializers)
if(TSUGUMI_INSTALLED_CORE)
  install(TARGETS tsugumi_document_edit_driver RUNTIME DESTINATION libexec/tsugumi-tests)
endif()
