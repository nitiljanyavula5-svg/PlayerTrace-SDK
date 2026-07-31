# Copyright 2026 The PlayerTrace Authors
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

project(playertrace
  VERSION 0.1.0
  DESCRIPTION "Engine-agnostic, privacy-conscious C++17 telemetry SDK"
  HOMEPAGE_URL "https://github.com/nitiljanyavula5-svg/PlayerTrace-SDK"
  LANGUAGES C CXX)

# Treat this as the top-level project only when built standalone. Everything
# that would affect a parent project (default build type, tests, examples,
# install rules) is gated on this.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(PLAYERTRACE_IS_TOP_LEVEL ON)
else()
  set(PLAYERTRACE_IS_TOP_LEVEL OFF)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PLAYERTRACE_BUILD_TESTS "Build unit tests" ${PLAYERTRACE_IS_TOP_LEVEL})
option(PLAYERTRACE_BUILD_EXAMPLES "Build examples" ${PLAYERTRACE_IS_TOP_LEVEL})
option(PLAYERTRACE_ENABLE_WARNINGS "Enable a strict warning set" ON)
option(PLAYERTRACE_ENABLE_WERROR "Treat warnings as errors" OFF)
option(PLAYERTRACE_ENABLE_SANITIZERS "Enable ASan+UBSan (GCC/Clang only)" OFF)
option(PLAYERTRACE_USE_SYSTEM_JSON
       "Use find_package(nlohmann_json) instead of the vendored header" OFF)
# Installing from inside a larger build is opt-in: a parent project's `install`
# target must not silently start shipping PlayerTrace.
option(PLAYERTRACE_INSTALL "Generate install/package targets"
       ${PLAYERTRACE_IS_TOP_LEVEL})

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only choose a default build type for our own standalone builds; forcing it
# into a parent project's cache would silently change how the parent is built.
if(PLAYERTRACE_IS_TOP_LEVEL)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
  endif()
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# Flag used to silence warnings on vendored third-party C source.
if(MSVC)
  set(PLAYERTRACE_SILENCE_WARNINGS /w)
else()
  set(PLAYERTRACE_SILENCE_WARNINGS -w)
endif()

# ---------------------------------------------------------------------------
# Library
#
# v0.1 ships as a STATIC library on every platform. A shared build would need an
# export/import contract and a tested runtime-deployment story that this release
# does not have, so BUILD_SHARED_LIBS is deliberately not honored here rather
# than advertising an untested configuration.
# ---------------------------------------------------------------------------
add_library(playertrace STATIC
  src/client.cpp
  src/event.cpp
  src/event_validator.cpp
  src/event_queue.cpp
  src/file_sink.cpp
  src/id_generator.cpp
  src/json_serializer.cpp
  src/path_registry.cpp
  src/session_manager.cpp
  src/sqlite_store.cpp
  src/worker.cpp
  # Vendored SQLite amalgamation, compiled directly into the library so the
  # installed static library is fully self-contained (no external SQLite dep).
  third_party/sqlite/sqlite3.c)
add_library(playertrace::playertrace ALIAS playertrace)

# Rename every public sqlite3_* symbol to playertrace_sqlite3_* by force-
# including a generated header. Without this, an application that links its own
# SQLite would collide with (or silently bind to) our embedded copy. Symbol
# visibility flags cannot solve this: they have no effect on a static archive.
set(PLAYERTRACE_SQLITE_PREFIX_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/playertrace_sqlite_prefix.h)
if(MSVC)
  set(PLAYERTRACE_FORCE_INCLUDE "/FI${PLAYERTRACE_SQLITE_PREFIX_HEADER}")
else()
  set(PLAYERTRACE_FORCE_INCLUDE "-include${PLAYERTRACE_SQLITE_PREFIX_HEADER}")
endif()

# Silence warnings and set feature flags only for the vendored SQLite source.
set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES
  COMPILE_OPTIONS "${PLAYERTRACE_SILENCE_WARNINGS};${PLAYERTRACE_FORCE_INCLUDE}"
  COMPILE_DEFINITIONS
    "SQLITE_THREADSAFE=1;SQLITE_DQS=0;SQLITE_DEFAULT_MEMSTATUS=0;SQLITE_OMIT_LOAD_EXTENSION;SQLITE_OMIT_DEPRECATED")

# Our own translation units that include <sqlite3.h> need the same renaming.
set_source_files_properties(src/sqlite_store.cpp PROPERTIES
  COMPILE_OPTIONS "${PLAYERTRACE_FORCE_INCLUDE}")

target_include_directories(playertrace
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)

# nlohmann/json is used only internally (in .cpp files); it never appears in a
# public header. When the system package is used it must still be recorded as a
# dependency of the installed export, or a consumer's find_package() will fail
# on a missing nlohmann_json::nlohmann_json target.
if(PLAYERTRACE_USE_SYSTEM_JSON)
  find_package(nlohmann_json CONFIG REQUIRED)
  target_link_libraries(playertrace PRIVATE nlohmann_json::nlohmann_json)
  set(PLAYERTRACE_NEEDS_JSON_DEPENDENCY ON)
else()
  target_include_directories(playertrace SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  set(PLAYERTRACE_NEEDS_JSON_DEPENDENCY OFF)
endif()

target_compile_features(playertrace PUBLIC cxx_std_17)
target_link_libraries(playertrace PUBLIC Threads::Threads)

# 64-bit file offsets on POSIX. FileSink already uses ftello/fseeko/ftruncate,
# but on a 32-bit POSIX target off_t defaults to 32 bits, which would silently
# misreport the position of an NDJSON file past 2 GiB — and that position is
# what the rollback point is built from. This makes the large-file API the one
# actually compiled; file_sink.cpp additionally static_asserts the result, so an
# unsupported configuration fails to build instead of truncating at runtime.
if(UNIX)
  target_compile_definitions(playertrace PRIVATE
    _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE _LARGEFILE64_SOURCE)
endif()
if(UNIX)
  target_link_libraries(playertrace PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(playertrace PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# Strict warnings apply only to first-party sources (the vendored .c file is
# silenced above via per-source options). C++-only warnings are guarded with a
# generator expression so they are not passed to the C compile of sqlite3.c.
if(PLAYERTRACE_ENABLE_WARNINGS)
  if(MSVC)
    target_compile_options(playertrace PRIVATE /W4 /permissive-)
  else()
    target_compile_options(playertrace PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow
      $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
      $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>)
  endif()
endif()
if(PLAYERTRACE_ENABLE_WERROR)
  if(MSVC)
    target_compile_options(playertrace PRIVATE /WX)
  else()
    target_compile_options(playertrace PRIVATE -Werror)
  endif()
endif()
if(PLAYERTRACE_ENABLE_SANITIZERS AND NOT MSVC)
  target_compile_options(playertrace PUBLIC
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(playertrace PUBLIC -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PLAYERTRACE_BUILD_TESTS AND PLAYERTRACE_IS_TOP_LEVEL)
  enable_testing()

  # Catch2 (vendored amalgamated distribution). The .cpp provides a default
  # main() unless CATCH_AMALGAMATED_CUSTOM_MAIN is defined.
  add_library(playertrace_catch2 STATIC
    third_party/catch2/catch2/catch_amalgamated.cpp)
  target_include_directories(playertrace_catch2 SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/catch2)
  target_compile_features(playertrace_catch2 PUBLIC cxx_std_17)
  set_source_files_properties(
    third_party/catch2/catch2/catch_amalgamated.cpp PROPERTIES
    COMPILE_OPTIONS "${PLAYERTRACE_SILENCE_WARNINGS}")

  # A helper process used for genuine crash-recovery tests: it terminates with
  # std::_Exit() so no destructor or shutdown path runs.
  add_executable(playertrace_crash_helper tests/crash_helper.cpp)
  target_link_libraries(playertrace_crash_helper PRIVATE playertrace)

  add_executable(playertrace_tests
    tests/event_tests.cpp
    tests/serialization_tests.cpp
    tests/session_tests.cpp
    tests/queue_tests.cpp
    tests/storage_tests.cpp
    tests/client_tests.cpp
    tests/consent_tests.cpp
    tests/lifecycle_tests.cpp
    tests/persistence_tests.cpp
    tests/file_sink_tests.cpp
    tests/process_safety_tests.cpp
    tests/restart_tests.cpp
    tests/regression_tests.cpp
    tests/blocker_tests.cpp)
  target_link_libraries(playertrace_tests PRIVATE
    playertrace playertrace_catch2)
  add_dependencies(playertrace_tests playertrace_crash_helper)
  # Tests reach internal components directly; a couple of white-box storage
  # tests also use the vendored SQLite header to tamper with the database.
  target_include_directories(playertrace_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_include_directories(playertrace_tests SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
  # Tests link the renamed SQLite symbols, so they need the same force-include.
  target_compile_options(playertrace_tests PRIVATE
    "${PLAYERTRACE_FORCE_INCLUDE}")
  target_compile_definitions(playertrace_tests PRIVATE
    "PLAYERTRACE_CRASH_HELPER=\"$<TARGET_FILE:playertrace_crash_helper>\"")
  if(NOT PLAYERTRACE_USE_SYSTEM_JSON)
    target_include_directories(playertrace_tests SYSTEM PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)
  else()
    target_link_libraries(playertrace_tests PRIVATE nlohmann_json::nlohmann_json)
  endif()

  if(PLAYERTRACE_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(playertrace_tests PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(playertrace_tests PRIVATE -fsanitize=address,undefined)
  endif()

  add_test(NAME playertrace_tests COMMAND playertrace_tests)

  # A separate registration for the threading-sensitive subset (background
  # worker, concurrent producers, session and shutdown lifecycle) so it can be
  # exercised repeatedly, e.g.:
  #   ctest -R playertrace_tests_threading --repeat until-fail:50
  add_test(NAME playertrace_tests_threading
           COMMAND playertrace_tests
                   "[concurrency],[client],[session],[consent],[lifecycle],[process_safety]")
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(PLAYERTRACE_BUILD_EXAMPLES AND PLAYERTRACE_IS_TOP_LEVEL)
  add_executable(basic_tracking examples/basic_tracking.cpp)
  target_link_libraries(basic_tracking PRIVATE playertrace)

  add_executable(simulated_game examples/simulated_game.cpp)
  target_link_libraries(simulated_game PRIVATE playertrace)
endif()

# ---------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------
if(PLAYERTRACE_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS playertrace
    EXPORT playertraceTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(DIRECTORY include/playertrace
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

  # Legal notices. The distributed static library contains compiled nlohmann/json
  # code, so its MIT notice must ship alongside the project's own license and the
  # third-party summary. (Catch2 is test-only and never linked into the library;
  # SQLite is public domain and imposes no notice requirement, but both are
  # documented in THIRD_PARTY_NOTICES.md.)
  install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
  install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/LICENSE.MIT
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/third_party/nlohmann)
  install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/catch2/LICENSE.txt
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/third_party/catch2)

  install(EXPORT playertraceTargets
    FILE playertraceTargets.cmake
    NAMESPACE playertrace::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/playertrace)

  configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/playertrace-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/playertrace-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/playertrace)

  write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/playertrace-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/playertrace-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/playertrace-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/playertrace)
endif()
