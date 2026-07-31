# Copyright 2026 The PlayerTrace Authors
# SPDX-License-Identifier: Apache-2.0
#
# Proves that an application can link its OWN SQLite alongside PlayerTrace.
#
# PlayerTrace embeds the SQLite amalgamation. Before the symbols were renamed,
# this configuration produced duplicate-symbol errors or, worse, silently bound
# one library's calls to the other's copy depending on link order.
#
# Configure with:
#   -Dplayertrace_DIR=<prefix>/lib/cmake/playertrace
#   -DPLAYERTRACE_SQLITE_AMALGAMATION=<path to sqlite3.c>
cmake_minimum_required(VERSION 3.16)
project(playertrace_colink_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(playertrace CONFIG REQUIRED)

if(NOT DEFINED PLAYERTRACE_SQLITE_AMALGAMATION)
  message(FATAL_ERROR
    "Set PLAYERTRACE_SQLITE_AMALGAMATION to a sqlite3.c to co-link against")
endif()
get_filename_component(_sqlite_dir "${PLAYERTRACE_SQLITE_AMALGAMATION}" DIRECTORY)

# The application's own SQLite, built with default (unprefixed) symbol names —
# exactly what a real consumer would link.
add_library(app_sqlite STATIC "${PLAYERTRACE_SQLITE_AMALGAMATION}")
target_include_directories(app_sqlite PUBLIC "${_sqlite_dir}")
if(MSVC)
  target_compile_options(app_sqlite PRIVATE /w)
else()
  target_compile_options(app_sqlite PRIVATE -w)
endif()
target_compile_definitions(app_sqlite PRIVATE
  SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
if(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(app_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

add_executable(colink main.cpp)
# Deliberately link PlayerTrace first and the app's SQLite second; then the
# reverse order is covered by the second target below. Both must work.
target_link_libraries(colink PRIVATE playertrace::playertrace app_sqlite)

add_executable(colink_reversed main.cpp)
target_link_libraries(colink_reversed PRIVATE app_sqlite playertrace::playertrace)

enable_testing()
add_test(NAME colink COMMAND colink)
add_test(NAME colink_reversed COMMAND colink_reversed)
