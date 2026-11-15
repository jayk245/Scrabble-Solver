cmake_minimum_required(VERSION 3.24)

project(ScrabbleSolver
  VERSION 0.1.0
  DESCRIPTION "Scrabble engine with Monte Carlo simulation and endgame solving"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd / IDE tooling can index the project.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SCRABBLE_BUILD_TESTS "Build unit tests" ON)
option(SCRABBLE_NATIVE "Optimize for the host CPU (-march=native)" ON)
option(SCRABBLE_LTO "Enable link-time optimization in Release builds" ON)
option(SCRABBLE_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)

add_subdirectory(engine)
add_subdirectory(apps)

if(SCRABBLE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
