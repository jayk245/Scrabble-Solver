# The learned default leave table is compiled into the engine (see
# LeaveEvaluator::Learned()), regenerated whenever the CSV changes.
set(SCRABBLE_DEFAULT_LEAVES_CSV
  ${PROJECT_SOURCE_DIR}/data/leaves/enable_leaves_default.csv)
set(SCRABBLE_DEFAULT_LEAVES_CC
  ${CMAKE_CURRENT_BINARY_DIR}/generated/default_leaves_data.cc)
add_custom_command(
  OUTPUT ${SCRABBLE_DEFAULT_LEAVES_CC}
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=${SCRABBLE_DEFAULT_LEAVES_CSV}
    -DOUTPUT=${SCRABBLE_DEFAULT_LEAVES_CC}
    -DSYMBOL=kDefaultLeaves
    -DNAMESPACE=scrabble::embedded
    -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
  DEPENDS
    ${SCRABBLE_DEFAULT_LEAVES_CSV}
    ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
  COMMENT "Embedding the default leave table"
  VERBATIM)

add_library(scrabble_engine
  ${SCRABBLE_DEFAULT_LEAVES_CC}
  src/arena.cc
  src/bag.cc
  src/board.cc
  src/bot.cc
  src/cross_checks.cc
  src/endgame.cc
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

find_package(Threads REQUIRED)
# The arena runs games on a std::thread pool.
target_link_libraries(scrabble_engine
  PUBLIC scrabble::compiler_options
  PRIVATE Threads::Threads)
