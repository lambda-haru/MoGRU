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

#ifndef MOZC_CONFIG_GRU_CONFIG_H_
#define MOZC_CONFIG_GRU_CONFIG_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"

namespace mozc {

namespace commands {
class Context;
}

struct GruPenaltyConfig {
  bool enable_gru_rewriter = true;
  bool enable_live_conversion = true;
  bool show_segment_separator_in_conversion = false;
  bool show_segment_separator_in_live_conversion = false;
  bool enable_live_conversion_prefix_freezing = true;
  bool enable_live_conversion_auto_commit = false;
  int32_t live_conversion_length_threshold = 24;
  int32_t live_conversion_excluded_hiragana_characters = 0;
  std::string gru_model_path = "";
  std::string gru_chardata_path = "";
  std::string gru_user_model_path = "";
  double gru_score_weight = 800.0;
  bool show_gru_score = true;
  double katakana_multiplier = 1.3;
  double hiragana_multiplier = 1.1;
  double hiragana_length_power = 1.0;
  double emoji_penalty = 15000.0;
  double alphabet_multiplier = 2.0;
  bool enable_resegmentation = true;
  double resegment_threshold = 600.0;
  bool enable_online_learning = true;
  std::string online_learning_mode = "output_only";
  double online_learning_rate = 0.05;
  double online_learning_weight_decay = 0.001;
  int32_t online_learning_save_interval = 5;
  bool enable_english_fallback = true;
  double english_fallback_cost_threshold = 15000.0;
  int32_t english_fallback_min_word_length = 3;
  bool enable_live_conversion_english_fallback = true;
  std::vector<std::string> user_english_words;
  bool enable_conversion_stats = true;
  std::string conversion_stats_path = "";
  bool enable_conversion_history_log = false;

  // Profile configuration
  std::string current_profile = "default";
  bool enable_app_profile_switching = true;
  std::string app_profile_map = "code:coding,cursor:coding,slack:business,discord:casual";

  // Instant adaptation mode configuration
  bool enable_instant_mode = false;
  double instant_learning_rate = 0.80;
  double instant_max_weight_delta = 2.0;

  // Profile overrides: profile_name -> profile_specific_config
  std::map<std::string, GruPenaltyConfig> profiles;

  // Profiles explicitly deleted by the user (prevents built-in presets from re-appearing)
  std::set<std::string> deleted_profiles;
};

using GruConfig = GruPenaltyConfig;

namespace config {

class GruConfigManager {
 public:
  // Resolves a data file path (e.g. "grumodel.bin", "chardata.bin").
  static std::string ResolveDataFilePath(const std::string& user_specified,
                                         const std::string& default_filename);

  // Returns the preferred default path for a data file in the user profile.
  static std::string GetDefaultDataFilePath(const std::string& filename);

  // Resolves the path to gru_config.ini.
  // When `allow_in_test` is false (default) and `user_specified` is empty,
  // returns empty string if running inside a Bazel/gtest environment (TEST_TMPDIR).
  static std::string ResolveConfigFilePath(
      const std::string& user_specified = "",
      bool allow_in_test = false);

  // Resolves the path to grumodel_user.bin.
  static std::string ResolveUserModelFilePath(
      const std::string& user_specified = "");

  // Resolves the path to grumodel_user_<profile>.bin for a specific profile.
  // If profile_name is empty or "default", returns ResolveUserModelFilePath(user_specified).
  static std::string ResolveProfileUserModelFilePath(
      const std::string& profile_name,
      const std::string& user_specified = "");

  // Sanitizes a profile name into a safe filename token (alphanumeric, '_', '-').
  // Returns "default" if empty or invalid.
  static std::string SanitizeProfileName(absl::string_view profile_name);

  // Resolves a ConfigFileStream URI (e.g. "user://segment.db", "user://boundary.db",
  // "user://.history.db") to a profile-specific file path.
  // For "default" profile, returns ConfigFileStream::GetFileName(base_uri).
  // For other profiles, appends "_<profile>" before ".db".
  static std::string ResolveProfileHistoryFileName(
      absl::string_view base_uri,
      absl::string_view profile_name);

  // Resolves the currently active profile name by checking gru_config.ini
  // (with 1-second throttled mtime hot-reload) and applying app-based automatic
  // profile switching when `context` provides an application name.
  static std::string ResolveEffectiveActiveProfile(
      const commands::Context* context = nullptr,
      const std::string& explicit_config_path = "");

  // Forces the next call to ResolveEffectiveActiveProfile to re-read gru_config.ini.
  static void InvalidateConfigCache();

  // Returns built-in default profile configs for presets (business, coding, casual, creative, instant).
  static std::map<std::string, GruPenaltyConfig> GetBuiltinDefaultProfiles(
      const GruPenaltyConfig& base_config = GruPenaltyConfig());

  // Returns an ordered list of available profile names ("default" first, followed by
  // active built-in presets and user-defined profiles, excluding deleted_profiles).
  static std::vector<std::string> GetAvailableProfiles(
      const GruPenaltyConfig& config);

  // Adds a profile (or restores a deleted preset) to `config->profiles`.
  // Returns false if `profile_name` is empty or resolves to "default".
  static bool AddProfile(
      GruPenaltyConfig* config,
      absl::string_view profile_name,
      const GruPenaltyConfig* base_profile = nullptr);

  // Deletes a profile (anything other than "default") from `config`, recording it
  // in `config->deleted_profiles`, resetting `current_profile` to "default" if needed,
  // cleaning up `app_profile_map` entries pointing to it, and optionally removing
  // its dedicated user model / history DB files.
  // Returns false if `profile_name` is empty or resolves to "default".
  static bool DeleteProfile(
      GruPenaltyConfig* config,
      absl::string_view profile_name,
      bool remove_data_files = false);

  // Resolves effective configuration for a given profile by applying profile overrides on top of base_config.
  static GruPenaltyConfig GetEffectiveProfileConfig(
      const GruPenaltyConfig& root_config,
      const std::string& profile_name);

  // Matches active application name against app_profile_map and returns target profile name (or empty if no match).
  static std::string DetectProfileFromApplication(
      const std::string& app_name,
      const GruPenaltyConfig& config);

  // Resolves the effective active profile name given |root_config| and |context|
  // (accounting for app-based auto-switching when enabled and deleted profiles).
  static std::string ResolveActiveProfileForContext(
      const GruPenaltyConfig& root_config,
      const commands::Context* context = nullptr);

  // Sets |enable_instant_mode| on |active_profile| (or root config when empty
  // or "default") so saving and reloading preserves the setting for that profile.
  static void SetInstantModeEnabled(GruPenaltyConfig* config,
                                    absl::string_view active_profile,
                                    bool enabled);

  // Resolves the current application name from Context, env vars, or active window.
  static std::string GetCurrentApplicationName(
      const commands::Context* context = nullptr);

  // Adds or updates an app-to-profile mapping in config->app_profile_map,
  // enabling app profile switching.
  static void AddOrUpdateAppProfileMapping(
      GruPenaltyConfig* config,
      absl::string_view app_name,
      absl::string_view profile_name);

  // Returns the modification timestamp of `path` (or 0 if missing).
  static int64_t GetFileMtime(const std::string& path);

  // Ensures gru_config.ini exists at `path` and upgrades/appends missing keys.
  static void EnsureConfigFileExists(const std::string& path);

  // Parses gru_config.ini at `path`, using `base_config` for unspecified keys.
  static GruPenaltyConfig ParseConfigFile(
      const std::string& path,
      const GruPenaltyConfig& base_config = GruPenaltyConfig());

  // Applies MOZC_GRU_* environment variable overrides.
  static void ApplyEnvOverrides(GruPenaltyConfig* config);

  // Formats `config` into a complete gru_config.ini string with comments.
  static std::string FormatConfigIni(const GruPenaltyConfig& config);

  // Loads GruPenaltyConfig from disk (creating gru_config.ini if needed).
  static GruPenaltyConfig LoadConfig(
      const std::string& explicit_path = "",
      const GruPenaltyConfig& base_config = GruPenaltyConfig());

  // Saves `config` to gru_config.ini and synchronizes any secondary profile
  // copies as well as ~/.config/fcitx5/conf/mogru.conf on Linux if present.
  static bool SaveConfig(const GruPenaltyConfig& config,
                         const std::string& explicit_path = "");

  // Fast (1-second throttled mtime check) query for whether live conversion is
  // enabled in gru_config.ini.
  static bool IsLiveConversionEnabled(
      bool flag_default,
      const std::string& explicit_config_path = "");

  // Fast (1-second throttled mtime check) query for whether segment separator
  // in conversion is enabled in gru_config.ini.
  static bool IsShowSegmentSeparatorInConversionEnabled(
      bool flag_default = false,
      const std::string& explicit_config_path = "");

  // Fast (1-second throttled mtime check) query for whether segment separator
  // in live conversion is enabled in gru_config.ini.
  static bool IsShowSegmentSeparatorInLiveConversionEnabled(
      bool flag_default = false,
      const std::string& explicit_config_path = "");

  // Fast (1-second throttled mtime check) query for whether live conversion prefix
  // freezing is enabled in gru_config.ini.
  static bool IsLiveConversionPrefixFreezingEnabled(
      bool flag_default = true,
      const std::string& explicit_config_path = "");

  // Fast (1-second throttled mtime check) query for whether live conversion auto
  // commit is enabled in gru_config.ini.
  static bool IsLiveConversionAutoCommitEnabled(
      bool flag_default = false,
      const std::string& explicit_config_path = "");

  // Fast (1-second throttled mtime check) query for live conversion length threshold
  // in gru_config.ini.
  static int32_t GetLiveConversionLengthThreshold(
      int32_t flag_default = 24,
      const std::string& explicit_config_path = "");

  // Fast (1-second throttled mtime check) query for excluded trailing hiragana
  // character count in live conversion in gru_config.ini.
  static int32_t GetLiveConversionExcludedHiraganaCharacters(
      int32_t flag_default = 0,
      const std::string& explicit_config_path = "");

  // Fast (1-second throttled mtime check) query for whether conversion stats
  // recording is enabled in gru_config.ini.
  static bool IsConversionStatsEnabled(
      bool flag_default = true,
      const std::string& explicit_config_path = "");

  // Fast (1-second throttled mtime check) query for custom conversion stats path
  // in gru_config.ini.
  static std::string GetConversionStatsPath(
      const std::string& explicit_config_path = "");

  // Fast (1-second throttled mtime check) query for whether conversion history
  // TSV logging is enabled in gru_config.ini.
  static bool IsConversionHistoryLogEnabled(
      bool flag_default = false,
      const std::string& explicit_config_path = "");
};

}  // namespace config
}  // namespace mozc

#endif  // MOZC_CONFIG_GRU_CONFIG_H_
