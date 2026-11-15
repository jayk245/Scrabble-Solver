add_executable(scrabble_cli cli/main.cc)
target_link_libraries(scrabble_cli PRIVATE scrabble::engine)
