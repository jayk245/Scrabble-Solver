add_library(scrabble_engine
  src/board.cc)
add_library(scrabble::engine ALIAS scrabble_engine)

target_include_directories(scrabble_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(scrabble_engine PUBLIC scrabble::compiler_options)
