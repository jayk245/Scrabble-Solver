# HTTP/JSON server behind the web UI.
#
# Split into a socket-free library (JSON codec and request handlers, unit
# tested in tests/server) and a thin executable that binds the handlers to
# cpp-httplib routes.

include(FetchContent)

# Both dependencies are header-only. Their own CMake projects are skipped
# (SOURCE_SUBDIR points at a directory that does not exist) so that optional
# features such as OpenSSL or zlib detection never leak into this build; the
# INTERFACE targets below expose the headers as SYSTEM includes, which keeps
# third-party warnings out of the project's zero-warning contract.
FetchContent_Declare(
  httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.60.1.tar.gz
  URL_HASH SHA256=f42fc046e55f968a9dd43b272251f033b08538d3e359235608f980f551053215
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR headers-only)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR headers-only)
FetchContent_MakeAvailable(httplib nlohmann_json)

add_library(scrabble_third_party_httplib INTERFACE)
target_include_directories(scrabble_third_party_httplib SYSTEM INTERFACE
  ${httplib_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(scrabble_third_party_httplib INTERFACE Threads::Threads)

add_library(scrabble_third_party_json INTERFACE)
target_include_directories(scrabble_third_party_json SYSTEM INTERFACE
  ${nlohmann_json_SOURCE_DIR}/single_include)

add_library(scrabble_server_lib
  api.cc
  codec.cc)
add_library(scrabble::server_lib ALIAS scrabble_server_lib)
target_include_directories(scrabble_server_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(scrabble_server_lib PUBLIC
  scrabble::engine
  scrabble_third_party_json)

add_executable(scrabble_server
  http_server.cc
  main.cc)
target_link_libraries(scrabble_server PRIVATE
  scrabble::server_lib
  scrabble_third_party_httplib)
target_compile_definitions(scrabble_server PRIVATE
  SCRABBLE_LEXICA_DIR="${PROJECT_SOURCE_DIR}/data/lexica"
  SCRABBLE_WEB_DIST_DIR="${PROJECT_SOURCE_DIR}/web/dist"
  SCRABBLE_VERSION="${PROJECT_VERSION}")
