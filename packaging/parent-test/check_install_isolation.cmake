# Copyright 2026 The PlayerTrace Authors
# SPDX-License-Identifier: Apache-2.0
#
# Installs the parent project into a scratch prefix and fails if PlayerTrace
# artifacts leaked into it. Embedding a library must not silently extend the
# parent's install set.

if(NOT DEFINED BUILD_DIR)
  message(FATAL_ERROR "BUILD_DIR must be set")
endif()

set(_prefix "${BUILD_DIR}/isolation-prefix")
file(REMOVE_RECURSE "${_prefix}")

execute_process(
  COMMAND ${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${_prefix}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "parent install failed:\n${_out}\n${_err}")
endif()

# PlayerTrace headers, library, package config and notices must all be absent.
set(_forbidden_globs
  "${_prefix}/include/playertrace/*"
  "${_prefix}/lib/cmake/playertrace/*"
  "${_prefix}/lib/*playertrace*"
  "${_prefix}/share/doc/playertrace/*")

set(_leaked "")
foreach(_glob IN LISTS _forbidden_globs)
  file(GLOB _hits "${_glob}")
  if(_hits)
    list(APPEND _leaked ${_hits})
  endif()
endforeach()

if(_leaked)
  string(REPLACE ";" "\n  " _pretty "${_leaked}")
  message(FATAL_ERROR
    "Parent install leaked PlayerTrace artifacts:\n  ${_pretty}")
endif()

message(STATUS "Parent install isolation verified (${_prefix})")
