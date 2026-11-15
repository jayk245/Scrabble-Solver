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
  move_test.cc)
target_link_libraries(scrabble_tests PRIVATE scrabble::engine GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(scrabble_tests)
