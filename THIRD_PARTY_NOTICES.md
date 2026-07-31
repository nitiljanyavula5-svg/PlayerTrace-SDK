# Third-Party Notices

PlayerTrace itself is licensed under the Apache License 2.0 (see [LICENSE](LICENSE)).

This project **vendors** the third-party components listed below under
`third_party/` so that a clean checkout builds without a package manager or
network access. Each component retains the copyright and license notices shipped
by its authors; those notices have not been modified or removed.

---

## SQLite

| | |
|---|---|
| **Version** | 3.53.3 (amalgamation) |
| **Files** | `third_party/sqlite/sqlite3.c`, `third_party/sqlite/sqlite3.h` |
| **Purpose** | Durable local storage of pending telemetry events. |
| **Upstream** | https://www.sqlite.org/ |
| **Source archive** | https://sqlite.org/2026/sqlite-amalgamation-3530300.zip |
| **Licensing** | **Public domain — not a conventional open-source license.** |
| **Distribution impact** | Compiled directly into the PlayerTrace library, so it is present in distributed binaries. |

SQLite is **not** distributed under an OSI-style license such as MIT, BSD, or
Apache. Its authors have released it into the **public domain** and disclaim
copyright. Accordingly there is no upstream `LICENSE` file to reproduce. In place
of a legal notice the source carries the SQLite "blessing", which is retained
verbatim at the top of both vendored files:

> The author disclaims copyright to this source code. In place of a legal
> notice, here is a blessing:
>
> May you do good and not evil.
> May you find forgiveness for yourself and forgive others.
> May you share freely, never taking more than you give.

Because the code is public domain, it imposes no attribution, notice-retention,
or reciprocity obligations on PlayerTrace or on downstream users. It is
documented here for transparency, not because a license requires it. See
https://www.sqlite.org/copyright.html for the upstream statement.

---

## JSON for Modern C++ (nlohmann/json)

| | |
|---|---|
| **Version** | 3.11.3 (single-include header) |
| **Files** | `third_party/nlohmann/nlohmann/json.hpp` |
| **License file** | [`third_party/nlohmann/LICENSE.MIT`](third_party/nlohmann/LICENSE.MIT) |
| **Purpose** | Internal JSON serialization of events. Never exposed through any public PlayerTrace header. |
| **Upstream** | https://github.com/nlohmann/json |
| **License** | MIT |
| **Copyright** | © 2013–2022 Niels Lohmann |
| **Distribution impact** | Header-only; its code is inlined into the compiled PlayerTrace library, so the MIT notice obligation applies to distributed binaries. |

The vendored header carries an `SPDX-License-Identifier: MIT` marker but does not
embed the full license text, so the upstream `LICENSE.MIT` file is included
alongside it to satisfy the MIT requirement that the copyright and permission
notice accompany all copies.

---

## Catch2

| | |
|---|---|
| **Version** | 3.7.1 (amalgamated distribution) |
| **Files** | `third_party/catch2/catch2/catch_amalgamated.hpp`, `third_party/catch2/catch2/catch_amalgamated.cpp` |
| **License file** | [`third_party/catch2/LICENSE.txt`](third_party/catch2/LICENSE.txt) |
| **Purpose** | Unit-test framework. **Test builds only** — not linked into the installed library and not present in distributed binaries. |
| **Upstream** | https://github.com/catchorg/Catch2 |
| **License** | Boost Software License 1.0 (BSL-1.0) |
| **Copyright** | © Catch2 Authors |
| **Distribution impact** | Consumed only by the `playertrace_tests` target; excluded from the install/package targets. |

The vendored sources reference an "accompanying file LICENSE.txt". That file is
included here so the reference resolves and the BSL-1.0 notice requirement is
satisfied.

---

## Summary

| Component | Version | License | In distributed binaries? |
|-----------|---------|---------|--------------------------|
| SQLite | 3.53.3 | Public domain (no license) | Yes |
| nlohmann/json | 3.11.3 | MIT | Yes (inlined) |
| Catch2 | 3.7.1 | BSL-1.0 | No (tests only) |

Updating a vendored dependency should include refreshing its entry above and its
accompanying license file.
