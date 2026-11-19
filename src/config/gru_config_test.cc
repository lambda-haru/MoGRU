// Copyright 2026, Haruhiro Obora.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Haruhiro Obora nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "config/gru_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "base/file_util.h"
#include "protocol/commands.pb.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

namespace mozc {
namespace config {
namespace {

class GruConfigModeIndicatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GruConfigManager::InvalidateConfigCache();
    GruConfigManager::ClearAllWindowProfileOverrides();
    const char* tmp = std::getenv("TEST_TMPDIR");
    if (tmp != nullptr && tmp[0] != '\0') {
      test_dir_ = tmp;
    } else {
      test_dir_ = testing::GetSourcePath({"config", "test_tmp"});
    }
    std::error_code ec;
    std::filesystem::create_directories(test_dir_, ec);
    config_file_ = FileUtil::JoinPath(test_dir_, "test_gru_config.ini");
    std::filesystem::remove(config_file_, ec);
  }

  void TearDown() override {
    GruConfigManager::ClearAllWindowProfileOverrides();
    std::error_code ec;
    std::filesystem::remove(config_file_, ec);
    GruConfigManager::InvalidateConfigCache();
  }

  std::string test_dir_;
  std::string config_file_;
};

TEST_F(GruConfigModeIndicatorTest, FormatModeIndicatorLabel) {
  EXPECT_EQ(
      GruConfigManager::FormatModeIndicatorLabel("あ", false, "coding", true),
      "あ [coding]");
  EXPECT_EQ(
      GruConfigManager::FormatModeIndicatorLabel("あ", false, "default", true),
      "あ [default]");
  EXPECT_EQ(
      GruConfigManager::FormatModeIndicatorLabel("ア", false, "business", true),
      "ア [business]");
  EXPECT_EQ(
      GruConfigManager::FormatModeIndicatorLabel("A", true, "coding", true),
      "A");
  EXPECT_EQ(
      GruConfigManager::FormatModeIndicatorLabel("あ", false, "coding", false),
      "あ");
  EXPECT_EQ(
      GruConfigManager::FormatModeIndicatorLabel("あ", false, "", true),
      "あ");
}

TEST_F(GruConfigModeIndicatorTest, ExtractAppNameFromExecutablePath) {
  EXPECT_EQ(GruConfigManager::ExtractAppNameFromExecutablePath(
                "C:\\Program Files\\Microsoft VS Code\\Code.exe"),
            "Code");
  EXPECT_EQ(
      GruConfigManager::ExtractAppNameFromExecutablePath("D:\\Apps\\Slack.EXE"),
      "Slack");
  EXPECT_EQ(GruConfigManager::ExtractAppNameFromExecutablePath("/usr/bin/code"),
            "code");
  EXPECT_EQ(GruConfigManager::ExtractAppNameFromExecutablePath("  notepad.exe "),
            "notepad");
  EXPECT_EQ(GruConfigManager::ExtractAppNameFromExecutablePath("   "), "");
}

TEST_F(GruConfigModeIndicatorTest, ResolveAndPersistModeIndicatorFlag) {
  GruPenaltyConfig cfg;
  cfg.current_profile = "business";
  cfg.enable_app_profile_switching = true;
  cfg.app_profile_map = "Code:coding,slack:business";
  cfg.show_profile_in_mode_indicator = false;
  ASSERT_TRUE(GruConfigManager::SaveConfig(cfg, config_file_));

  GruPenaltyConfig loaded = GruConfigManager::LoadConfig(config_file_);
  EXPECT_FALSE(loaded.show_profile_in_mode_indicator);
  EXPECT_EQ(loaded.current_profile, "business");

  const auto info_default =
      GruConfigManager::ResolveModeIndicatorProfileInfo(nullptr, config_file_);
  EXPECT_EQ(info_default.active_profile, "business");
  EXPECT_FALSE(info_default.show_profile_in_mode_indicator);

  commands::Context code_ctx;
  code_ctx.set_application_name("Code");
  const auto info_code = GruConfigManager::ResolveModeIndicatorProfileInfo(
      &code_ctx, config_file_);
  EXPECT_EQ(info_code.active_profile, "coding");
  EXPECT_FALSE(info_code.show_profile_in_mode_indicator);

  // Simulate an external edit within the same second and test force_mtime_check.
  cfg.current_profile = "default";
  cfg.show_profile_in_mode_indicator = true;
  {
    std::ofstream ofs(config_file_, std::ios::trunc);
    ASSERT_TRUE(ofs.is_open());
    ofs << "current_profile = default\n"
        << "enable_app_profile_switching = true\n"
        << "app_profile_map = Code:coding,slack:business\n"
        << "show_profile_in_mode_indicator = true\n";
  }

  const auto refreshed = GruConfigManager::ResolveModeIndicatorProfileInfo(
      nullptr, config_file_, /*force_mtime_check=*/true);
  EXPECT_EQ(refreshed.active_profile, "default");
  EXPECT_TRUE(refreshed.show_profile_in_mode_indicator);

  const auto refreshed_code = GruConfigManager::ResolveModeIndicatorProfileInfo(
      &code_ctx, config_file_);
  EXPECT_EQ(refreshed_code.active_profile, "coding");
  EXPECT_TRUE(refreshed_code.show_profile_in_mode_indicator);
}

TEST(GruConfigAppNameTest, IdentifiesCompositorsAndPortals) {
  EXPECT_TRUE(GruConfigManager::IsCompositorOrProxyAppName("gnome-shell"));
  EXPECT_TRUE(GruConfigManager::IsCompositorOrProxyAppName(" Xwayland "));
  EXPECT_TRUE(GruConfigManager::IsCompositorOrProxyAppName("ibus-portal"));
  EXPECT_TRUE(
      GruConfigManager::IsCompositorOrProxyAppName("xdg-desktop-portal-gnome"));
  EXPECT_FALSE(GruConfigManager::IsCompositorOrProxyAppName("code"));
  EXPECT_FALSE(GruConfigManager::IsCompositorOrProxyAppName(""));
}

TEST_F(GruConfigModeIndicatorTest, WindowProfileMemoryConfigAndPrecedence) {
  GruPenaltyConfig cfg;
  EXPECT_TRUE(cfg.enable_window_profile_memory);
  cfg.current_profile = "default";
  cfg.enable_app_profile_switching = true;
  cfg.enable_window_profile_memory = false;
  cfg.app_profile_map = "code:coding,slack:business";
  ASSERT_TRUE(GruConfigManager::SaveConfig(cfg, config_file_));

  GruPenaltyConfig loaded = GruConfigManager::LoadConfig(config_file_);
  EXPECT_FALSE(loaded.enable_window_profile_memory);

  cfg.enable_window_profile_memory = true;
  ASSERT_TRUE(GruConfigManager::SaveConfig(cfg, config_file_));
  loaded = GruConfigManager::LoadConfig(config_file_);
  EXPECT_TRUE(loaded.enable_window_profile_memory);

  commands::Context win_code_a;
  win_code_a.set_window_id("gnome:101");
  win_code_a.set_application_name("code");

  commands::Context win_code_b;
  win_code_b.set_window_id("gnome:102");
  win_code_b.set_application_name("code");

  commands::Context win_gedit;
  win_gedit.set_window_id("gnome:103");
  win_gedit.set_application_name("gedit");

  // Unoverridden windows follow app_profile_map > current_profile.
  EXPECT_EQ(
      GruConfigManager::ResolveActiveProfileForContext(loaded, &win_code_a),
      "coding");
  EXPECT_EQ(
      GruConfigManager::ResolveActiveProfileForContext(loaded, &win_code_b),
      "coding");
  EXPECT_EQ(
      GruConfigManager::ResolveActiveProfileForContext(loaded, &win_gedit),
      "default");

  // Override only win_code_a -> win_code_b stays "coding".
  GruConfigManager::SetWindowProfileOverride("gnome:101", "creative");
  EXPECT_EQ(
      GruConfigManager::ResolveActiveProfileForContext(loaded, &win_code_a),
      "creative");
  EXPECT_EQ(
      GruConfigManager::ResolveActiveProfileForContext(loaded, &win_code_b),
      "coding");

  // Changing global current_profile updates unoverridden windows, not win_code_a.
  loaded.current_profile = "casual";
  EXPECT_EQ(
      GruConfigManager::ResolveActiveProfileForContext(loaded, &win_code_a),
      "creative");
  EXPECT_EQ(
      GruConfigManager::ResolveActiveProfileForContext(loaded, &win_gedit),
      "casual");

  // Invalid/deleted window override falls back to app_profile_map.
  GruConfigManager::SetWindowProfileOverride("gnome:101", "deleted_profile");
  EXPECT_EQ(
      GruConfigManager::ResolveActiveProfileForContext(loaded, &win_code_a),
      "coding");

  // Feature disabled ignores window override.
  GruConfigManager::SetWindowProfileOverride("gnome:101", "creative");
  loaded.enable_window_profile_memory = false;
  EXPECT_EQ(
      GruConfigManager::ResolveActiveProfileForContext(loaded, &win_code_a),
      "coding");
}

TEST_F(GruConfigModeIndicatorTest, ExecuteInlineCommandCandidatePerWindow) {
  GruPenaltyConfig cfg;
  cfg.current_profile = "default";
  cfg.enable_app_profile_switching = true;
  cfg.enable_window_profile_memory = true;
  cfg.app_profile_map = "code:coding";
  ASSERT_TRUE(GruConfigManager::SaveConfig(cfg, config_file_));

  commands::Context win1;
  win1.set_window_id("gnome:201");
  win1.set_application_name("gedit");

  commands::Context win2;
  win2.set_window_id("gnome:202");
  win2.set_application_name("gedit");

  // Switching profile inside win1 stores override in memory without changing disk.
  GruPenaltyConfig out_cfg;
  const InlineCommandResult switch_res =
      GruConfigManager::ExecuteInlineCommandCandidate(
          "MoGRU: プロファイル切替 [business]", &win1, config_file_, &out_cfg);
  EXPECT_TRUE(switch_res.handled);
  EXPECT_FALSE(switch_res.config_saved);
  EXPECT_EQ(switch_res.active_profile, "business");
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("gnome:201"), "business");
  EXPECT_EQ(GruConfigManager::LoadConfig(config_file_).current_profile, "default");

  EXPECT_EQ(GruConfigManager::ResolveModeIndicatorProfileInfo(
                &win1, config_file_, /*force_mtime_check=*/true)
                .active_profile,
            "business");
  EXPECT_EQ(GruConfigManager::ResolveModeIndicatorProfileInfo(
                &win2, config_file_, /*force_mtime_check=*/true)
                .active_profile,
            "default");

  // Resetting window memory returns win1 to default.
  const InlineCommandResult reset_res =
      GruConfigManager::ExecuteInlineCommandCandidate(
          kWindowProfileResetCommand, &win1, config_file_, &out_cfg);
  EXPECT_TRUE(reset_res.handled);
  EXPECT_FALSE(reset_res.config_saved);
  EXPECT_EQ(reset_res.active_profile, "default");
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("gnome:201"), "");

  // Close notification cleans up window override.
  GruConfigManager::SetWindowProfileOverride("gnome:201", "creative");
  commands::Context close_ctx;
  close_ctx.add_experimental_features("close_window:gnome:201");
  GruConfigManager::ProcessContextNotifications(&close_ctx);
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("gnome:201"), "");

  // App registration updates config on disk and clears active window override.
  commands::Context win_code;
  win_code.set_window_id("gnome:203");
  win_code.set_application_name("code");
  GruConfigManager::SetWindowProfileOverride("gnome:203", "business");
  const InlineCommandResult bind_res =
      GruConfigManager::ExecuteInlineCommandCandidate(
          "MoGRU: 現在のアプリ (code) を [creative] に登録", &win_code,
          config_file_, &out_cfg);
  EXPECT_TRUE(bind_res.handled);
  EXPECT_TRUE(bind_res.config_saved);
  EXPECT_EQ(bind_res.active_profile, "creative");
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("gnome:203"), "");

  // Switch without window_id still writes current_profile to disk.
  const InlineCommandResult global_res =
      GruConfigManager::ExecuteInlineCommandCandidate(
          "MoGRU: プロファイル切替 [casual]", nullptr, config_file_, &out_cfg);
  EXPECT_TRUE(global_res.handled);
  EXPECT_TRUE(global_res.config_saved);
  EXPECT_EQ(global_res.active_profile, "casual");
  EXPECT_EQ(GruConfigManager::LoadConfig(config_file_).current_profile, "casual");
}

TEST_F(GruConfigModeIndicatorTest, WindowProfileOverrideLruEviction) {
  for (int i = 0; i < 1024; ++i) {
    GruConfigManager::SetWindowProfileOverride(
        "w:" + std::to_string(i), "coding");
  }
  // Touch "w:0" so "w:1" becomes least-recently used.
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("w:0"), "coding");

  GruConfigManager::SetWindowProfileOverride("w:1024", "business");
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("w:0"), "coding");
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("w:1"), "");
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("w:1024"), "business");

  GruConfigManager::ClearWindowProfileOverridesByPrefix("w:");
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("w:0"), "");
  EXPECT_EQ(GruConfigManager::GetWindowProfileOverride("w:1024"), "");
}

}  // namespace
}  // namespace config
}  // namespace mozc
