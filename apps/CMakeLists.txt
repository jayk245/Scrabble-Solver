add_executable(scrabble_cli cli/main.cc)
target_link_libraries(scrabble_cli PRIVATE scrabble::engine)

add_executable(scrabble_lexicon_tool lexicon_tool/main.cc)
target_link_libraries(scrabble_lexicon_tool PRIVATE scrabble::engine)

add_executable(scrabble_movegen_bench movegen_bench/main.cc)
target_link_libraries(scrabble_movegen_bench PRIVATE scrabble::engine)
# Default lexicon location, overridable with --lexicon.
target_compile_definitions(scrabble_movegen_bench PRIVATE
  SCRABBLE_LEXICA_DIR="${PROJECT_SOURCE_DIR}/data/lexica")

add_executable(scrabble_arena arena/main.cc)
target_link_libraries(scrabble_arena PRIVATE scrabble::engine)
target_compile_definitions(scrabble_arena PRIVATE
  SCRABBLE_LEXICA_DIR="${PROJECT_SOURCE_DIR}/data/lexica")

add_subdirectory(server)
