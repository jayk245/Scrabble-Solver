add_library(scrabble_engine
  src/bag.cc
  src/board.cc
  src/bot.cc
  src/cross_checks.cc
  src/gaddag.cc
  src/game.cc
  src/gcg.cc
  src/leave_evaluator.cc
  src/lexicon.cc
  src/move.cc
  src/move_generator.cc
  src/move_validation.cc
  src/position.cc
  src/rack.cc
  src/scoring.cc)
add_library(scrabble::engine ALIAS scrabble_engine)

target_include_directories(scrabble_engine PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(scrabble_engine PUBLIC scrabble::compiler_options)
