# Socket-free tests for the web server's JSON codec and request handlers.
add_executable(scrabble_server_tests
  api_test.cc
  codec_test.cc)
target_link_libraries(scrabble_server_tests PRIVATE
  scrabble::server_lib GTest::gtest_main)
target_compile_definitions(scrabble_server_tests PRIVATE
  SCRABBLE_LEXICA_DIR="${PROJECT_SOURCE_DIR}/data/lexica")

gtest_discover_tests(scrabble_server_tests)
