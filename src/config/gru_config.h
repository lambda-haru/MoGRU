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
#include <string>
#include <vector>

namespace mozc {

struct GruPenaltyConfig {
  bool enable_gru_rewriter = true;
  bool enable_live_conversion = true;
  bool show_segment_separator_in_conversion = false;
  bool show_segment_separator_in_live_conversion = false;
  bool enable_live_conversion_prefix_freezing = true;
  bool enable_live_conversion_auto_commit = false;
  int32_t live_conversion_length_threshold = 24;
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
};

}  // namespace config
}  // namespace mozc

#endif  // MOZC_CONFIG_GRU_CONFIG_H_
