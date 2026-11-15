include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# Keep gtest out of `make install` and match the parent's runtime on MSVC.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(scrabble_tests
  tile_test.cc
  board_test.cc
  rng_test.cc
  rack_test.cc
  bag_test.cc
  move_test.cc
  letter_set_test.cc
  gaddag_test.cc
  lexicon_test.cc
  cross_checks_test.cc
  scoring_test.cc
  move_validation_test.cc
  move_generator_test.cc
  leave_evaluator_test.cc
  game_test.cc
  gcg_test.cc
  bot_test.cc
  arena_test.cc
  test_util.cc)
target_link_libraries(scrabble_tests PRIVATE scrabble::engine GTest::gtest_main)
# Integration tests read fetched word lists from the source tree.
target_compile_definitions(scrabble_tests PRIVATE
  SCRABBLE_LEXICA_DIR="${PROJECT_SOURCE_DIR}/data/lexica")

include(GoogleTest)
gtest_discover_tests(scrabble_tests)
add_subdirectory(server)
