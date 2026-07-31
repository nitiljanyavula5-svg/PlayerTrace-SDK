# Copyright 2026 The PlayerTrace Authors
# SPDX-License-Identifier: Apache-2.0
#
# A standalone project that consumes an installed PlayerTrace via
# find_package(). Configure it with -Dplayertrace_DIR or CMAKE_PREFIX_PATH
# pointing at the install tree. See CI for the exact invocation.
cmake_minimum_required(VERSION 3.16)
project(playertrace_consumer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(playertrace CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE playertrace::playertrace)

enable_testing()
add_test(NAME consumer COMMAND consumer)

# ---------------------------------------------------------------------------
# Install-manifest test: the distributed package must carry the project license
# and the third-party notices, including the MIT notice for the nlohmann/json
# code that is compiled into the static library.
# ---------------------------------------------------------------------------
get_filename_component(_pt_cmake_dir "${playertrace_DIR}" ABSOLUTE)
get_filename_component(_pt_lib_dir "${_pt_cmake_dir}/../.." ABSOLUTE)
get_filename_component(PLAYERTRACE_INSTALL_PREFIX "${_pt_lib_dir}/.." ABSOLUTE)

add_test(NAME installed_legal_files
  COMMAND ${CMAKE_COMMAND}
          -DPREFIX=${PLAYERTRACE_INSTALL_PREFIX}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_legal_files.cmake)
