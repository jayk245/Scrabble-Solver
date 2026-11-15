add_executable(scrabble_cli cli/main.cc)
target_link_libraries(scrabble_cli PRIVATE scrabble::engine)

add_executable(scrabble_lexicon_tool lexicon_tool/main.cc)
target_link_libraries(scrabble_lexicon_tool PRIVATE scrabble::engine)
