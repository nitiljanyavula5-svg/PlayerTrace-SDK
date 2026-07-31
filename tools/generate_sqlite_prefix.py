#!/usr/bin/env python3
# Copyright 2026 The PlayerTrace Authors
# SPDX-License-Identifier: Apache-2.0
"""Regenerates third_party/sqlite/playertrace_sqlite_prefix.h.

PlayerTrace compiles the SQLite amalgamation into its own library. To keep those
symbols from colliding with an application's own SQLite, every public sqlite3_*
name is renamed to playertrace_sqlite3_* by force-including the generated header
ahead of sqlite3.c. Run this script after updating the vendored amalgamation:

    python tools/generate_sqlite_prefix.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Both files must be scanned. sqlite3.h covers the documented API, while
# sqlite3.c additionally declares platform-specific SQLITE_API helpers (the
# sqlite3_win32_* family, for example) that appear in no header but are still
# emitted with external linkage — and so still collide when co-linked.
SOURCES = [
    os.path.join(ROOT, "third_party", "sqlite", "sqlite3.h"),
    os.path.join(ROOT, "third_party", "sqlite", "sqlite3.c"),
]
OUTPUT = os.path.join(ROOT, "third_party", "sqlite",
                      "playertrace_sqlite_prefix.h")

# Objects that are not always declared with a plain SQLITE_API prefix.
ALWAYS_INCLUDE = [
    "sqlite3_version",
    "sqlite3_temp_directory",
    "sqlite3_data_directory",
]

BANNER = """// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
//
// GENERATED FILE - do not edit by hand.
// Regenerate with tools/generate_sqlite_prefix.py after updating the vendored
// SQLite amalgamation.
//
// PlayerTrace compiles the SQLite amalgamation directly into its own library so
// that a clean checkout builds with no external dependency. Without renaming,
// every public sqlite3_* symbol would be defined globally in libplayertrace,
// and an application that also links its own SQLite would hit duplicate symbols
// or silently bind to the wrong copy depending on link order.
//
// This header is force-included ahead of sqlite3.c (and of our own sqlite3.h
// consumers) so the entire public surface becomes playertrace_sqlite3_*. The
// renaming is textual, applies equally to static and shared libraries, and is
// portable across MSVC, GCC and Clang - unlike visibility flags, which do
// nothing for a static archive.
#ifndef PLAYERTRACE_SQLITE_SYMBOL_PREFIX_H
#define PLAYERTRACE_SQLITE_SYMBOL_PREFIX_H

"""


def main() -> int:
    names = set(ALWAYS_INCLUDE)
    pattern = re.compile(
        r"SQLITE_API\b[^;{]*?\b(sqlite3_[A-Za-z0-9_]+)\s*(\(|\[|;|=)", re.S)

    for path in SOURCES:
        if not os.path.exists(path):
            sys.stderr.write("missing %s\n" % path)
            return 1
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            source = handle.read()
        for match in pattern.finditer(source):
            names.add(match.group(1))

    out = io.StringIO()
    out.write(BANNER)
    for name in sorted(names):
        out.write("#define %s playertrace_%s\n" % (name, name))
    out.write("\n#endif  // PLAYERTRACE_SQLITE_SYMBOL_PREFIX_H\n")

    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(out.getvalue())
    sys.stdout.write("wrote %d renames to %s\n" % (len(names), OUTPUT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
