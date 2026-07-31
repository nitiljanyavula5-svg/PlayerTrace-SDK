// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_CONSENT_HPP
#define PLAYERTRACE_CONSENT_HPP

namespace playertrace {

/// Runtime consent gate for collection.
///
/// The default in Config is `Unknown` — collection NEVER happens unless the
/// integrating developer explicitly sets `Granted`. Transitioning away from
/// `Granted` (to `Denied` or `Unknown`) rejects new events immediately,
/// discards queued events, and purges pending unsent events (see
/// docs/privacy.md).
enum class ConsentState {
  Unknown = 0,  ///< No decision recorded. Collection is blocked.
  Denied,       ///< Player opted out. Collection is blocked; pending purged.
  Granted       ///< Player opted in. Collection permitted.
};

const char* to_string(ConsentState state) noexcept;

}  // namespace playertrace

#endif  // PLAYERTRACE_CONSENT_HPP
