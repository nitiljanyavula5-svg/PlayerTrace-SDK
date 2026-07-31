# Copyright 2026 The PlayerTrace Authors
# SPDX-License-Identifier: Apache-2.0
#
# A minimal stand-in for the real nlohmann_json CMake package.
#
# It exists so the PLAYERTRACE_USE_SYSTEM_JSON=ON code path — and, crucially,
# the find_dependency() that the installed playertrace-config must emit — can be
# exercised without vcpkg or a system install. It simply exposes the vendored
# single header through the same imported-target name the real package uses.
#
# Point CMAKE_PREFIX_PATH at this directory:
#   -DPLAYERTRACE_USE_SYSTEM_JSON=ON
#   -DCMAKE_PREFIX_PATH=<repo>/packaging/test-fixtures/nlohmann_json

if(NOT TARGET nlohmann_json::nlohmann_json)
  get_filename_component(_fixture_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
  get_filename_component(_repo_root "${_fixture_dir}/../../.." ABSOLUTE)
  set(_json_include "${_repo_root}/third_party/nlohmann")

  if(NOT EXISTS "${_json_include}/nlohmann/json.hpp")
    message(FATAL_ERROR
      "test fixture could not find the vendored nlohmann/json header at "
      "${_json_include}")
  endif()

  add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
  set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_json_include}")
endif()

set(nlohmann_json_FOUND TRUE)
