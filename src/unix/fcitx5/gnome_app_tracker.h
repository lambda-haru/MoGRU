// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#ifndef MOZC_UNIX_FCITX5_GNOME_APP_TRACKER_H_
#define MOZC_UNIX_FCITX5_GNOME_APP_TRACKER_H_

#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/dbus/message.h>
#include <fcitx-utils/dbus/servicewatcher.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fcitx {

// Subscribes to the MoGRU GNOME Shell extension over Session D-Bus to track
// the currently focused application name on GNOME + Wayland without blocking
// key event handling.
class GnomeAppTracker {
 public:
  static constexpr const char* kBusName =
      "io.github.lambda_haru.MoGRU.AppTracker";
  static constexpr const char* kObjectPath =
      "/io/github/lambda_haru/MoGRU/AppTracker";
  static constexpr const char* kInterface =
      "io.github.lambda_haru.MoGRU.AppTracker";
  static constexpr const char* kMethodGetFocusedApp = "GetFocusedApp";
  static constexpr const char* kSignalFocusedAppChanged = "FocusedAppChanged";

  using ChangeCallback = std::function<void(const std::string&)>;

  GnomeAppTracker(dbus::Bus* bus, ChangeCallback on_changed);
  ~GnomeAppTracker();

  GnomeAppTracker(const GnomeAppTracker&) = delete;
  GnomeAppTracker& operator=(const GnomeAppTracker&) = delete;

  const std::string& focused_app() const { return focused_app_; }
  bool is_service_available() const { return !current_owner_.empty(); }

 private:
  friend class GnomeAppTrackerTestPeer;

  void HandleServiceOwnerChanged(const std::string& new_owner);
  bool HandleFocusedAppSignal(dbus::Message& msg, uint64_t generation);
  bool HandleGetFocusedAppReply(dbus::Message& reply, uint64_t generation);
  void UpdateFocusedApp(const std::string& raw_app_name);

  dbus::Bus* bus_ = nullptr;
  ChangeCallback on_changed_;
  std::unique_ptr<dbus::ServiceWatcher> watcher_;
  std::unique_ptr<dbus::ServiceWatcherEntry> watcher_entry_;
  std::unique_ptr<dbus::Slot> signal_match_slot_;
  std::unique_ptr<dbus::Slot> pending_get_slot_;
  std::string current_owner_;
  std::string focused_app_;
  uint64_t generation_ = 0;
  bool signal_seen_in_generation_ = false;
};

}  // namespace fcitx

#endif  // MOZC_UNIX_FCITX5_GNOME_APP_TRACKER_H_
