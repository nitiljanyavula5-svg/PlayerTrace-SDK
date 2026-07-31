// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// Internal header — NOT installed and NOT part of the public API. Lets tests
// inject deterministic I/O faults into FileSink without adding a test seam to
// the public headers.
#ifndef PLAYERTRACE_INTERNAL_FILE_SINK_INTERNAL_HPP
#define PLAYERTRACE_INTERNAL_FILE_SINK_INTERNAL_HPP

#include <functional>

#include "playertrace/file_sink.hpp"

namespace playertrace {

/// Returns true to make the named FileSink operation fail. Operation names are
/// stable strings: "open", "write", "partial_write", "flush", "sync",
/// "truncate".
using FileSinkFaultHook = std::function<bool(const char* operation)>;

struct FileSinkInternal {
  static void set_fault_hook(FileSink& sink, FileSinkFaultHook hook);
};

}  // namespace playertrace

#endif  // PLAYERTRACE_INTERNAL_FILE_SINK_INTERNAL_HPP
