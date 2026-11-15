# Shared compiler configuration, exposed as an INTERFACE target so every
# library, app and test links the same flags instead of mutating globals.

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

add_library(scrabble_compiler_options INTERFACE)
add_library(scrabble::compiler_options ALIAS scrabble_compiler_options)

# Warnings are part of the build contract: new code should compile cleanly.
target_compile_options(scrabble_compiler_options INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wnon-virtual-dtor
  -Wold-style-cast)

if(SCRABBLE_NATIVE)
  check_cxx_compiler_flag(-march=native SCRABBLE_HAS_MARCH_NATIVE)
  check_cxx_compiler_flag(-mcpu=native SCRABBLE_HAS_MCPU_NATIVE)
  # x86 toolchains accept -march=native; Apple Silicon clang prefers -mcpu.
  if(SCRABBLE_HAS_MARCH_NATIVE)
    target_compile_options(scrabble_compiler_options INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo>:-march=native>)
  elseif(SCRABBLE_HAS_MCPU_NATIVE)
    target_compile_options(scrabble_compiler_options INTERFACE
      $<$<CONFIG:Release,RelWithDebInfo>:-mcpu=native>)
  endif()
endif()

target_compile_options(scrabble_compiler_options INTERFACE
  $<$<CONFIG:Release>:-O3>)

if(SCRABBLE_LTO)
  check_ipo_supported(RESULT SCRABBLE_IPO_SUPPORTED OUTPUT SCRABBLE_IPO_ERROR)
  if(SCRABBLE_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(STATUS "LTO not supported: ${SCRABBLE_IPO_ERROR}")
  endif()
endif()

if(SCRABBLE_SANITIZE)
  target_compile_options(scrabble_compiler_options INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(scrabble_compiler_options INTERFACE
    -fsanitize=address,undefined)
endif()
