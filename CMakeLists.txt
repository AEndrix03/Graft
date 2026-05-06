cmake_minimum_required(VERSION 3.20)
project(memgraph C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -fPIC)
endif()

# === Dipendenze esterne ===
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE REQUIRED sqlite3)
pkg_check_modules(YAML REQUIRED yaml-0.1)

# llama.cpp (deve essere stato compilato in third_party/llama.cpp/build)
set(LLAMA_DIR ${CMAKE_SOURCE_DIR}/third_party/llama.cpp)
include_directories(${LLAMA_DIR}/include ${LLAMA_DIR}/ggml/include)
link_directories(${LLAMA_DIR}/build/bin ${LLAMA_DIR}/build/src ${LLAMA_DIR}/build/ggml/src)

# mpack
set(MPACK_DIR ${CMAKE_SOURCE_DIR}/third_party/mpack/src/mpack)
include_directories(${MPACK_DIR})

# blake3 (portable-only: skip SIMD to avoid intrinsics build pain on MinGW)
set(BLAKE3_DIR ${CMAKE_SOURCE_DIR}/third_party/BLAKE3/c)
add_library(blake3 STATIC
  ${BLAKE3_DIR}/blake3.c
  ${BLAKE3_DIR}/blake3_dispatch.c
  ${BLAKE3_DIR}/blake3_portable.c
)
target_include_directories(blake3 PUBLIC ${BLAKE3_DIR})
target_compile_definitions(blake3 PRIVATE
  BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512
)

# sqlite-vec
set(SQLITE_VEC_DIR ${CMAKE_SOURCE_DIR}/third_party/sqlite-vec)
add_library(sqlite_vec STATIC ${SQLITE_VEC_DIR}/sqlite-vec.c)
target_include_directories(sqlite_vec PUBLIC ${SQLITE_VEC_DIR} ${SQLITE_INCLUDE_DIRS})
target_compile_definitions(sqlite_vec PRIVATE SQLITE_CORE)

# mpack come libreria
add_library(mpack STATIC
  ${MPACK_DIR}/mpack-common.c
  ${MPACK_DIR}/mpack-expect.c
  ${MPACK_DIR}/mpack-node.c
  ${MPACK_DIR}/mpack-platform.c
  ${MPACK_DIR}/mpack-reader.c
  ${MPACK_DIR}/mpack-writer.c
)
target_include_directories(mpack PUBLIC ${MPACK_DIR})

# === Headers ===
include_directories(${CMAKE_SOURCE_DIR}/include)

# === Sorgenti core (libreria condivisa interna) ===
file(GLOB MEMGRAPH_SRC CONFIGURE_DEPENDS
  src/common/*.c
  src/storage/*.c
  src/embed/*.c
  src/insert/*.c
  src/retrieve/*.c
  src/explore/*.c
  src/verify/*.c
  src/stats/*.c
  src/config/*.c
  src/daemon/wire.c
  src/daemon/dispatch.c
  src/daemon/socket.c
)

add_library(memgraph_core STATIC ${MEMGRAPH_SRC})
# Order matters: sqlite_vec depends on sqlite3 symbols, must precede ${SQLITE_LIBRARIES}.
set(MG_PLATFORM_LIBS "")
if(WIN32)
  list(APPEND MG_PLATFORM_LIBS ws2_32 bcrypt)
endif()
target_link_libraries(memgraph_core
  sqlite_vec
  ${SQLITE_LIBRARIES}
  ${YAML_LIBRARIES}
  mpack
  blake3
  llama
  ggml
  m
  pthread
  ${MG_PLATFORM_LIBS}
)

# === Binari ===
add_executable(memgraphd src/daemon/main.c)
target_link_libraries(memgraphd memgraph_core)

add_executable(memgraph src/cli/main.c)
target_link_libraries(memgraph memgraph_core)

# === Tests ===
enable_testing()
file(GLOB TEST_SRC tests/test_*.c)
foreach(TFILE ${TEST_SRC})
  get_filename_component(TNAME ${TFILE} NAME_WE)
  add_executable(${TNAME} ${TFILE})
  target_link_libraries(${TNAME} memgraph_core)
  add_test(NAME ${TNAME} COMMAND ${TNAME})
endforeach()
