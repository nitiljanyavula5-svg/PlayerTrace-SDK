// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_VERSION_HPP
#define PLAYERTRACE_VERSION_HPP

#define PLAYERTRACE_VERSION_MAJOR 0
#define PLAYERTRACE_VERSION_MINOR 1
#define PLAYERTRACE_VERSION_PATCH 0
#define PLAYERTRACE_VERSION_STRING "0.1.0"

namespace playertrace {

/// Returns the SDK version string, e.g. "0.1.0".
const char* version() noexcept;

}  // namespace playertrace

#endif  // PLAYERTRACE_VERSION_HPP
