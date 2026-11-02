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

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "base/file_util.h"
#include "base/system_util.h"

namespace mozc {
namespace config {
namespace {

bool IsInTestEnvironment() {
  const char* test_tmpdir = std::getenv("TEST_TMPDIR");
  return test_tmpdir != nullptr && test_tmpdir[0] != '\0';
}

std::string ExpandTilde(const std::string& path) {
  if (!path.empty() && path[0] == '~') {
    if (const char* home = std::getenv("HOME")) {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

// Writable user-specific directories for gru_config.ini and grumodel_user.bin.
std::vector<std::string> GetUserProfileCandidateDirs() {
  std::vector<std::string> dirs;
  auto add_unique = [&dirs](const std::string& d) {
    if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end()) {
      dirs.push_back(d);
    }
  };

#ifdef __linux__
  if (const char* xdg_config = std::getenv("XDG_CONFIG_HOME")) {
    if (xdg_config[0] != '\0') {
      add_unique(FileUtil::JoinPath(xdg_config, "mogru"));
    }
  }
  if (const char* home = std::getenv("HOME")) {
    add_unique(FileUtil::JoinPath(home, ".config/mogru"));
    add_unique(FileUtil::JoinPath(home, ".mogru"));
  }
  add_unique(SystemUtil::GetUserProfileDirectory());
#else
  add_unique(SystemUtil::GetUserProfileDirectory());
  if (const char* home = std::getenv("HOME")) {
    add_unique(FileUtil::JoinPath(home, "Library/Application Support/MoGRU"));
    add_unique(FileUtil::JoinPath(home, ".config/mogru"));
    add_unique(FileUtil::JoinPath(home, ".mogru"));
  }
  if (const char* userprofile = std::getenv("USERPROFILE")) {
    add_unique(FileUtil::JoinPath(userprofile, "AppData/LocalLow/MoGRU"));
  }
#endif
  return dirs;
}

// Read-only system/application directories for bundled base models
// (grumodel.bin, chardata.bin, grumodel_new.bin, chardata_new.bin).
std::vector<std::string> GetSystemModelCandidateDirs() {
  std::vector<std::string> dirs;
  auto add_unique = [&dirs](const std::string& d) {
    if (!d.empty() && std::find(dirs.begin(), dirs.end(), d) == dirs.end()) {
      dirs.push_back(d);
    }
  };

#ifdef __linux__
  if (const char* xdg_data = std::getenv("XDG_DATA_HOME")) {
    if (xdg_data[0] != '\0') {
      add_unique(FileUtil::JoinPath(xdg_data, "mogru/models"));
      add_unique(FileUtil::JoinPath(xdg_data, "mogru"));
    }
  }
  if (const char* home = std::getenv("HOME")) {
    add_unique(FileUtil::JoinPath(home, ".local/share/mogru/models"));
    add_unique(FileUtil::JoinPath(home, ".local/share/mogru"));
  }
  add_unique("/usr/local/share/mogru/models");
  add_unique("/usr/share/mogru/models");
#endif
  const std::string server_dir = SystemUtil::GetServerDirectory();
  if (!server_dir.empty()) {
    add_unique(FileUtil::JoinPath(server_dir, "models"));
    add_unique(server_dir);
  }
  return dirs;
}

// Combined search directories for base models and chardata.
// System model directories are searched first so bundled/updated models take
// precedence over stale copies left by older installer scripts, followed by
// user profile directories for backward compatibility.
std::vector<std::string> GetCandidateModelSearchDirs() {
  std::vector<std::string> dirs = GetSystemModelCandidateDirs();
  for (const auto& d : GetUserProfileCandidateDirs()) {
    if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) {
      dirs.push_back(d);
    }
  }
  return dirs;
}

void MigrateLegacyFilesOnce() {
  if (IsInTestEnvironment()) {
    return;
  }
  static std::once_flag once;
  std::call_once(once, []() {
    const auto user_dirs = GetUserProfileCandidateDirs();
    if (user_dirs.empty()) {
      return;
    }
    const std::string primary_user_dir = user_dirs.front();

    std::vector<std::string> legacy_mozc_dirs;
    auto add_legacy = [&legacy_mozc_dirs](const std::string& d) {
      if (!d.empty() &&
          std::find(legacy_mozc_dirs.begin(), legacy_mozc_dirs.end(), d) ==
              legacy_mozc_dirs.end()) {
        legacy_mozc_dirs.push_back(d);
      }
    };
#ifdef __linux__
    if (const char* home = std::getenv("HOME")) {
      add_legacy(FileUtil::JoinPath(home, ".config/mozc"));
      add_legacy(FileUtil::JoinPath(home, ".mozc"));
    }
#else
    if (const char* home = std::getenv("HOME")) {
      add_legacy(FileUtil::JoinPath(home, "Library/Application Support/Mozc"));
      add_legacy(FileUtil::JoinPath(
          home, "Library/Application Support/Google/JapaneseInput"));
      add_legacy(FileUtil::JoinPath(home, ".config/mozc"));
    }
    if (const char* userprofile = std::getenv("USERPROFILE")) {
      add_legacy(FileUtil::JoinPath(userprofile, "AppData/LocalLow/Mozc"));
      add_legacy(FileUtil::JoinPath(userprofile,
                                    "AppData/LocalLow/Google/JapaneseInput"));
    }
#endif

    std::error_code ec;
    for (const auto& legacy_dir : legacy_mozc_dirs) {
      if (!std::filesystem::exists(legacy_dir, ec)) {
        continue;
      }
      const std::string legacy_ini =
          FileUtil::JoinPath(legacy_dir, "gru_config.ini");
      const std::string target_ini =
          FileUtil::JoinPath(primary_user_dir, "gru_config.ini");
      if (std::filesystem::exists(legacy_ini, ec) &&
          !std::filesystem::exists(target_ini, ec)) {
        std::filesystem::create_directories(primary_user_dir, ec);
        std::filesystem::copy_file(
            legacy_ini, target_ini,
            std::filesystem::copy_options::overwrite_existing, ec);
      }

      const std::string legacy_user_model =
          FileUtil::JoinPath(legacy_dir, "grumodel_user.bin");
      const std::string target_user_model =
          FileUtil::JoinPath(primary_user_dir, "grumodel_user.bin");
      if (std::filesystem::exists(legacy_user_model, ec) &&
          !std::filesystem::exists(target_user_model, ec)) {
        std::filesystem::create_directories(primary_user_dir, ec);
        std::filesystem::copy_file(
            legacy_user_model, target_user_model,
            std::filesystem::copy_options::overwrite_existing, ec);
      }

      for (const char* fname :
           {"grumodel.bin", "chardata.bin", "gru_config.ini",
            "gru_config.ini.bak", "grumodel_user.bin"}) {
        std::filesystem::remove(FileUtil::JoinPath(legacy_dir, fname), ec);
      }
    }

    // If system model directory already provides grumodel.bin, clean up
    // duplicate base model copies placed in user profile dirs by older scripts.
    const auto sys_dirs = GetSystemModelCandidateDirs();
    bool has_system_model = false;
    for (const auto& sdir : sys_dirs) {
      if (std::filesystem::exists(FileUtil::JoinPath(sdir, "grumodel.bin"),
                                  ec)) {
        has_system_model = true;
        break;
      }
    }
    if (has_system_model) {
      for (const auto& udir : user_dirs) {
        if (std::find(sys_dirs.begin(), sys_dirs.end(), udir) !=
            sys_dirs.end()) {
          continue;
        }
        for (const char* fname : {"grumodel.bin", "chardata.bin",
                                  "grumodel_new.bin", "chardata_new.bin"}) {
          std::filesystem::remove(FileUtil::JoinPath(udir, fname), ec);
        }
      }
    }
  });
}

}  // namespace

std::string GruConfigManager::GetDefaultDataFilePath(
    const std::string& filename) {
  MigrateLegacyFilesOnce();
  const auto dirs = GetCandidateModelSearchDirs();
  for (const auto& dir : dirs) {
    const std::string p = FileUtil::JoinPath(dir, filename);
    if (std::filesystem::exists(p)) {
      return p;
    }
  }
  const auto sys_dirs = GetSystemModelCandidateDirs();
  if (!sys_dirs.empty()) {
    return FileUtil::JoinPath(sys_dirs.front(), filename);
  }
  if (!dirs.empty()) {
    return FileUtil::JoinPath(dirs.front(), filename);
  }
  return filename;
}

std::string GruConfigManager::ResolveDataFilePath(
    const std::string& user_specified,
    const std::string& default_filename) {
  MigrateLegacyFilesOnce();
  if (!user_specified.empty()) {
    const std::string expanded = ExpandTilde(user_specified);
    if (std::filesystem::exists(expanded)) {
      return expanded;
    }
    if (std::filesystem::path(expanded).is_relative()) {
      for (const auto& dir : GetCandidateModelSearchDirs()) {
        const std::string p = FileUtil::JoinPath(dir, expanded);
        if (std::filesystem::exists(p)) return p;
      }
      for (const auto& dev_prefix :
           {"models", "../models", "gru_scorer", "../gru_scorer"}) {
        const std::string p = FileUtil::JoinPath(dev_prefix, expanded);
        if (std::filesystem::exists(p)) return p;
      }
    }
    if (const char* test_srcdir = std::getenv("TEST_SRCDIR")) {
      std::string p = FileUtil::JoinPath(test_srcdir, user_specified);
      if (std::filesystem::exists(p)) return p;
      p = FileUtil::JoinPath(
          test_srcdir, FileUtil::JoinPath("_main", user_specified));
      if (std::filesystem::exists(p)) return p;
    }
    return "";
  }

  // 1. System model directories and user profile directories
  for (const auto& dir : GetCandidateModelSearchDirs()) {
    const std::string p = FileUtil::JoinPath(dir, default_filename);
    if (std::filesystem::exists(p)) return p;
  }

  // 2. Known local development paths & runfiles
  std::vector<std::string> filenames = {default_filename};
  if (default_filename == "grumodel.bin") {
    filenames.push_back("dummy_model.bin");
  }

  for (const auto& fname : filenames) {
    const std::vector<std::string> dev_paths = {
        FileUtil::JoinPath("models", fname),
        FileUtil::JoinPath("../models", fname),
        FileUtil::JoinPath("gru_scorer", fname),
        FileUtil::JoinPath("../gru_scorer", fname),
        fname,
    };
    for (const auto& p : dev_paths) {
      if (std::filesystem::exists(p)) return p;
    }

    if (const char* test_srcdir = std::getenv("TEST_SRCDIR")) {
      std::string p = FileUtil::JoinPath(
          test_srcdir, FileUtil::JoinPath("_main/gru_scorer", fname));
      if (std::filesystem::exists(p)) return p;
      p = FileUtil::JoinPath(
          test_srcdir, FileUtil::JoinPath("gru_scorer", fname));
      if (std::filesystem::exists(p)) return p;
    }
  }

  return "";
}

std::string GruConfigManager::ResolveConfigFilePath(
    const std::string& user_specified,
    bool allow_in_test) {
  MigrateLegacyFilesOnce();
  if (!user_specified.empty()) {
    const std::string expanded = ExpandTilde(user_specified);
    if (std::filesystem::exists(expanded)) {
      return expanded;
    }
    if (const char* test_srcdir = std::getenv("TEST_SRCDIR")) {
      std::string p = FileUtil::JoinPath(test_srcdir, user_specified);
      if (std::filesystem::exists(p)) return p;
      p = FileUtil::JoinPath(
          test_srcdir, FileUtil::JoinPath("_main", user_specified));
      if (std::filesystem::exists(p)) return p;
    }
    return expanded;
  }

  if (!allow_in_test && IsInTestEnvironment()) {
    return "";
  }

  const auto dirs = GetUserProfileCandidateDirs();
  for (const auto& dir : dirs) {
    const std::string p = FileUtil::JoinPath(dir, "gru_config.ini");
    if (std::filesystem::exists(p)) {
      return p;
    }
  }

  if (!dirs.empty()) {
    return FileUtil::JoinPath(dirs.front(), "gru_config.ini");
  }
  return "";
}

std::string GruConfigManager::ResolveUserModelFilePath(
    const std::string& user_specified) {
  MigrateLegacyFilesOnce();
  if (!user_specified.empty()) {
    return ExpandTilde(user_specified);
  }
  if (const char* test_tmpdir = std::getenv("TEST_TMPDIR")) {
    if (test_tmpdir[0] != '\0') {
      return FileUtil::JoinPath(test_tmpdir, "grumodel_user.bin");
    }
  }
  const auto dirs = GetUserProfileCandidateDirs();
  for (const auto& dir : dirs) {
    const std::string p = FileUtil::JoinPath(dir, "grumodel_user.bin");
    if (std::filesystem::exists(p)) {
      return p;
    }
  }
  if (!dirs.empty()) {
    return FileUtil::JoinPath(dirs.front(), "grumodel_user.bin");
  }
  return "";
}

int64_t GruConfigManager::GetFileMtime(const std::string& path) {
  if (path.empty() || !std::filesystem::exists(path)) {
    return 0;
  }
  std::error_code ec;
  const auto ftime = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return 0;
  }
  return ftime.time_since_epoch().count();
}

std::string GruConfigManager::FormatConfigIni(const GruPenaltyConfig& config) {
  return absl::StrFormat(
      "# ==============================================================================\n"
      "# Mozc GRU 言語モデル リライター 設定ファイル\n"
      "# ==============================================================================\n"
      "# このファイルの設定値を変更・保存すると、Fcitx5 や Mozc の再起動なしで\n"
      "# 次の日本語変換から自動的に反映されます（ホットリロードおよびGUI設定画面と双方向同期）。\n"
      "# ==============================================================================\n"
      "\n"
      "# 【GRU ニューラル言語モデルリライターの有効化】 (初期値: true)\n"
      "# - true: 有効, false: 無効（標準の Mozc と同じ変換挙動）\n"
      "enable_gru_rewriter = %s\n"
      "\n"
      "# 【リアルタイム・ライブ変換の有効化】 (初期値: true)\n"
      "# - 入力と同時にインラインで漢字かな交じり文へ自動変換します。\n"
      "# - true: 有効, false: 無効（スペースキーで変換）\n"
      "enable_live_conversion = %s\n"
      "\n"
      "# 【通常変換時（スペースキー等）の文節区切り「/」表示】 (初期値: false)\n"
      "# - true: 変換中のプリエディットに文節区切り「/」を表示, false: 非表示\n"
      "show_segment_separator_in_conversion = %s\n"
      "\n"
      "# 【ライブ変換時の文節区切り「/」表示】 (初期値: false)\n"
      "# - true: ライブ変換中のプリエディットに文節区切り「/」を表示, false: 非表示\n"
      "show_segment_separator_in_live_conversion = %s\n"
      "\n"
      "# 【ライブ変換時の前文節固定（計算量抑制）の有効化】 (初期値: true)\n"
      "# - 指定文字数を超えた際に前半分程度の文節を固定（変換停止）し、計算量を抑制します。\n"
      "# - true: 有効（固定文節とアクティブ文節の境界に「／」を表示）, false: 無効（常に全文変換）\n"
      "enable_live_conversion_prefix_freezing = %s\n"
      "\n"
      "# 【ライブ変換時の自動確定（段階的確定）の有効化】 (初期値: false)\n"
      "# - 前文節が固定された後、次のライブ変換更新タイミング（次のキー入力）で自動確定します。\n"
      "# - true: 有効, false: 無効（Enterキーを押すまでPreedit内に保持）\n"
      "enable_live_conversion_auto_commit = %s\n"
      "\n"
      "# 【ライブ変換の文節固定・自動確定トリガー文字数閾値】 (初期値: 24)\n"
      "# - 確定平仮名キーの文字数がこの値以上になった際に前文節を固定します。\n"
      "live_conversion_length_threshold = %d\n"
      "\n"
      "# 【ベース言語モデルファイル (grumodel.bin) のパス】 (初期値: 空欄 = 標準パスを自動検出)\n"
      "gru_model_path = %s\n"
      "\n"
      "# 【文字インデックス辞書 (chardata.bin) のパス】 (初期値: 空欄 = 標準パスを自動検出)\n"
      "gru_chardata_path = %s\n"
      "\n"
      "# 【学習済みユーザーモデル (grumodel_user.bin) の保存先パス】 (初期値: 空欄 = 標準パスを自動検出)\n"
      "gru_user_model_path = %s\n"
      "\n"
      "# 【GRU スコア重み係数 (Alpha)】 (初期値: 800.0)\n"
      "# - Viterbi コストと GRU 対数尤度スコアを結合する重み係数です。\n"
      "gru_score_weight = %.1f\n"
      "\n"
      "# 【変換候補の説明欄への GRU スコア表示】 (初期値: true)\n"
      "# - true: 候補リストに GRU スコア (GRU:...) を表示, false: 非表示\n"
      "show_gru_score = %s\n"
      "\n"
      "# 【カタカナのみの候補に対するスコア乗数】 (初期値: 1.3)\n"
      "# - GRU の対数尤度スコア（負値）に乗算されます。\n"
      "# - 1.0 = 補正なし（素の言語モデルスコア）\n"
      "# - 1.0 より大きい値（例: 1.3, 1.5）: コストが増加し、カタカナ候補が抑制されます。\n"
      "#   確信度が高いカタカナ語（例:「ラーメン」）は抑制が小さく、不自然なカタカナ候補は強く抑制されます。\n"
      "# - 1.0 より小さい値（例: 0.8）: コストが減少し、カタカナ候補が優先（ブースト）されます。\n"
      "katakana_multiplier = %.2f\n"
      "\n"
      "# 【平仮名のみの候補に対するスコア乗数】 (初期値: 1.1)\n"
      "# - 漢字変換できる語で、ひらがなのまま確定するのを防ぎたい場合に有効です。\n"
      "# - 1.0 = 補正なし\n"
      "# - 1.0 より大きい値（例: 1.1, 1.3）: 平仮名候補が抑制されます。\n"
      "# - 1文字の助詞（「は」「が」「を」等）は自動的にペナルティ0倍（補正なし）となります。\n"
      "# - 2文字以上の語には、文字数に応じたスケーリングが適用されます。\n"
      "hiragana_multiplier = %.2f\n"
      "\n"
      "# 【平仮名ペナルティの文字数スケーリング指数】 (初期値: 1.0)\n"
      "# - 文字数 L に応じて倍率をスケーリングする指数 p です。\n"
      "# - 式: effective_multiplier = 1.0 + (hiragana_multiplier - 1.0) * (L - 1)^p\n"
      "# - 1.0: 文字数に比例して線形に増加（2文字: 1.10, 3文字: 1.20, 4文字: 1.30, 6文字: 1.50）\n"
      "# - 1.5 や 2.0: 長い平仮名ほど加速度的にペナルティを強化（4文字以上をより確実に漢字化）\n"
      "# - 0.0: 文字数によらず一律 hiragana_multiplier を適用\n"
      "hiragana_length_power = %.2f\n"
      "\n"
      "# 【絵文字候補に対するペナルティ】 (初期値: 15000.0)\n"
      "# - 文脈予測で絵文字が上位に割り込んでくるのを防ぐための固定加算コストです。\n"
      "emoji_penalty = %.1f\n"
      "\n"
      "# 【英単語候補に対するスコア乗数】 (初期値: 2.0)\n"
      "# - 「とうきょう」に「TOKYO」や「Tokyo」が誤って第1候補に昇格するのを防ぎます。\n"
      "# - 負の対数尤度スコアを乗算（2.0 = コスト実質2倍加算）します。\n"
      "alphabet_multiplier = %.2f\n"
      "\n"
      "# 【文節区切り再編成（Resegmentation）の有効化】 (初期値: true)\n"
      "# - GRU 言語モデルの文全体スコアを用いて、より自然な文節区切り位置へ自動変更します。\n"
      "# - true: 有効, false: 無効（従来の Viterbi による固定区切りを使用）\n"
      "enable_resegmentation = %s\n"
      "\n"
      "# 【文節区切りを変更するための最小コスト改善閾値】 (初期値: 600.0)\n"
      "# - 別の文節区切りパターンのスコアが、デフォルトの区切りよりこの値以上改善した場合のみ区切りを変更します。\n"
      "# - 値を小さくする（例: 300）と積極的に区切りを変更し、大きくする（例: 1000）と慎重になります。\n"
      "resegment_threshold = %.1f\n"
      "\n"
      "# 【入力履歴を用いたオンライン継続学習の有効化】 (初期値: true)\n"
      "# - 変換確定時の文字列・文脈を用いて、GRU 言語モデルをリアルタイムに微調整します。\n"
      "enable_online_learning = %s\n"
      "\n"
      "# 【オンライン学習の更新対象モード】 (初期値: output_only)\n"
      "# - output_only: 出力層（w_out / b_out）のみを更新（高速・忘却が起きにくく安定）\n"
      "# - full: Embedding・GRUセル・出力層の全パラメータをBPTTで更新（高い文脈適応力）\n"
      "online_learning_mode = %s\n"
      "\n"
      "# 【オンライン学習の学習率】 (初期値: 0.05)\n"
      "# - output_only モードでは 0.02〜0.10、full モードでは 0.01〜0.05 程度を推奨します。\n"
      "online_learning_rate = %.5f\n"
      "\n"
      "# 【ベースモデルへのL2アンカー正則化係数】 (初期値: 0.001)\n"
      "# - 元の言語モデル（grumodel.bin）から重みが過度に乖離して一般的な日本語を忘却するのを防ぎます。\n"
      "online_learning_weight_decay = %.5f\n"
      "\n"
      "# 【学習済みユーザーモデルの自動保存間隔（確定回数）】 (初期値: 5)\n"
      "# - 指定した確定回数ごと、およびフォーカス移動時（Sync）に grumodel_user.bin へ保存します。\n"
      "online_learning_save_interval = %d\n",
      config.enable_gru_rewriter ? "true" : "false",
      config.enable_live_conversion ? "true" : "false",
      config.show_segment_separator_in_conversion ? "true" : "false",
      config.show_segment_separator_in_live_conversion ? "true" : "false",
      config.enable_live_conversion_prefix_freezing ? "true" : "false",
      config.enable_live_conversion_auto_commit ? "true" : "false",
      config.live_conversion_length_threshold,
      config.gru_model_path,
      config.gru_chardata_path,
      config.gru_user_model_path,
      config.gru_score_weight,
      config.show_gru_score ? "true" : "false",
      config.katakana_multiplier,
      config.hiragana_multiplier,
      config.hiragana_length_power,
      config.emoji_penalty,
      config.alphabet_multiplier,
      config.enable_resegmentation ? "true" : "false",
      config.resegment_threshold,
      config.enable_online_learning ? "true" : "false",
      config.online_learning_mode,
      config.online_learning_rate,
      config.online_learning_weight_decay,
      config.online_learning_save_interval);
}

void GruConfigManager::EnsureConfigFileExists(const std::string& path) {
  if (path.empty()) return;

  if (!std::filesystem::exists(path)) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
      std::filesystem::create_directories(parent, ec);
    }
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;
    ofs << FormatConfigIni(GruPenaltyConfig());
    return;
  }

  std::ifstream ifs(path);
  if (!ifs.is_open()) return;
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  ifs.close();

  const bool has_legacy_keys =
      (content.find("katakana_penalty") != std::string::npos ||
       content.find("hiragana_penalty") != std::string::npos);

  if (has_legacy_keys) {
    std::error_code ec;
    std::filesystem::copy_file(
        path, path + ".bak",
        std::filesystem::copy_options::overwrite_existing, ec);

    const GruPenaltyConfig migrated = ParseConfigFile(path, GruPenaltyConfig());
    std::ofstream ofs(path, std::ios::trunc);
    if (ofs.is_open()) {
      ofs << FormatConfigIni(migrated);
      LOG(INFO) << "GruConfigManager: Upgraded legacy config file to new format: " << path;
    }
    return;
  }

  if (IsInTestEnvironment()) {
    return;
  }

  std::string append_content;
  if (content.find("enable_gru_rewriter") == std::string::npos) {
    append_content +=
        "\n# 【GRU ニューラル言語モデルリライターの有効化】 (初期値: true)\n"
        "enable_gru_rewriter = true\n";
  }
  if (content.find("enable_live_conversion") == std::string::npos) {
    append_content +=
        "\n# 【リアルタイム・ライブ変換の有効化】 (初期値: true)\n"
        "enable_live_conversion = true\n";
  }
  if (content.find("show_segment_separator_in_conversion") == std::string::npos) {
    append_content +=
        "\n# 【通常変換時（スペースキー等）の文節区切り「/」表示】 (初期値: false)\n"
        "show_segment_separator_in_conversion = false\n";
  }
  if (content.find("show_segment_separator_in_live_conversion") == std::string::npos) {
    append_content +=
        "\n# 【ライブ変換時の文節区切り「/」表示】 (初期値: false)\n"
        "show_segment_separator_in_live_conversion = false\n";
  }
  if (content.find("enable_live_conversion_prefix_freezing") == std::string::npos) {
    append_content +=
        "\n# 【ライブ変換時の前文節固定（計算量抑制）の有効化】 (初期値: true)\n"
        "enable_live_conversion_prefix_freezing = true\n";
  }
  if (content.find("enable_live_conversion_auto_commit") == std::string::npos) {
    append_content +=
        "\n# 【ライブ変換時の自動確定（段階的確定）の有効化】 (初期値: false)\n"
        "enable_live_conversion_auto_commit = false\n";
  }
  if (content.find("live_conversion_length_threshold") == std::string::npos) {
    append_content +=
        "\n# 【ライブ変換の文節固定・自動確定トリガー文字数閾値】 (初期値: 24)\n"
        "live_conversion_length_threshold = 24\n";
  }
  if (content.find("gru_model_path") == std::string::npos) {
    append_content +=
        "\n# 【ベース言語モデルファイル (grumodel.bin) のパス】 (空欄 = 標準パス)\n"
        "gru_model_path = \n";
  }
  if (content.find("gru_chardata_path") == std::string::npos) {
    append_content +=
        "\n# 【文字インデックス辞書 (chardata.bin) のパス】 (空欄 = 標準パス)\n"
        "gru_chardata_path = \n";
  }
  if (content.find("gru_user_model_path") == std::string::npos) {
    append_content +=
        "\n# 【学習済みユーザーモデル (grumodel_user.bin) の保存先パス】 (空欄 = 標準パス)\n"
        "gru_user_model_path = \n";
  }
  if (content.find("gru_score_weight") == std::string::npos) {
    append_content +=
        "\n# 【GRU スコア重み係数 (Alpha)】 (初期値: 800.0)\n"
        "gru_score_weight = 800.0\n";
  }
  if (content.find("show_gru_score") == std::string::npos) {
    append_content +=
        "\n# 【変換候補の説明欄への GRU スコア表示】 (初期値: true)\n"
        "show_gru_score = true\n";
  }
  if (content.find("hiragana_length_power") == std::string::npos) {
    append_content +=
        "\n# 【平仮名ペナルティの文字数スケーリング指数】 (初期値: 1.0)\n"
        "hiragana_length_power = 1.0\n";
  }
  if (content.find("alphabet_multiplier") == std::string::npos) {
    append_content +=
        "\n# 【英単語候補に対するスコア乗数】 (初期値: 2.0)\n"
        "alphabet_multiplier = 2.0\n";
  }
  if (content.find("enable_resegmentation") == std::string::npos) {
    append_content +=
        "\n# 【文節区切り再編成（Resegmentation）の有効化】 (初期値: true)\n"
        "enable_resegmentation = true\n";
  }
  if (content.find("resegment_threshold") == std::string::npos) {
    append_content +=
        "\n# 【文節区切りを変更するための最小コスト改善閾値】 (初期値: 600.0)\n"
        "resegment_threshold = 600.0\n";
  }
  if (content.find("enable_online_learning") == std::string::npos) {
    append_content +=
        "\n# 【入力履歴を用いたオンライン継続学習の有効化】 (初期値: true)\n"
        "enable_online_learning = true\n";
  }
  if (content.find("online_learning_mode") == std::string::npos) {
    append_content +=
        "\n# 【オンライン学習の更新対象モード】 (初期値: output_only)\n"
        "online_learning_mode = output_only\n";
  }
  if (content.find("online_learning_rate") == std::string::npos) {
    append_content +=
        "\n# 【オンライン学習の学習率】 (初期値: 0.05)\n"
        "online_learning_rate = 0.05\n";
  }
  if (content.find("online_learning_weight_decay") == std::string::npos) {
    append_content +=
        "\n# 【ベースモデルへのL2アンカー正則化係数】 (初期値: 0.001)\n"
        "online_learning_weight_decay = 0.001\n";
  }
  if (content.find("online_learning_save_interval") == std::string::npos) {
    append_content +=
        "\n# 【学習済みユーザーモデルの自動保存間隔（確定回数）】 (初期値: 5)\n"
        "online_learning_save_interval = 5\n";
  }

  if (!append_content.empty()) {
    std::ofstream ofs(path, std::ios::app);
    if (ofs.is_open()) {
      ofs << append_content;
      LOG(INFO) << "GruConfigManager: Appended missing config options to: " << path;
    }
  }
}

GruPenaltyConfig GruConfigManager::ParseConfigFile(
    const std::string& path,
    const GruPenaltyConfig& base_config) {
  GruPenaltyConfig config = base_config;
  std::ifstream ifs(path);
  if (!ifs.is_open()) return config;

  bool has_katakana_mult = false;
  bool has_hiragana_mult = false;
  double legacy_katakana_penalty = 0.0;
  bool has_legacy_katakana_penalty = false;
  double legacy_hiragana_penalty = 0.0;
  bool has_legacy_hiragana_penalty = false;
  const bool in_test = IsInTestEnvironment();

  std::string line;
  while (std::getline(ifs, line)) {
    absl::string_view sv = line;
    const size_t comment_pos = sv.find_first_of("#;");
    if (comment_pos != absl::string_view::npos) {
      sv = sv.substr(0, comment_pos);
    }
    sv = absl::StripAsciiWhitespace(sv);
    if (sv.empty()) continue;

    const size_t eq_pos = sv.find('=');
    if (eq_pos == absl::string_view::npos) continue;

    absl::string_view key = absl::StripAsciiWhitespace(sv.substr(0, eq_pos));
    absl::string_view val = absl::StripAsciiWhitespace(sv.substr(eq_pos + 1));

    if (key == "gru_model_path") {
      if (!val.empty()) {
        config.gru_model_path = std::string(val);
      } else if (!in_test) {
        config.gru_model_path.clear();
      }
      continue;
    }
    if (key == "gru_chardata_path") {
      if (!val.empty()) {
        config.gru_chardata_path = std::string(val);
      } else if (!in_test) {
        config.gru_chardata_path.clear();
      }
      continue;
    }
    if (key == "gru_user_model_path") {
      if (!val.empty()) {
        config.gru_user_model_path = std::string(val);
      } else if (!in_test) {
        config.gru_user_model_path.clear();
      }
      continue;
    }
    if (key == "online_learning_mode") {
      if (val == "full" || val == "full_bptt" || val == "bptt" ||
          val == "output_only") {
        config.online_learning_mode =
            (val == "full_bptt" || val == "bptt") ? "full" : std::string(val);
      }
      continue;
    }
    if (key == "enable_gru_rewriter") {
      bool b = true;
      if (absl::SimpleAtob(val, &b)) {
        config.enable_gru_rewriter = b;
      }
      continue;
    }
    if (key == "enable_live_conversion") {
      bool b = true;
      if (absl::SimpleAtob(val, &b)) {
        config.enable_live_conversion = b;
      }
      continue;
    }
    if (key == "show_segment_separator_in_conversion" ||
        key == "show_segment_separator") {
      bool b = false;
      if (absl::SimpleAtob(val, &b)) {
        config.show_segment_separator_in_conversion = b;
      }
      continue;
    }
    if (key == "show_segment_separator_in_live_conversion") {
      bool b = false;
      if (absl::SimpleAtob(val, &b)) {
        config.show_segment_separator_in_live_conversion = b;
      }
      continue;
    }
    if (key == "enable_live_conversion_prefix_freezing") {
      bool b = true;
      if (absl::SimpleAtob(val, &b)) {
        config.enable_live_conversion_prefix_freezing = b;
      }
      continue;
    }
    if (key == "enable_live_conversion_auto_commit") {
      bool b = false;
      if (absl::SimpleAtob(val, &b)) {
        config.enable_live_conversion_auto_commit = b;
      }
      continue;
    }
    if (key == "live_conversion_length_threshold") {
      int32_t val_int = 24;
      if (absl::SimpleAtoi(val, &val_int)) {
        config.live_conversion_length_threshold = std::max<int32_t>(1, val_int);
      }
      continue;
    }
    if (key == "show_gru_score") {
      bool b = true;
      if (absl::SimpleAtob(val, &b)) {
        config.show_gru_score = b;
      }
      continue;
    }
    if (key == "enable_resegmentation") {
      bool b = true;
      if (absl::SimpleAtob(val, &b)) {
        config.enable_resegmentation = b;
      }
      continue;
    }
    if (key == "enable_online_learning") {
      bool b = true;
      if (absl::SimpleAtob(val, &b)) {
        config.enable_online_learning = b;
      }
      continue;
    }
    if (key == "online_learning_save_interval") {
      int32_t interval = 5;
      if (absl::SimpleAtoi(val, &interval) && interval >= 1) {
        config.online_learning_save_interval = interval;
      }
      continue;
    }

    double d = 0.0;
    if (absl::SimpleAtod(val, &d)) {
      if (key == "gru_score_weight" || key == "score_weight") {
        config.gru_score_weight = d;
      } else if (key == "katakana_multiplier") {
        config.katakana_multiplier = d;
        has_katakana_mult = true;
      } else if (key == "hiragana_multiplier") {
        config.hiragana_multiplier = d;
        has_hiragana_mult = true;
      } else if (key == "hiragana_length_power") {
        config.hiragana_length_power = d;
      } else if (key == "katakana_penalty") {
        legacy_katakana_penalty = d;
        has_legacy_katakana_penalty = true;
      } else if (key == "hiragana_penalty") {
        legacy_hiragana_penalty = d;
        has_legacy_hiragana_penalty = true;
      } else if (key == "emoji_penalty") {
        config.emoji_penalty = d;
      } else if (key == "alphabet_multiplier") {
        config.alphabet_multiplier = d;
      } else if (key == "resegment_threshold") {
        config.resegment_threshold = d;
      } else if (key == "online_learning_rate") {
        config.online_learning_rate = d;
      } else if (key == "online_learning_weight_decay") {
        config.online_learning_weight_decay = d;
      }
    }
  }

  if (!has_katakana_mult && has_legacy_katakana_penalty) {
    config.katakana_multiplier = 1.0 + (legacy_katakana_penalty / 1600.0);
  }
  if (!has_hiragana_mult && has_legacy_hiragana_penalty) {
    config.hiragana_multiplier = 1.0 + (legacy_hiragana_penalty / 3000.0);
  }

  return config;
}

void GruConfigManager::ApplyEnvOverrides(GruPenaltyConfig* config) {
  if (config == nullptr) return;
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_REWRITER")) {
    bool b = true;
    if (absl::SimpleAtob(e, &b)) config->enable_gru_rewriter = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_LIVE_CONVERSION")) {
    bool b = true;
    if (absl::SimpleAtob(e, &b)) config->enable_live_conversion = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_SHOW_SEGMENT_SEPARATOR_IN_CONVERSION")) {
    bool b = false;
    if (absl::SimpleAtob(e, &b)) config->show_segment_separator_in_conversion = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_SHOW_SEGMENT_SEPARATOR_IN_LIVE_CONVERSION")) {
    bool b = false;
    if (absl::SimpleAtob(e, &b)) config->show_segment_separator_in_live_conversion = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_LIVE_CONVERSION_PREFIX_FREEZING")) {
    bool b = true;
    if (absl::SimpleAtob(e, &b)) config->enable_live_conversion_prefix_freezing = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_LIVE_CONVERSION_AUTO_COMMIT")) {
    bool b = false;
    if (absl::SimpleAtob(e, &b)) config->enable_live_conversion_auto_commit = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_LIVE_CONVERSION_LENGTH_THRESHOLD")) {
    int32_t val_int = 24;
    if (absl::SimpleAtoi(e, &val_int)) config->live_conversion_length_threshold = std::max<int32_t>(1, val_int);
  }
  if (const char* e = std::getenv("MOZC_GRU_MODEL_PATH")) {
    if (e[0] != '\0') config->gru_model_path = e;
  }
  if (const char* e = std::getenv("MOZC_GRU_CHARDATA_PATH")) {
    if (e[0] != '\0') config->gru_chardata_path = e;
  }
  if (const char* e = std::getenv("MOZC_GRU_USER_MODEL_PATH")) {
    if (e[0] != '\0') config->gru_user_model_path = e;
  }
  if (const char* e = std::getenv("MOZC_GRU_SCORE_WEIGHT")) {
    double d = 0.0;
    if (absl::SimpleAtod(e, &d)) config->gru_score_weight = d;
  }
  if (const char* e = std::getenv("MOZC_GRU_SHOW_SCORE")) {
    bool b = true;
    if (absl::SimpleAtob(e, &b)) config->show_gru_score = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_KATAKANA_MULTIPLIER")) {
    double d = 0.0;
    if (absl::SimpleAtod(e, &d)) config->katakana_multiplier = d;
  }
  if (const char* e = std::getenv("MOZC_GRU_HIRAGANA_MULTIPLIER")) {
    double d = 0.0;
    if (absl::SimpleAtod(e, &d)) config->hiragana_multiplier = d;
  }
  if (const char* e = std::getenv("MOZC_GRU_HIRAGANA_LENGTH_POWER")) {
    double d = 0.0;
    if (absl::SimpleAtod(e, &d)) config->hiragana_length_power = d;
  }
  if (const char* e = std::getenv("MOZC_GRU_ALPHABET_MULTIPLIER")) {
    double d = 0.0;
    if (absl::SimpleAtod(e, &d)) config->alphabet_multiplier = d;
  }
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_RESEGMENTATION")) {
    bool b = true;
    if (absl::SimpleAtob(e, &b)) config->enable_resegmentation = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_RESEGMENT_THRESHOLD")) {
    double d = 0.0;
    if (absl::SimpleAtod(e, &d)) config->resegment_threshold = d;
  }
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_ONLINE_LEARNING")) {
    bool b = true;
    if (absl::SimpleAtob(e, &b)) config->enable_online_learning = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_ONLINE_LEARNING_MODE")) {
    absl::string_view sv = absl::StripAsciiWhitespace(e);
    if (sv == "full" || sv == "full_bptt" || sv == "bptt" ||
        sv == "output_only") {
      config->online_learning_mode =
          (sv == "full_bptt" || sv == "bptt") ? "full" : std::string(sv);
    }
  }
  if (const char* e = std::getenv("MOZC_GRU_ONLINE_LEARNING_RATE")) {
    double d = 0.0;
    if (absl::SimpleAtod(e, &d)) config->online_learning_rate = d;
  }
}

GruPenaltyConfig GruConfigManager::LoadConfig(
    const std::string& explicit_path,
    const GruPenaltyConfig& base_config) {
  const std::string path =
      ResolveConfigFilePath(explicit_path, /*allow_in_test=*/true);
  if (path.empty()) {
    GruPenaltyConfig cfg = base_config;
    ApplyEnvOverrides(&cfg);
    return cfg;
  }
  EnsureConfigFileExists(path);
  GruPenaltyConfig cfg = ParseConfigFile(path, base_config);
  ApplyEnvOverrides(&cfg);
  return cfg;
}

bool GruConfigManager::SaveConfig(const GruPenaltyConfig& config,
                                  const std::string& explicit_path) {
  const std::string formatted = FormatConfigIni(config);
  std::vector<std::string> targets;

  if (!explicit_path.empty()) {
    targets.push_back(ExpandTilde(explicit_path));
  } else {
    const std::string primary =
        ResolveConfigFilePath("", /*allow_in_test=*/true);
    if (!primary.empty()) {
      targets.push_back(primary);
    }
    if (!IsInTestEnvironment()) {
      for (const auto& dir : GetUserProfileCandidateDirs()) {
        const std::string candidate = FileUtil::JoinPath(dir, "gru_config.ini");
        if (std::filesystem::exists(candidate) &&
            std::find(targets.begin(), targets.end(), candidate) ==
                targets.end()) {
          targets.push_back(candidate);
        }
      }
    }
  }

  if (targets.empty()) {
    return false;
  }

  bool any_saved = false;
  for (const auto& path : targets) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
      std::filesystem::create_directories(parent, ec);
    }
    const std::string tmp_path = path + ".tmp";
    {
      std::ofstream ofs(tmp_path, std::ios::trunc);
      if (!ofs.is_open()) continue;
      ofs << formatted;
      ofs.flush();
      if (!ofs.good()) continue;
    }
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
      std::ofstream ofs(path, std::ios::trunc);
      if (ofs.is_open()) {
        ofs << formatted;
        any_saved = true;
      }
      std::filesystem::remove(tmp_path, ec);
    } else {
      any_saved = true;
    }
  }

  return any_saved;
}

bool GruConfigManager::IsLiveConversionEnabled(
    bool flag_default,
    const std::string& explicit_config_path) {
  static absl::NoDestructor<absl::Mutex> mutex;
  static absl::Time last_check_time = absl::InfinitePast();
  static int64_t last_mtime = -1;
  static std::string last_path;
  static bool cached_value = true;
  static bool has_cached = false;

  absl::MutexLock lock(*mutex);
  const std::string path =
      ResolveConfigFilePath(explicit_config_path, /*allow_in_test=*/false);
  if (path.empty()) {
    GruPenaltyConfig cfg;
    cfg.enable_live_conversion = flag_default;
    ApplyEnvOverrides(&cfg);
    return cfg.enable_live_conversion;
  }

  const absl::Time now = absl::Now();
  if (has_cached && path == last_path &&
      (now - last_check_time) < absl::Seconds(1)) {
    return cached_value;
  }
  last_check_time = now;

  EnsureConfigFileExists(path);
  const int64_t mtime = GetFileMtime(path);
  if (!has_cached || path != last_path || mtime != last_mtime) {
    last_path = path;
    last_mtime = mtime;
    GruPenaltyConfig base;
    base.enable_live_conversion = flag_default;
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.enable_live_conversion;
    has_cached = true;
  }
  return cached_value;
}

bool GruConfigManager::IsShowSegmentSeparatorInConversionEnabled(
    bool flag_default,
    const std::string& explicit_config_path) {
  static absl::NoDestructor<absl::Mutex> mutex;
  static absl::Time last_check_time = absl::InfinitePast();
  static int64_t last_mtime = -1;
  static std::string last_path;
  static bool cached_value = false;
  static bool has_cached = false;

  absl::MutexLock lock(*mutex);
  const std::string path =
      ResolveConfigFilePath(explicit_config_path, /*allow_in_test=*/false);
  if (path.empty()) {
    GruPenaltyConfig cfg;
    cfg.show_segment_separator_in_conversion = flag_default;
    ApplyEnvOverrides(&cfg);
    return cfg.show_segment_separator_in_conversion;
  }

  const absl::Time now = absl::Now();
  if (has_cached && path == last_path &&
      (now - last_check_time) < absl::Seconds(1)) {
    return cached_value;
  }
  last_check_time = now;

  EnsureConfigFileExists(path);
  const int64_t mtime = GetFileMtime(path);
  if (!has_cached || path != last_path || mtime != last_mtime) {
    last_path = path;
    last_mtime = mtime;
    GruPenaltyConfig base;
    base.show_segment_separator_in_conversion = flag_default;
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.show_segment_separator_in_conversion;
    has_cached = true;
  }
  return cached_value;
}

bool GruConfigManager::IsShowSegmentSeparatorInLiveConversionEnabled(
    bool flag_default,
    const std::string& explicit_config_path) {
  static absl::NoDestructor<absl::Mutex> mutex;
  static absl::Time last_check_time = absl::InfinitePast();
  static int64_t last_mtime = -1;
  static std::string last_path;
  static bool cached_value = false;
  static bool has_cached = false;

  absl::MutexLock lock(*mutex);
  const std::string path =
      ResolveConfigFilePath(explicit_config_path, /*allow_in_test=*/false);
  if (path.empty()) {
    GruPenaltyConfig cfg;
    cfg.show_segment_separator_in_live_conversion = flag_default;
    ApplyEnvOverrides(&cfg);
    return cfg.show_segment_separator_in_live_conversion;
  }

  const absl::Time now = absl::Now();
  if (has_cached && path == last_path &&
      (now - last_check_time) < absl::Seconds(1)) {
    return cached_value;
  }
  last_check_time = now;

  EnsureConfigFileExists(path);
  const int64_t mtime = GetFileMtime(path);
  if (!has_cached || path != last_path || mtime != last_mtime) {
    last_path = path;
    last_mtime = mtime;
    GruPenaltyConfig base;
    base.show_segment_separator_in_live_conversion = flag_default;
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.show_segment_separator_in_live_conversion;
    has_cached = true;
  }
  return cached_value;
}

bool GruConfigManager::IsLiveConversionPrefixFreezingEnabled(
    bool flag_default,
    const std::string& explicit_config_path) {
  static absl::NoDestructor<absl::Mutex> mutex;
  static absl::Time last_check_time = absl::InfinitePast();
  static int64_t last_mtime = -1;
  static std::string last_path;
  static bool cached_value = true;
  static bool has_cached = false;

  absl::MutexLock lock(*mutex);
  const std::string path =
      ResolveConfigFilePath(explicit_config_path, /*allow_in_test=*/false);
  if (path.empty()) {
    GruPenaltyConfig cfg;
    cfg.enable_live_conversion_prefix_freezing = flag_default;
    ApplyEnvOverrides(&cfg);
    return cfg.enable_live_conversion_prefix_freezing;
  }

  const absl::Time now = absl::Now();
  if (has_cached && path == last_path &&
      (now - last_check_time) < absl::Seconds(1)) {
    return cached_value;
  }
  last_check_time = now;

  EnsureConfigFileExists(path);
  const int64_t mtime = GetFileMtime(path);
  if (!has_cached || path != last_path || mtime != last_mtime) {
    last_path = path;
    last_mtime = mtime;
    GruPenaltyConfig base;
    base.enable_live_conversion_prefix_freezing = flag_default;
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.enable_live_conversion_prefix_freezing;
    has_cached = true;
  }
  return cached_value;
}

bool GruConfigManager::IsLiveConversionAutoCommitEnabled(
    bool flag_default,
    const std::string& explicit_config_path) {
  static absl::NoDestructor<absl::Mutex> mutex;
  static absl::Time last_check_time = absl::InfinitePast();
  static int64_t last_mtime = -1;
  static std::string last_path;
  static bool cached_value = false;
  static bool has_cached = false;

  absl::MutexLock lock(*mutex);
  const std::string path =
      ResolveConfigFilePath(explicit_config_path, /*allow_in_test=*/false);
  if (path.empty()) {
    GruPenaltyConfig cfg;
    cfg.enable_live_conversion_auto_commit = flag_default;
    ApplyEnvOverrides(&cfg);
    return cfg.enable_live_conversion_auto_commit;
  }

  const absl::Time now = absl::Now();
  if (has_cached && path == last_path &&
      (now - last_check_time) < absl::Seconds(1)) {
    return cached_value;
  }
  last_check_time = now;

  EnsureConfigFileExists(path);
  const int64_t mtime = GetFileMtime(path);
  if (!has_cached || path != last_path || mtime != last_mtime) {
    last_path = path;
    last_mtime = mtime;
    GruPenaltyConfig base;
    base.enable_live_conversion_auto_commit = flag_default;
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.enable_live_conversion_auto_commit;
    has_cached = true;
  }
  return cached_value;
}

int32_t GruConfigManager::GetLiveConversionLengthThreshold(
    int32_t flag_default,
    const std::string& explicit_config_path) {
  static absl::NoDestructor<absl::Mutex> mutex;
  static absl::Time last_check_time = absl::InfinitePast();
  static int64_t last_mtime = -1;
  static std::string last_path;
  static int32_t cached_value = 24;
  static bool has_cached = false;

  absl::MutexLock lock(*mutex);
  const std::string path =
      ResolveConfigFilePath(explicit_config_path, /*allow_in_test=*/false);
  if (path.empty()) {
    GruPenaltyConfig cfg;
    cfg.live_conversion_length_threshold = flag_default;
    ApplyEnvOverrides(&cfg);
    return cfg.live_conversion_length_threshold;
  }

  const absl::Time now = absl::Now();
  if (has_cached && path == last_path &&
      (now - last_check_time) < absl::Seconds(1)) {
    return cached_value;
  }
  last_check_time = now;

  EnsureConfigFileExists(path);
  const int64_t mtime = GetFileMtime(path);
  if (!has_cached || path != last_path || mtime != last_mtime) {
    last_path = path;
    last_mtime = mtime;
    GruPenaltyConfig base;
    base.live_conversion_length_threshold = flag_default;
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.live_conversion_length_threshold;
    has_cached = true;
  }
  return cached_value;
}

}  // namespace config
}  // namespace mozc
