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

}  // namespace
}  // namespace config
}  // namespace mozc
