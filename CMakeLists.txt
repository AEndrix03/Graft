cmake_minimum_required(VERSION 3.20)
file(READ "${CMAKE_SOURCE_DIR}/VERSION" GRAFT_VERSION_RAW)
string(STRIP "${GRAFT_VERSION_RAW}" GRAFT_VERSION_FULL)
# project(VERSION ...) only accepts X.Y.Z[.W], strip SemVer pre-release suffix.
string(REGEX REPLACE "[-+].*$" "" GRAFT_VERSION_NUMERIC "${GRAFT_VERSION_FULL}")
project(graft VERSION ${GRAFT_VERSION_NUMERIC} LANGUAGES C)
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
  BLAKE3_USE_NEON=0
)

# sqlite-vec
set(SQLITE_VEC_DIR ${CMAKE_SOURCE_DIR}/third_party/sqlite-vec)
# The submodule ships only a `sqlite-vec.h.tmpl`; the header is generated
# at make-time upstream. Substitute the placeholders ourselves so the build
# works end-to-end without an external `make sqlite-vec.h` step.
if(NOT EXISTS ${SQLITE_VEC_DIR}/sqlite-vec.h)
  file(READ ${SQLITE_VEC_DIR}/VERSION _sv_raw)
  string(STRIP "${_sv_raw}" VERSION)
  string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _sv_match "${VERSION}")
  set(VERSION_MAJOR "${CMAKE_MATCH_1}")
  set(VERSION_MINOR "${CMAKE_MATCH_2}")
  set(VERSION_PATCH "${CMAKE_MATCH_3}")
  string(TIMESTAMP DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
  set(SOURCE "cmake-configured")
  configure_file(
    ${SQLITE_VEC_DIR}/sqlite-vec.h.tmpl
    ${SQLITE_VEC_DIR}/sqlite-vec.h
    @ONLY
  )
  # @ONLY only substitutes @VAR@ — but the template uses ${VAR}. Re-run with
  # the default mode that also handles ${VAR}.
  configure_file(
    ${SQLITE_VEC_DIR}/sqlite-vec.h.tmpl
    ${SQLITE_VEC_DIR}/sqlite-vec.h
  )
endif()
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
# SQLITE/YAML headers may live outside the compiler default search path on
# Homebrew arm64 (/opt/homebrew/include) — explicitly add the pkg-config dirs.
include_directories(
  ${CMAKE_SOURCE_DIR}/include
  ${SQLITE_INCLUDE_DIRS}
  ${YAML_INCLUDE_DIRS}
)

# === Sorgenti core (libreria condivisa interna) ===
file(GLOB GRAFT_SRC CONFIGURE_DEPENDS
  src/common/*.c
  src/storage/*.c
  src/embed/*.c
  src/insert/*.c
  src/retrieve/*.c
  src/explore/*.c
  src/verify/*.c
  src/rerank/*.c
  src/stats/*.c
  src/maintain/*.c
  src/config/*.c
  src/http/*.c
  src/daemon/wire.c
  src/daemon/dispatch.c
  src/daemon/socket.c
)

add_library(graft_core STATIC ${GRAFT_SRC})
# Order matters: sqlite_vec depends on sqlite3 symbols, must precede ${SQLITE_LIBRARIES}.
set(MG_PLATFORM_LIBS "")
if(WIN32)
  list(APPEND MG_PLATFORM_LIBS ws2_32 bcrypt)
endif()
target_link_libraries(graft_core
  sqlite_vec
  ${SQLITE_LIBRARIES}
  ${YAML_LIBRARIES}
  mpack
  blake3
  llama
  ggml
  ggml-base
  m
  pthread
  ${MG_PLATFORM_LIBS}
)

# === Binari ===
add_executable(graftd src/daemon/main.c)
target_link_libraries(graftd graft_core)

# Everything the CLI does except main(), so tests can link the subcommands.
add_library(graft_cli STATIC
  src/cli/autostart.c
  src/cli/client.c
  src/cli/sources.c
  src/cli/project.c
  src/cli/maintain.c
  src/cli/status.c
  src/cli/hook.c
  src/cli/usage_log.c
  src/cli/profile.c
  src/cli/setup.c
  src/cli/upgrade.c
  src/cli/view.c
)
target_compile_definitions(graft_cli PUBLIC
  GRAFT_VERSION="${GRAFT_VERSION_FULL}"
  GRAFT_REPOSITORY="AEndrix03/Graft"
)
target_link_libraries(graft_cli PUBLIC graft_core)

add_executable(graft src/cli/main.c)
target_link_libraries(graft graft_cli)

# === Tests ===
option(GRAFT_BUILD_TESTS "Build the test executables" ON)
if(GRAFT_BUILD_TESTS)
  enable_testing()
  # On Windows the test binaries import llama/ggml and the MinGW runtime
  # directly (the CLI does not - the linker drops the unused imports, which is
  # why `graft.exe` runs while the tests die with 0xc0000139
  # STATUS_ENTRYPOINT_NOT_FOUND). Neither directory is on PATH under a plain
  # shell, so put them there for ctest instead of asking humans to do it.
  set(MG_TEST_PATHS "")
  if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
    get_filename_component(MG_TOOLCHAIN_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
    foreach(DIR
        ${LLAMA_DIR}/build/bin
        ${LLAMA_DIR}/build/src
        ${LLAMA_DIR}/build/ggml/src
        ${MG_TOOLCHAIN_BIN})
      if(IS_DIRECTORY "${DIR}")
        file(TO_NATIVE_PATH "${DIR}" NATIVE_DIR)
        list(APPEND MG_TEST_PATHS "PATH=path_list_prepend:${NATIVE_DIR}")
      endif()
    endforeach()
  endif()

  file(GLOB TEST_SRC tests/test_*.c)
  foreach(TFILE ${TEST_SRC})
    get_filename_component(TNAME ${TFILE} NAME_WE)
    add_executable(${TNAME} ${TFILE})
    target_link_libraries(${TNAME} graft_cli)
    add_test(NAME ${TNAME} COMMAND ${TNAME})
    # A hung test must fail, not hold a CI runner for its 6 h job limit.
    # Exit code 77 means "skipped" (test_embed without GRAFT_TEST_MODEL).
    set_tests_properties(${TNAME} PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
    if(MG_TEST_PATHS)
      set_tests_properties(${TNAME} PROPERTIES ENVIRONMENT_MODIFICATION "${MG_TEST_PATHS}")
    endif()
  endforeach()

  # End to end against a real model: a private graftd, the CLI and the prompt
  # hook (tests/e2e/test_e2e.py). Skipped (exit 77) without GRAFT_TEST_MODEL.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME e2e_model
             COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/e2e/test_e2e.py
                     --graft $<TARGET_FILE:graft>)
    set_tests_properties(e2e_model PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
    if(MG_TEST_PATHS)
      set_tests_properties(e2e_model PROPERTIES ENVIRONMENT_MODIFICATION "${MG_TEST_PATHS}")
    endif()
  endif()

  # The CLI must refuse an option it does not know, or a known one without its
  # value, before it ever talks to the daemon (no daemon runs during ctest).
  add_test(NAME cli_rejects_unknown_option
           COMMAND graft insert --summary hello --body world)
  add_test(NAME cli_rejects_missing_value
           COMMAND graft insert --body world --title)
  add_test(NAME cli_rejects_unknown_query_option
           COMMAND graft query --top-k 3 hello)
  set_tests_properties(cli_rejects_unknown_option PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option or missing value: '--summary'")
  set_tests_properties(cli_rejects_missing_value PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option or missing value: '--title'")
  set_tests_properties(cli_rejects_unknown_query_option PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option or missing value: '--top-k'")
  # --source file: is resolved and fingerprinted client-side, so a missing
  # file fails before any daemon is contacted.
  add_test(NAME cli_rejects_missing_source_file
           COMMAND graft insert --title t --body b --source file:no-such-file.md)
  add_test(NAME cli_rejects_unknown_source_kind
           COMMAND graft insert --title t --body b --source bogus:x)
  add_test(NAME cli_rejects_unknown_sources_option
           COMMAND graft sources diff --bogus)
  set_tests_properties(cli_rejects_missing_source_file PROPERTIES
    PASS_REGULAR_EXPRESSION "source file not found: no-such-file.md")
  set_tests_properties(cli_rejects_unknown_source_kind PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown source 'bogus:x'")
  set_tests_properties(cli_rejects_unknown_sources_option PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option or missing value: '--bogus'")
  # `project` never talks to the daemon: it runs end to end here, against a
  # DB path of its own (the state file lands in <build>/projects/).
  add_test(NAME cli_rejects_unknown_project_option
           COMMAND graft project status --bogus)
  add_test(NAME cli_rejects_project_topic_without_state
           COMMAND graft project mark --topic x)
  add_test(NAME cli_project_mark_run
           COMMAND graft project mark --topic smoke --state pending --priority high --run)
  set_tests_properties(cli_rejects_unknown_project_option PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option or missing value: '--bogus'")
  set_tests_properties(cli_rejects_project_topic_without_state PROPERTIES
    PASS_REGULAR_EXPRESSION "--state is covered, pending, skipped or drop")
  set_tests_properties(cli_project_mark_run PROPERTIES
    ENVIRONMENT "GRAFT_DB_PATH=${CMAKE_BINARY_DIR}/cli_project_test.db"
    PASS_REGULAR_EXPRESSION "\"bootstrapped\": true")
  # `graft maintain` validates its arguments before contacting the daemon.
  add_test(NAME cli_rejects_unknown_maintain_option
           COMMAND graft maintain scan --bogus)
  add_test(NAME cli_rejects_maintain_missing_value
           COMMAND graft maintain resolve abc --action)
  add_test(NAME cli_rejects_maintain_resolve_without_action
           COMMAND graft maintain resolve abc --note n)
  add_test(NAME cli_rejects_maintain_bad_limit
           COMMAND graft maintain log --limit 0)
  add_test(NAME cli_rejects_maintain_extra_argument
           COMMAND graft maintain status now)
  set_tests_properties(cli_rejects_unknown_maintain_option PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option or missing value: '--bogus'")
  set_tests_properties(cli_rejects_maintain_missing_value PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option or missing value: '--action'")
  set_tests_properties(cli_rejects_maintain_resolve_without_action PROPERTIES
    PASS_REGULAR_EXPRESSION "--action is required")
  set_tests_properties(cli_rejects_maintain_bad_limit PROPERTIES
    PASS_REGULAR_EXPRESSION "--limit expects a positive integer")
  set_tests_properties(cli_rejects_maintain_extra_argument PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option or missing value: 'now'")
  # `graft status` never starts the daemon: with none listening it still
  # answers, from the project state alone, with maintenance null.
  add_test(NAME cli_rejects_unknown_status_option
           COMMAND graft status --bogus)
  add_test(NAME cli_rejects_unknown_hook_event
           COMMAND graft hook bogus)
  add_test(NAME cli_status_without_daemon
           COMMAND graft status)
  set_tests_properties(cli_rejects_unknown_status_option PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option or missing value: '--bogus'")
  set_tests_properties(cli_rejects_unknown_hook_event PROPERTIES
    PASS_REGULAR_EXPRESSION "usage: graft hook <session-start\\|prompt>")
  set_tests_properties(cli_status_without_daemon PROPERTIES
    ENVIRONMENT "GRAFT_HOME=${CMAKE_BINARY_DIR}/cli_status_home;GRAFT_DB_PATH=${CMAKE_BINARY_DIR}/cli_status_test.db;GRAFT_SOCKET=${CMAKE_BINARY_DIR}/cli_status_no_daemon.sock"
    PASS_REGULAR_EXPRESSION "\"maintenance\": null")
endif()
