# Copyright 2026 The PlayerTrace Authors
# SPDX-License-Identifier: Apache-2.0
#
# Fails if an installed PlayerTrace package is missing required legal files.
# Invoked as: cmake -DPREFIX=<install-prefix> -P check_legal_files.cmake

if(NOT DEFINED PREFIX)
  message(FATAL_ERROR "PREFIX must be set to the install prefix")
endif()

# GNUInstallDirs puts docs under share/doc/<project>; accept the common layouts
# so the check does not become platform-specific.
set(_candidates
  "${PREFIX}/share/doc/playertrace"
  "${PREFIX}/share/doc/PlayerTrace"
  "${PREFIX}/doc/playertrace")

set(_docdir "")
foreach(_dir IN LISTS _candidates)
  if(EXISTS "${_dir}")
    set(_docdir "${_dir}")
    break()
  endif()
endforeach()

if(_docdir STREQUAL "")
  message(FATAL_ERROR
    "No PlayerTrace documentation directory found under '${PREFIX}'. "
    "The install must ship LICENSE and THIRD_PARTY_NOTICES.md.")
endif()

set(_required
  "${_docdir}/LICENSE"
  "${_docdir}/THIRD_PARTY_NOTICES.md"
  # The static library contains compiled nlohmann/json code, so the MIT notice
  # must accompany the distribution.
  "${_docdir}/third_party/nlohmann/LICENSE.MIT")

set(_missing "")
foreach(_file IN LISTS _required)
  if(NOT EXISTS "${_file}")
    list(APPEND _missing "${_file}")
  endif()
endforeach()

if(_missing)
  string(REPLACE ";" "\n  " _pretty "${_missing}")
  message(FATAL_ERROR
    "Installed package is missing required legal files:\n  ${_pretty}")
endif()

# The Apache license text must actually be the license, not an empty placeholder.
file(READ "${_docdir}/LICENSE" _license_text)
string(FIND "${_license_text}" "Apache License" _apache_pos)
if(_apache_pos EQUAL -1)
  message(FATAL_ERROR "Installed LICENSE does not contain the Apache License text")
endif()

file(READ "${_docdir}/third_party/nlohmann/LICENSE.MIT" _mit_text)
string(FIND "${_mit_text}" "MIT License" _mit_pos)
if(_mit_pos EQUAL -1)
  message(FATAL_ERROR "Installed nlohmann LICENSE.MIT does not contain the MIT text")
endif()

message(STATUS "Installed legal files verified in ${_docdir}")
