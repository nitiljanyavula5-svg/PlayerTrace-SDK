# Copyright 2026 The PlayerTrace Authors
# SPDX-License-Identifier: Apache-2.0
#
# A parent project that embeds PlayerTrace via add_subdirectory(). It asserts
# that being embedded does NOT change the parent's configuration or install set.
#
# Configure with -DPLAYERTRACE_SOURCE_DIR=<path to the playertrace checkout>.
cmake_minimum_required(VERSION 3.16)
project(playertrace_parent_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED PLAYERTRACE_SOURCE_DIR)
  message(FATAL_ERROR "Set PLAYERTRACE_SOURCE_DIR to the playertrace checkout")
endif()

# Record the parent's build type before embedding PlayerTrace.
set(PARENT_BUILD_TYPE_BEFORE "${CMAKE_BUILD_TYPE}")

add_subdirectory("${PLAYERTRACE_SOURCE_DIR}" playertrace-build)

# 1. PlayerTrace must not have forced a build type into the parent's cache.
if(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "${PARENT_BUILD_TYPE_BEFORE}")
  message(FATAL_ERROR
    "PlayerTrace changed CMAKE_BUILD_TYPE from "
    "'${PARENT_BUILD_TYPE_BEFORE}' to '${CMAKE_BUILD_TYPE}'")
endif()

# 2. As a subproject it must not build tests or examples by default.
foreach(_unwanted playertrace_tests basic_tracking simulated_game)
  if(TARGET ${_unwanted})
    message(FATAL_ERROR
      "PlayerTrace defined '${_unwanted}' as a subproject; tests and examples "
      "must default to OFF when embedded")
  endif()
endforeach()

# 3. The library target itself must exist and be usable.
if(NOT TARGET playertrace::playertrace)
  message(FATAL_ERROR "playertrace::playertrace target was not defined")
endif()

add_executable(parent_app main.cpp)
target_link_libraries(parent_app PRIVATE playertrace::playertrace)

# 4. The parent's own install set must not contain PlayerTrace artifacts.
install(TARGETS parent_app RUNTIME DESTINATION bin)

enable_testing()
add_test(NAME parent_app COMMAND parent_app)
# $<CONFIG> resolves to the configuration ctest is actually running, so the
# isolation check installs what was built rather than whatever a multi-config
# generator would otherwise default to.
add_test(NAME parent_install_isolation
  COMMAND ${CMAKE_COMMAND}
          -DBUILD_DIR=${CMAKE_BINARY_DIR}
          -DBUILD_CONFIG=$<CONFIG>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_install_isolation.cmake)
