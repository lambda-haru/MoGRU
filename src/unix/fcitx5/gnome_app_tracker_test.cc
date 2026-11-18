// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#include "unix/fcitx5/gnome_app_tracker.h"

#include <string>
#include <vector>

#include "testing/gunit.h"

namespace fcitx {

class GnomeAppTrackerTestPeer {
 public:
  static void UpdateFocusedApp(GnomeAppTracker* tracker,
                               const std::string& app) {
    tracker->UpdateFocusedApp(app);
  }
  static void HandleServiceOwnerChanged(GnomeAppTracker* tracker,
                                        const std::string& owner) {
    tracker->HandleServiceOwnerChanged(owner);
  }
};

namespace {

TEST(GnomeAppTrackerTest, NullBusIsSafeNoOp) {
  bool called = false;
  GnomeAppTracker tracker(nullptr,
                          [&called](const std::string&) { called = true; });
  EXPECT_EQ(tracker.focused_app(), "");
  EXPECT_FALSE(tracker.is_service_available());
  EXPECT_FALSE(called);
}

TEST(GnomeAppTrackerTest, DeduplicatesAndFiltersProxyNames) {
  std::vector<std::string> events;
  GnomeAppTracker tracker(
      nullptr, [&events](const std::string& app) { events.push_back(app); });

  GnomeAppTrackerTestPeer::UpdateFocusedApp(&tracker, "  code  ");
  GnomeAppTrackerTestPeer::UpdateFocusedApp(&tracker, "code");
  GnomeAppTrackerTestPeer::UpdateFocusedApp(&tracker, "firefox");
  GnomeAppTrackerTestPeer::UpdateFocusedApp(&tracker, "gnome-shell");
  GnomeAppTrackerTestPeer::UpdateFocusedApp(&tracker, "xwayland");
  GnomeAppTrackerTestPeer::UpdateFocusedApp(&tracker, "");

  ASSERT_EQ(events.size(), 3);
  EXPECT_EQ(events[0], "code");
  EXPECT_EQ(events[1], "firefox");
  EXPECT_EQ(events[2], "");
  EXPECT_EQ(tracker.focused_app(), "");
}

TEST(GnomeAppTrackerTest, ClearingOwnerResetsFocusedApp) {
  std::vector<std::string> events;
  GnomeAppTracker tracker(
      nullptr, [&events](const std::string& app) { events.push_back(app); });

  GnomeAppTrackerTestPeer::UpdateFocusedApp(&tracker, "code");
  GnomeAppTrackerTestPeer::HandleServiceOwnerChanged(&tracker, "");

  EXPECT_EQ(tracker.focused_app(), "");
  EXPECT_FALSE(tracker.is_service_available());
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[1], "");
}

}  // namespace
}  // namespace fcitx
