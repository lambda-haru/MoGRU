// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#include "unix/fcitx5/gnome_app_tracker.h"

#include <fcitx-utils/dbus/matchrule.h>
#include <fcitx-utils/dbus/variant.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "unix/fcitx5/app_name_resolver.h"

namespace fcitx {
namespace {

constexpr uint64_t kGetFocusedAppTimeoutUsec = 2000000;  // 2 seconds.

void RegisterDBusVariantTypesOnce() {
  static std::once_flag once;
  std::call_once(once, []() {
    dbus::registerVariantType<std::string>();
    dbus::registerVariantType<int32_t>();
    dbus::registerVariantType<uint32_t>();
    dbus::registerVariantType<int64_t>();
    dbus::registerVariantType<uint64_t>();
    dbus::registerVariantType<bool>();
    dbus::registerVariantType<double>();
  });
}

bool TryExtractUint64(const dbus::Variant& variant, uint64_t* out) {
  if (out == nullptr) {
    return false;
  }
  const std::string& sig = variant.signature();
  if (sig == "t") {
    *out = variant.dataAs<uint64_t>();
    return true;
  }
  if (sig == "u") {
    *out = variant.dataAs<uint32_t>();
    return true;
  }
  if (sig == "x") {
    const int64_t val = variant.dataAs<int64_t>();
    *out = val > 0 ? static_cast<uint64_t>(val) : 0;
    return true;
  }
  if (sig == "i") {
    const int32_t val = variant.dataAs<int32_t>();
    *out = val > 0 ? static_cast<uint64_t>(val) : 0;
    return true;
  }
  return false;
}

void ExtractAppAndWindowFromDict(
    const std::vector<dbus::DictEntry<std::string, dbus::Variant>>& entries,
    std::string* out_app_name,
    uint64_t* out_window_id) {
  for (const auto& entry : entries) {
    if (out_app_name != nullptr && entry.key() == "app_name" &&
        entry.value().signature() == "s") {
      *out_app_name = entry.value().dataAs<std::string>();
    } else if (out_window_id != nullptr && entry.key() == "window_id") {
      TryExtractUint64(entry.value(), out_window_id);
    }
  }
}

std::string FormatGnomeWindowId(uint64_t raw_window_id) {
  if (raw_window_id == 0) {
    return "";
  }
  return absl::StrCat("gnome:", raw_window_id);
}

}  // namespace

GnomeAppTracker::GnomeAppTracker(dbus::Bus* bus,
                                 ChangeCallback on_changed,
                                 WindowClosedCallback on_window_closed)
    : bus_(bus),
      on_changed_(std::move(on_changed)),
      on_window_closed_(std::move(on_window_closed)) {
  if (bus_ == nullptr || !bus_->isOpen()) {
    return;
  }
  RegisterDBusVariantTypesOnce();
  watcher_ = std::make_unique<dbus::ServiceWatcher>(*bus_);
  watcher_entry_ = watcher_->watchService(
      kBusName, [this](const std::string& /*service*/,
                       const std::string& /*old_owner*/,
                       const std::string& new_owner) {
        HandleServiceOwnerChanged(new_owner);
      });
}

GnomeAppTracker::~GnomeAppTracker() {
  pending_get_slot_.reset();
  signal_match_slot_.reset();
  window_closed_match_slot_.reset();
  watcher_entry_.reset();
  watcher_.reset();
}

void GnomeAppTracker::HandleServiceOwnerChanged(const std::string& new_owner) {
  pending_get_slot_.reset();
  signal_match_slot_.reset();
  window_closed_match_slot_.reset();
  current_owner_ = new_owner;
  ++generation_;
  signal_seen_in_generation_ = false;

  if (new_owner.empty() || bus_ == nullptr || !bus_->isOpen()) {
    UpdateFocusedState("", 0);
    return;
  }

  const uint64_t gen = generation_;
  const dbus::MatchRule app_rule(new_owner, kObjectPath, kInterface,
                                 kSignalFocusedAppChanged);
  signal_match_slot_ =
      bus_->addMatch(app_rule, [this, gen](dbus::Message& msg) {
        return HandleFocusedAppSignal(msg, gen);
      });

  const dbus::MatchRule close_rule(new_owner, kObjectPath, kInterface,
                                   kSignalWindowClosed);
  window_closed_match_slot_ =
      bus_->addMatch(close_rule, [this, gen](dbus::Message& msg) {
        return HandleWindowClosedSignal(msg, gen);
      });

  auto call = bus_->createMethodCall(new_owner.c_str(), kObjectPath, kInterface,
                                     kMethodGetFocusedAppInfo);
  pending_get_slot_ =
      call.callAsync(kGetFocusedAppTimeoutUsec,
                     [this, gen](dbus::Message& reply) {
                       return HandleGetFocusedAppInfoReply(reply, gen);
                     });
}

bool GnomeAppTracker::HandleFocusedAppSignal(dbus::Message& msg,
                                             uint64_t generation) {
  if (generation != generation_ || msg.isError()) {
    return false;
  }
  std::string app_name;
  if (!(msg >> app_name)) {
    return false;
  }
  uint64_t window_id = 0;
  std::vector<dbus::DictEntry<std::string, dbus::Variant>> entries;
  if (msg >> entries) {
    ExtractAppAndWindowFromDict(entries, nullptr, &window_id);
  }
  signal_seen_in_generation_ = true;
  pending_get_slot_.reset();
  UpdateFocusedState(app_name, window_id);
  return false;
}

bool GnomeAppTracker::HandleWindowClosedSignal(dbus::Message& msg,
                                               uint64_t generation) {
  if (generation != generation_ || msg.isError()) {
    return false;
  }
  uint64_t window_id = 0;
  if (!(msg >> window_id) || window_id == 0) {
    return false;
  }
  if (on_window_closed_) {
    on_window_closed_(FormatGnomeWindowId(window_id));
  }
  return false;
}

bool GnomeAppTracker::HandleGetFocusedAppInfoReply(dbus::Message& reply,
                                                   uint64_t generation) {
  if (generation != generation_ || signal_seen_in_generation_) {
    return true;
  }
  if (!reply.isError()) {
    std::vector<dbus::DictEntry<std::string, dbus::Variant>> entries;
    if (reply >> entries) {
      std::string app_name;
      uint64_t window_id = 0;
      ExtractAppAndWindowFromDict(entries, &app_name, &window_id);
      UpdateFocusedState(app_name, window_id);
    }
  }
  return true;
}

void GnomeAppTracker::UpdateFocusedState(const std::string& raw_app_name,
                                         uint64_t raw_window_id) {
  std::string normalized_app = NormalizeUsableAppName(raw_app_name);
  std::string normalized_win = FormatGnomeWindowId(raw_window_id);
  if (focused_app_ == normalized_app &&
      focused_window_id_ == normalized_win) {
    return;
  }
  focused_app_ = std::move(normalized_app);
  focused_window_id_ = std::move(normalized_win);
  if (on_changed_) {
    on_changed_(focused_app_, focused_window_id_);
  }
}

}  // namespace fcitx
