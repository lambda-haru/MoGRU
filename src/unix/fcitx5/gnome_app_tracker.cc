// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#include "unix/fcitx5/gnome_app_tracker.h"

#include <fcitx-utils/dbus/matchrule.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "unix/fcitx5/app_name_resolver.h"

namespace fcitx {
namespace {

constexpr uint64_t kGetFocusedAppTimeoutUsec = 2000000;  // 2 seconds.

}  // namespace

GnomeAppTracker::GnomeAppTracker(dbus::Bus* bus, ChangeCallback on_changed)
    : bus_(bus), on_changed_(std::move(on_changed)) {
  if (bus_ == nullptr || !bus_->isOpen()) {
    return;
  }
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
  watcher_entry_.reset();
  watcher_.reset();
}

void GnomeAppTracker::HandleServiceOwnerChanged(const std::string& new_owner) {
  pending_get_slot_.reset();
  signal_match_slot_.reset();
  current_owner_ = new_owner;
  ++generation_;
  signal_seen_in_generation_ = false;

  if (new_owner.empty() || bus_ == nullptr || !bus_->isOpen()) {
    UpdateFocusedApp("");
    return;
  }

  const dbus::MatchRule rule(new_owner, kObjectPath, kInterface,
                             kSignalFocusedAppChanged);
  const uint64_t gen = generation_;
  signal_match_slot_ =
      bus_->addMatch(rule, [this, gen](dbus::Message& msg) {
        return HandleFocusedAppSignal(msg, gen);
      });

  auto call = bus_->createMethodCall(new_owner.c_str(), kObjectPath, kInterface,
                                     kMethodGetFocusedApp);
  pending_get_slot_ =
      call.callAsync(kGetFocusedAppTimeoutUsec,
                     [this, gen](dbus::Message& reply) {
                       return HandleGetFocusedAppReply(reply, gen);
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
  signal_seen_in_generation_ = true;
  pending_get_slot_.reset();
  UpdateFocusedApp(app_name);
  return false;
}

bool GnomeAppTracker::HandleGetFocusedAppReply(dbus::Message& reply,
                                               uint64_t generation) {
  if (generation != generation_ || signal_seen_in_generation_) {
    return true;
  }
  if (!reply.isError()) {
    std::string app_name;
    if (reply >> app_name) {
      UpdateFocusedApp(app_name);
    }
  }
  return true;
}

void GnomeAppTracker::UpdateFocusedApp(const std::string& raw_app_name) {
  std::string normalized = NormalizeUsableAppName(raw_app_name);
  if (focused_app_ == normalized) {
    return;
  }
  focused_app_ = std::move(normalized);
  if (on_changed_) {
    on_changed_(focused_app_);
  }
}

}  // namespace fcitx
