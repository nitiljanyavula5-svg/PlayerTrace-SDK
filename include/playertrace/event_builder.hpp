// Copyright 2026 The PlayerTrace Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PLAYERTRACE_EVENT_BUILDER_HPP
#define PLAYERTRACE_EVENT_BUILDER_HPP

#include <string>
#include <utility>

#include "event.hpp"

namespace playertrace {

/// Small fluent helper for building an event name + properties when the
/// brace-initializer form is inconvenient (e.g. conditional properties).
///
///   client->track(EventBuilder("level_completed")
///                     .set("level_id", "forest_01")
///                     .set("deaths", std::int64_t{3}));
///
/// This is purely a convenience over `track(name, Properties)`; it does not
/// perform validation (that happens inside track()).
class EventBuilder {
 public:
  explicit EventBuilder(std::string name) : name_(std::move(name)) {}

  EventBuilder& set(std::string key, PropertyValue value) {
    properties_.emplace_back(std::move(key), std::move(value));
    return *this;
  }

  const std::string& name() const noexcept { return name_; }
  const Properties& properties() const noexcept { return properties_; }

 private:
  std::string name_;
  Properties properties_;
};

}  // namespace playertrace

#endif  // PLAYERTRACE_EVENT_BUILDER_HPP
