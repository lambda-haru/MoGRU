// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#include "unix/fcitx5/gnome_app_tracker.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "testing/gunit.h"

namespace fcitx {

class GnomeAppTrackerTestPeer {
 public:
  static void UpdateFocusedState(GnomeAppTracker* tracker,
                                 const std::string& app,
                                 uint64_t window_id) {
    tracker->UpdateFocusedState(app, window_id);
  }
  static void HandleServiceOwnerChanged(GnomeAppTracker* tracker,
                                        const std::string& owner) {
    tracker->HandleServiceOwnerChanged(owner);
  }
};

namespace {

TEST(GnomeAppTrackerTest, NullBusIsSafeNoOp) {
  bool called = false;
  GnomeAppTracker tracker(
      nullptr,
      [&called](const std::string&, const std::string&) { called = true; });
  EXPECT_EQ(tracker.focused_app(), "");
  EXPECT_EQ(tracker.focused_window_id(), "");
  EXPECT_FALSE(tracker.is_service_available());
  EXPECT_FALSE(called);
}

TEST(GnomeAppTrackerTest, DeduplicatesAndNotifiesOnSameAppWindowChange) {
  std::vector<std::pair<std::string, std::string>> events;
  GnomeAppTracker tracker(
      nullptr, [&events](const std::string& app, const std::string& win_id) {
        events.emplace_back(app, win_id);
      });

  GnomeAppTrackerTestPeer::UpdateFocusedState(&tracker, "  code  ", 101);
  // Duplicate state is suppressed.
  GnomeAppTrackerTestPeer::UpdateFocusedState(&tracker, "code", 101);
  // Switching to another window of the same app MUST notify.
  GnomeAppTrackerTestPeer::UpdateFocusedState(&tracker, "code", 102);
  GnomeAppTrackerTestPeer::UpdateFocusedState(&tracker, "firefox", 200);
  // Compositor proxy name is filtered while preserving non-zero window ID.
  GnomeAppTrackerTestPeer::UpdateFocusedState(&tracker, "gnome-shell", 0);
  GnomeAppTrackerTestPeer::UpdateFocusedState(&tracker, "xwayland", 0);

  ASSERT_EQ(events.size(), 4);
  EXPECT_EQ(events[0], std::make_pair(std::string("code"), std::string("gnome:101")));
  EXPECT_EQ(events[1], std::make_pair(std::string("code"), std::string("gnome:102")));
  EXPECT_EQ(events[2], std::make_pair(std::string("firefox"), std::string("gnome:200")));
  EXPECT_EQ(events[3], std::make_pair(std::string(""), std::string("")));
  EXPECT_EQ(tracker.focused_app(), "");
  EXPECT_EQ(tracker.focused_window_id(), "");
}

TEST(GnomeAppTrackerTest, ClearingOwnerResetsFocusedState) {
  std::vector<std::pair<std::string, std::string>> events;
  GnomeAppTracker tracker(
      nullptr, [&events](const std::string& app, const std::string& win_id) {
        events.emplace_back(app, win_id);
      });

  GnomeAppTrackerTestPeer::UpdateFocusedState(&tracker, "code", 55);
  GnomeAppTrackerTestPeer::HandleServiceOwnerChanged(&tracker, "");

  EXPECT_EQ(tracker.focused_app(), "");
  EXPECT_EQ(tracker.focused_window_id(), "");
  EXPECT_FALSE(tracker.is_service_available());
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[1], std::make_pair(std::string(""), std::string("")));
}

}  // namespace
}  // namespace fcitx
