add_library(tsugumi_pack_card OBJECT card_call.cc)
target_include_directories(tsugumi_pack_card PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tsugumi_pack_card PUBLIC tsugumi_protocol flatbuffers::flatbuffers
  PRIVATE ${CMAKE_DL_LIBS})

if(TSUGUMI_BUILD_TESTS)
  add_library(tsugumi_pack_card_fixture MODULE card_fixture.cc)
  target_link_libraries(tsugumi_pack_card_fixture PRIVATE tsugumi_protocol flatbuffers::flatbuffers)
  add_executable(tsugumi_pack_card_test card_call_test.cc)
  target_link_libraries(tsugumi_pack_card_test PRIVATE tsugumi_pack_card)
  add_test(NAME pack-card-call
    COMMAND tsugumi_pack_card_test $<TARGET_FILE:tsugumi_pack_card_fixture>)
endif()
