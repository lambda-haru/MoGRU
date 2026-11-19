// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#include "unix/fcitx5/app_name_resolver.h"

#include "testing/gunit.h"

namespace fcitx {
namespace {

TEST(AppNameResolverTest, PrefersIcProgramWhenRealApp) {
  EXPECT_EQ(ResolveApplicationName("firefox", "code"), "firefox");
  EXPECT_EQ(ResolveApplicationName("  code  ", "slack"), "code");
}

TEST(AppNameResolverTest, FallsBackWhenIcProgramIsEmptyOrProxy) {
  EXPECT_EQ(ResolveApplicationName("", "code"), "code");
  EXPECT_EQ(ResolveApplicationName("gnome-shell", "code"), "code");
  EXPECT_EQ(ResolveApplicationName("mutter", "slack"), "slack");
  EXPECT_EQ(ResolveApplicationName("Xwayland", "discord"), "discord");
  EXPECT_EQ(ResolveApplicationName("ibus-portal", "firefox"), "firefox");
  EXPECT_EQ(ResolveApplicationName("xdg-desktop-portal-gnome", "code"), "code");
}

TEST(AppNameResolverTest, RejectsProxyFromGnomeFallback) {
  EXPECT_EQ(ResolveApplicationName("", "gnome-shell"), "");
  EXPECT_EQ(ResolveApplicationName("gnome-shell", "xwayland"), "");
  EXPECT_EQ(ResolveApplicationName("", ""), "");
}

TEST(AppNameResolverTest, IdentifiesActiveMoGRUInputMethod) {
  EXPECT_TRUE(IsMoGRUActiveInputMethod("mogru"));
  EXPECT_TRUE(IsMoGRUActiveInputMethod("mozc"));
  EXPECT_TRUE(IsMoGRUActiveInputMethod("mozc-gru"));
  EXPECT_FALSE(IsMoGRUActiveInputMethod("keyboard-us"));
  EXPECT_FALSE(IsMoGRUActiveInputMethod(""));
}

TEST(AppNameResolverTest, ResolvesWindowIdForGnomeAndFallback) {
  EXPECT_EQ(ResolveWindowId(/*gnome_service_available=*/true, "gnome:42",
                            /*allow_ic_fallback=*/true, "ic:abc"),
            "gnome:42");
  // When GNOME extension reports empty (e.g. Overview), do not fall back to IC.
  EXPECT_EQ(ResolveWindowId(/*gnome_service_available=*/true, "",
                            /*allow_ic_fallback=*/true, "ic:abc"),
            "");
  EXPECT_EQ(ResolveWindowId(/*gnome_service_available=*/false, "",
                            /*allow_ic_fallback=*/true, "ic:abc"),
            "ic:abc");
  EXPECT_EQ(ResolveWindowId(/*gnome_service_available=*/false, "",
                            /*allow_ic_fallback=*/false, "ic:abc"),
            "");

  const unsigned char bytes[4] = {0x0a, 0x1b, 0xff, 0x00};
  EXPECT_EQ(FormatInputContextWindowId(bytes, sizeof(bytes)), "ic:0a1bff00");
  const unsigned char zeros[4] = {0, 0, 0, 0};
  EXPECT_EQ(FormatInputContextWindowId(zeros, sizeof(zeros)), "");
}

}  // namespace
}  // namespace fcitx
