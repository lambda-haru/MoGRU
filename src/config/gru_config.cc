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
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "base/config_file_stream.h"
#include "base/file_util.h"
#include "base/system_util.h"
#include "protocol/commands.pb.h"

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
  }
#else
  if (const char* home = std::getenv("HOME")) {
    add_unique(FileUtil::JoinPath(home, "Library/Application Support/MoGRU"));
    add_unique(FileUtil::JoinPath(home, ".config/mogru"));
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
      add_legacy(FileUtil::JoinPath(home, ".mogru"));
    }
#else
    if (const char* home = std::getenv("HOME")) {
      add_legacy(FileUtil::JoinPath(home, "Library/Application Support/Mozc"));
      add_legacy(FileUtil::JoinPath(
          home, "Library/Application Support/Google/JapaneseInput"));
      add_legacy(FileUtil::JoinPath(home, ".config/mozc"));
      add_legacy(FileUtil::JoinPath(home, ".mozc"));
      add_legacy(FileUtil::JoinPath(home, ".mogru"));
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
      for (const auto& entry :
           std::filesystem::directory_iterator(legacy_dir, ec)) {
        if (ec) {
          break;
        }
        if (!entry.is_regular_file(ec)) {
          continue;
        }
        const std::string fname = entry.path().filename().string();
        const bool is_user_model =
            (absl::StartsWith(fname, "grumodel_user") &&
             absl::EndsWith(fname, ".bin"));
        const bool is_config_or_stats =
            (fname == "gru_config.ini" || fname == "conversion_stats.json" ||
             fname == "conversion_history.tsv");
        const bool is_stale_base =
            (fname == "grumodel.bin" || fname == "chardata.bin" ||
             fname == "grumodel_new.bin" || fname == "chardata_new.bin" ||
             fname == "gru_config.ini.bak");

        if (is_user_model || is_config_or_stats) {
          const std::string target_file =
              FileUtil::JoinPath(primary_user_dir, fname);
          if (!std::filesystem::exists(target_file, ec)) {
            std::filesystem::create_directories(primary_user_dir, ec);
            std::filesystem::copy_file(
                entry.path(), target_file,
                std::filesystem::copy_options::overwrite_existing, ec);
          }
          std::filesystem::remove(entry.path(), ec);
        } else if (is_stale_base) {
          std::filesystem::remove(entry.path(), ec);
        }
      }

      // If legacy directory is ~/.mogru, delete the directory itself if empty
      if (legacy_dir.find(".mogru") != std::string::npos) {
        std::filesystem::remove(legacy_dir, ec);
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

    // Self-heal any corrupted double-profile filenames (e.g. grumodel_user_business_business.bin)
    for (const auto& udir : user_dirs) {
      if (!std::filesystem::exists(udir, ec)) continue;
      for (const auto& entry : std::filesystem::directory_iterator(udir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string fname = entry.path().filename().string();
        for (const char* kp : {"default", "business", "coding", "casual", "creative", "instant"}) {
          const std::string dbl = std::string("_") + kp + "_" + kp + ".bin";
          if (absl::EndsWith(fname, dbl)) {
            const std::string correct_fname = (std::string(kp) == "default")
                                                  ? "grumodel_user.bin"
                                                  : ("grumodel_user_" + std::string(kp) + ".bin");
            const std::string correct_path = FileUtil::JoinPath(udir, correct_fname);
            if (!std::filesystem::exists(correct_path, ec)) {
              std::filesystem::rename(entry.path(), correct_path, ec);
            } else {
              std::filesystem::remove(entry.path(), ec);
            }
            break;
          }
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

  if (IsInTestEnvironment()) {
    const std::string test_profile_dir = SystemUtil::GetUserProfileDirectory();
    if (!test_profile_dir.empty()) {
      std::error_code ec;
      bool dir_usable = std::filesystem::exists(test_profile_dir, ec);
      if (!dir_usable) {
        dir_usable = std::filesystem::create_directories(test_profile_dir, ec);
      }
      if (dir_usable) {
        const std::string test_cfg =
            FileUtil::JoinPath(test_profile_dir, "gru_config.ini");
        if (allow_in_test || std::filesystem::exists(test_cfg)) {
          return test_cfg;
        }
      }
    }
    if (const char* test_tmpdir = std::getenv("TEST_TMPDIR")) {
      if (test_tmpdir[0] != '\0') {
        const std::string test_cfg =
            FileUtil::JoinPath(test_tmpdir, "gru_config.ini");
        if (allow_in_test || std::filesystem::exists(test_cfg)) {
          return test_cfg;
        }
      }
    }
    if (!allow_in_test) {
      return "";
    }
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
  if (IsInTestEnvironment()) {
    const std::string test_profile_dir = SystemUtil::GetUserProfileDirectory();
    if (!test_profile_dir.empty()) {
      std::error_code ec;
      if (std::filesystem::exists(test_profile_dir, ec) ||
          std::filesystem::create_directories(test_profile_dir, ec)) {
        return FileUtil::JoinPath(test_profile_dir, "grumodel_user.bin");
      }
    }
    if (const char* test_tmpdir = std::getenv("TEST_TMPDIR")) {
      if (test_tmpdir[0] != '\0') {
        return FileUtil::JoinPath(test_tmpdir, "grumodel_user.bin");
      }
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

std::string GruConfigManager::ResolveProfileUserModelFilePath(
    const std::string& profile_name,
    const std::string& user_specified) {
  const std::string prof = (profile_name.empty() || profile_name == "default")
                               ? "default"
                               : profile_name;

  if (user_specified.empty()) {
    const std::string filename = (prof == "default")
                                     ? "grumodel_user.bin"
                                     : ("grumodel_user_" + prof + ".bin");
    MigrateLegacyFilesOnce();
    if (IsInTestEnvironment()) {
      const std::string test_profile_dir = SystemUtil::GetUserProfileDirectory();
      if (!test_profile_dir.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(test_profile_dir, ec) ||
            std::filesystem::create_directories(test_profile_dir, ec)) {
          return FileUtil::JoinPath(test_profile_dir, filename);
        }
      }
      if (const char* test_tmpdir = std::getenv("TEST_TMPDIR")) {
        if (test_tmpdir[0] != '\0') {
          return FileUtil::JoinPath(test_tmpdir, filename);
        }
      }
    }
    const auto dirs = GetUserProfileCandidateDirs();
    for (const auto& dir : dirs) {
      const std::string p = FileUtil::JoinPath(dir, filename);
      if (std::filesystem::exists(p)) {
        return p;
      }
    }
    if (!dirs.empty()) {
      return FileUtil::JoinPath(dirs.front(), filename);
    }
    return "";
  }

  // user_specified is provided.
  std::string path = ExpandTilde(user_specified);
  std::string dir = FileUtil::Dirname(path);
  std::string basename = FileUtil::Basename(path);

  // If basename starts with grumodel_user:
  // e.g. grumodel_user.bin, grumodel_user_business.bin, grumodel_user_business_business.bin
  if (absl::StartsWith(basename, "grumodel_user")) {
    std::string new_basename = (prof == "default")
                                   ? "grumodel_user.bin"
                                   : ("grumodel_user_" + prof + ".bin");
    if (dir.empty()) {
      return ResolveProfileUserModelFilePath(prof, "");
    }
    return FileUtil::JoinPath(dir, new_basename);
  }

  // For generic custom names like /path/to/custom_model.bin:
  // Strip known profile suffixes if already present (e.g. _business.bin, _coding.bin, etc.)
  const std::string ext = ".bin";
  std::string stem = basename;
  bool has_bin = false;
  if (absl::EndsWith(stem, ext)) {
    stem = stem.substr(0, stem.size() - ext.size());
    has_bin = true;
  }

  // Remove existing profile suffix if present (e.g., _business, _business_business)
  static const char* kKnownProfiles[] = {
      "default", "business", "coding", "casual", "creative", "instant"};
  bool stripped = true;
  while (stripped) {
    stripped = false;
    for (const char* kp : kKnownProfiles) {
      const std::string suffix = std::string("_") + kp;
      if (absl::EndsWith(stem, suffix)) {
        stem = stem.substr(0, stem.size() - suffix.size());
        stripped = true;
        break;
      }
    }
    if (!stripped && !prof.empty()) {
      const std::string custom_suffix = "_" + prof;
      if (absl::EndsWith(stem, custom_suffix)) {
        stem = stem.substr(0, stem.size() - custom_suffix.size());
        stripped = true;
      }
    }
  }

  std::string new_basename;
  if (prof == "default") {
    new_basename = has_bin ? (stem + ext) : stem;
  } else {
    new_basename = stem + "_" + prof + (has_bin ? ext : "");
  }

  if (dir.empty()) {
    return new_basename;
  }
  return FileUtil::JoinPath(dir, new_basename);
}

namespace {

struct ActiveProfileCacheState {
  absl::Mutex mutex;
  absl::Time last_check_time = absl::InfinitePast();
  int64_t last_mtime = -1;
  std::string last_path;
  GruPenaltyConfig cached_root_config;
  std::string active_profile = "default";
  bool has_cached = false;
};

ActiveProfileCacheState& GetActiveProfileCacheState() {
  static absl::NoDestructor<ActiveProfileCacheState> state;
  return *state;
}

}  // namespace

std::string GruConfigManager::SanitizeProfileName(
    absl::string_view profile_name) {
  std::string out;
  out.reserve(profile_name.size());
  for (const char c : profile_name) {
    if (absl::ascii_isalnum(static_cast<unsigned char>(c)) || c == '_' ||
        c == '-') {
      out.push_back(c);
    }
  }
  if (out.empty() || out == "default") {
    return "default";
  }
  return out;
}

std::string GruConfigManager::ResolveProfileHistoryFileName(
    absl::string_view base_uri,
    absl::string_view profile_name) {
  const std::string prof = SanitizeProfileName(profile_name);
  if (prof == "default") {
    return ConfigFileStream::GetFileName(base_uri);
  }
  std::string profile_uri;
  if (absl::EndsWith(base_uri, ".db")) {
    profile_uri = absl::StrCat(
        base_uri.substr(0, base_uri.size() - 3), "_", prof, ".db");
  } else {
    profile_uri = absl::StrCat(base_uri, "_", prof);
  }
  return ConfigFileStream::GetFileName(profile_uri);
}

void GruConfigManager::InvalidateConfigCache() {
  auto& state = GetActiveProfileCacheState();
  absl::MutexLock lock(state.mutex);
  state.last_check_time = absl::InfinitePast();
  state.last_mtime = -1;
  state.has_cached = false;
}

std::string GruConfigManager::ResolveEffectiveActiveProfile(
    const commands::Context* context,
    const std::string& explicit_config_path) {
  auto& state = GetActiveProfileCacheState();
  absl::MutexLock lock(state.mutex);

  const std::string path =
      ResolveConfigFilePath(explicit_config_path, /*allow_in_test=*/false);
  if (path.empty()) {
    if (state.has_cached && !state.last_path.empty()) {
      state.cached_root_config = GruPenaltyConfig();
      state.active_profile = "default";
      state.last_path.clear();
      state.last_mtime = -1;
      state.has_cached = false;
    }
    if (!state.has_cached) {
      state.cached_root_config = GruPenaltyConfig();
      ApplyEnvOverrides(&state.cached_root_config);
      state.active_profile =
          SanitizeProfileName(state.cached_root_config.current_profile);
      state.has_cached = true;
    }
  } else {
    const absl::Time now = absl::Now();
    if (!state.has_cached || path != state.last_path ||
        (now - state.last_check_time) >= absl::Seconds(1)) {
      state.last_check_time = now;
      EnsureConfigFileExists(path);
      const int64_t mtime = GetFileMtime(path);
      if (!state.has_cached || path != state.last_path ||
          mtime != state.last_mtime) {
        state.last_path = path;
        state.last_mtime = mtime;
        state.cached_root_config = ParseConfigFile(path, GruPenaltyConfig());
        ApplyEnvOverrides(&state.cached_root_config);
        state.active_profile =
            SanitizeProfileName(state.cached_root_config.current_profile);
        state.has_cached = true;
      }
    }
  }

  if (state.cached_root_config.enable_app_profile_switching) {
    const std::string app_name = GetCurrentApplicationName(context);
    if (!app_name.empty()) {
      const std::string target_profile =
          DetectProfileFromApplication(app_name, state.cached_root_config);
      const std::string effective = target_profile.empty()
                                        ? state.cached_root_config.current_profile
                                        : target_profile;
      state.active_profile = SanitizeProfileName(effective);
    }
  }

  return state.active_profile;
}

std::map<std::string, GruPenaltyConfig> GruConfigManager::GetBuiltinDefaultProfiles(
    const GruPenaltyConfig& base_config) {
  std::map<std::string, GruPenaltyConfig> map;

  // 1. Business profile: formal language, no emojis, strict katakana/alphabet, gentle learning
  GruPenaltyConfig business = base_config;
  business.current_profile = "business";
  business.emoji_penalty = 30000.0;
  business.katakana_multiplier = 1.40;
  business.alphabet_multiplier = 2.00;
  business.online_learning_rate = 0.02000;
  business.online_learning_weight_decay = 0.00500;
  business.resegment_threshold = 800.0;
  map["business"] = business;

  // 2. Coding profile: programming, identifiers, low alphabet penalty, technical katakana, English fallback
  GruPenaltyConfig coding = base_config;
  coding.current_profile = "coding";
  coding.alphabet_multiplier = 1.00;
  coding.katakana_multiplier = 1.10;
  coding.english_fallback_cost_threshold = 10000.0;
  coding.online_learning_rate = 0.08000;
  coding.resegment_threshold = 500.0;
  map["coding"] = coding;

  // 3. Casual / SNS profile: relaxed emoji penalty, slangs, colloquial speech
  GruPenaltyConfig casual = base_config;
  casual.current_profile = "casual";
  casual.emoji_penalty = 0.0;
  casual.katakana_multiplier = 1.00;
  casual.hiragana_multiplier = 1.00;
  casual.online_learning_rate = 0.10000;
  map["casual"] = casual;

  // 4. Creative profile: literary nuance, higher GRU score weight, responsive resegmentation
  GruPenaltyConfig creative = base_config;
  creative.current_profile = "creative";
  creative.gru_score_weight = 1200.0;
  creative.resegment_threshold = 400.0;
  creative.katakana_multiplier = 1.20;
  map["creative"] = creative;

  // 5. Instant profile: extreme learning rate for immediate one-shot adaptation
  GruPenaltyConfig instant = base_config;
  instant.current_profile = "instant";
  instant.enable_instant_mode = true;
  instant.instant_learning_rate = 0.80;
  instant.instant_max_weight_delta = 2.0;
  instant.online_learning_rate = 0.80;
  instant.online_learning_mode = "output_only";
  map["instant"] = instant;

  return map;
}

std::vector<std::string> GruConfigManager::GetAvailableProfiles(
    const GruPenaltyConfig& config) {
  std::vector<std::string> result;
  std::set<std::string> seen;
  auto add_prof = [&](const std::string& name) {
    if (name.empty()) return;
    if (name != "default" && config.deleted_profiles.count(name) > 0) return;
    if (seen.insert(name).second) {
      result.push_back(name);
    }
  };

  add_prof("default");
  for (const char* preset :
       {"business", "coding", "casual", "creative", "instant"}) {
    add_prof(preset);
  }
  for (const auto& kv : config.profiles) {
    add_prof(kv.first);
  }
  if (!config.current_profile.empty()) {
    add_prof(SanitizeProfileName(config.current_profile));
  }
  return result;
}

bool GruConfigManager::AddProfile(
    GruPenaltyConfig* config,
    absl::string_view profile_name,
    const GruPenaltyConfig* base_profile) {
  if (config == nullptr) return false;
  const std::string prof = SanitizeProfileName(profile_name);
  if (prof.empty() || prof == "default") {
    return false;
  }
  config->deleted_profiles.erase(prof);
  if (base_profile != nullptr) {
    GruPenaltyConfig p = *base_profile;
    p.current_profile = prof;
    p.profiles.clear();
    p.deleted_profiles.clear();
    config->profiles[prof] = p;
  } else {
    const auto builtin = GetBuiltinDefaultProfiles(*config);
    auto it_b = builtin.find(prof);
    if (it_b != builtin.end()) {
      config->profiles[prof] = it_b->second;
    } else if (config->profiles.find(prof) == config->profiles.end()) {
      GruPenaltyConfig p = *config;
      p.current_profile = prof;
      p.profiles.clear();
      p.deleted_profiles.clear();
      config->profiles[prof] = p;
    }
  }
  return true;
}

bool GruConfigManager::DeleteProfile(
    GruPenaltyConfig* config,
    absl::string_view profile_name,
    bool remove_data_files) {
  if (config == nullptr) return false;
  const std::string prof = SanitizeProfileName(profile_name);
  if (prof.empty() || prof == "default") {
    return false;
  }
  config->profiles.erase(prof);
  config->deleted_profiles.insert(prof);
  if (SanitizeProfileName(config->current_profile) == prof) {
    config->current_profile = "default";
  }

  if (!config->app_profile_map.empty()) {
    std::vector<absl::string_view> pairs =
        absl::StrSplit(config->app_profile_map, ',', absl::SkipEmpty());
    std::vector<std::string> remaining;
    for (auto pair : pairs) {
      std::pair<absl::string_view, absl::string_view> kv =
          absl::StrSplit(pair, ':');
      std::string key = std::string(absl::StripAsciiWhitespace(kv.first));
      std::string val = std::string(absl::StripAsciiWhitespace(kv.second));
      if (!key.empty() && !val.empty() && SanitizeProfileName(val) != prof) {
        remaining.push_back(absl::StrCat(key, ":", val));
      }
    }
    config->app_profile_map = absl::StrJoin(remaining, ",");
  }

  if (remove_data_files) {
    std::error_code ec;
    const std::string model_path =
        ResolveProfileUserModelFilePath(prof, config->gru_user_model_path);
    if (!model_path.empty()) {
      std::filesystem::remove(model_path, ec);
    }
    for (const char* uri :
         {"user://segment.db", "user://boundary.db", "user://.history.db"}) {
      const std::string db_path = ResolveProfileHistoryFileName(uri, prof);
      if (!db_path.empty()) {
        std::filesystem::remove(db_path, ec);
      }
    }
  }

  return true;
}

GruPenaltyConfig GruConfigManager::GetEffectiveProfileConfig(
    const GruPenaltyConfig& root_config,
    const std::string& profile_name) {
  if (profile_name.empty() || profile_name == "default" ||
      root_config.deleted_profiles.count(profile_name) > 0) {
    GruPenaltyConfig eff = root_config;
    eff.current_profile = "default";
    eff.gru_user_model_path = ResolveProfileUserModelFilePath("default", root_config.gru_user_model_path);
    return eff;
  }

  GruPenaltyConfig eff = root_config;
  eff.current_profile = profile_name;

  const auto builtin = GetBuiltinDefaultProfiles(root_config);
  auto it_builtin = builtin.find(profile_name);
  if (it_builtin != builtin.end()) {
    const auto& p = it_builtin->second;
    eff.gru_score_weight = p.gru_score_weight;
    eff.katakana_multiplier = p.katakana_multiplier;
    eff.hiragana_multiplier = p.hiragana_multiplier;
    eff.hiragana_length_power = p.hiragana_length_power;
    eff.emoji_penalty = p.emoji_penalty;
    eff.alphabet_multiplier = p.alphabet_multiplier;
    eff.enable_resegmentation = p.enable_resegmentation;
    eff.resegment_threshold = p.resegment_threshold;
    eff.enable_online_learning = p.enable_online_learning;
    eff.online_learning_mode = p.online_learning_mode;
    eff.online_learning_rate = p.online_learning_rate;
    eff.online_learning_weight_decay = p.online_learning_weight_decay;
    eff.enable_english_fallback = p.enable_english_fallback;
    eff.english_fallback_cost_threshold = p.english_fallback_cost_threshold;
    if (!p.user_english_words.empty()) {
      eff.user_english_words = p.user_english_words;
    }
    eff.enable_instant_mode = p.enable_instant_mode;
    eff.instant_learning_rate = p.instant_learning_rate;
    eff.instant_max_weight_delta = p.instant_max_weight_delta;
  }

  auto it_custom = root_config.profiles.find(profile_name);
  if (it_custom != root_config.profiles.end()) {
    const auto& p = it_custom->second;
    if (p.gru_score_weight != 0.0) eff.gru_score_weight = p.gru_score_weight;
    eff.katakana_multiplier = p.katakana_multiplier;
    eff.hiragana_multiplier = p.hiragana_multiplier;
    eff.hiragana_length_power = p.hiragana_length_power;
    eff.emoji_penalty = p.emoji_penalty;
    eff.alphabet_multiplier = p.alphabet_multiplier;
    eff.enable_resegmentation = p.enable_resegmentation;
    eff.resegment_threshold = p.resegment_threshold;
    eff.enable_online_learning = p.enable_online_learning;
    eff.online_learning_mode = p.online_learning_mode;
    eff.online_learning_rate = p.online_learning_rate;
    eff.online_learning_weight_decay = p.online_learning_weight_decay;
    eff.enable_english_fallback = p.enable_english_fallback;
    eff.english_fallback_cost_threshold = p.english_fallback_cost_threshold;
    if (!p.user_english_words.empty()) {
      eff.user_english_words = p.user_english_words;
    }
    eff.enable_instant_mode = p.enable_instant_mode;
    eff.instant_learning_rate = p.instant_learning_rate;
    eff.instant_max_weight_delta = p.instant_max_weight_delta;
  }

  eff.gru_user_model_path = ResolveProfileUserModelFilePath(profile_name, root_config.gru_user_model_path);
  return eff;
}

std::string GruConfigManager::DetectProfileFromApplication(
    const std::string& app_name,
    const GruPenaltyConfig& config) {
  if (!config.enable_app_profile_switching || app_name.empty()) {
    return "";
  }
  std::string lower_app = absl::AsciiStrToLower(app_name);
  std::vector<std::string_view> pairs =
      absl::StrSplit(config.app_profile_map, ',', absl::SkipEmpty());
  for (auto pair : pairs) {
    std::pair<std::string_view, std::string_view> kv = absl::StrSplit(pair, ':');
    std::string key = std::string(absl::StripAsciiWhitespace(kv.first));
    std::string val = std::string(absl::StripAsciiWhitespace(kv.second));
    if (!key.empty() && !val.empty()) {
      if (config.deleted_profiles.count(val) > 0) {
        continue;
      }
      std::string lower_key = absl::AsciiStrToLower(key);
      if (lower_app.find(lower_key) != std::string::npos) {
        return val;
      }
    }
  }
  return "";
}

static bool IsCompositorOrProxyAppName(absl::string_view name) {
  std::string lower = absl::AsciiStrToLower(name);
  return lower == "gnome-shell" || lower == "kwin_wayland" ||
         lower == "mutter" || lower == "weston" || lower == "xwayland";
}

std::string GruConfigManager::GetCurrentApplicationName(
    const commands::Context* context) {
  std::string app_name;
  if (context != nullptr) {
    if (context->has_application_name() && !context->application_name().empty()) {
      app_name = context->application_name();
    }
    if (app_name.empty()) {
      for (const auto& feat : context->experimental_features()) {
        if (absl::StartsWith(feat, "app:")) {
          app_name = feat.substr(4);
          break;
        }
      }
    }
  }
  if (app_name.empty()) {
    const char* env_app = getenv("MOGRU_APP_NAME");
    if (env_app && *env_app) {
      app_name = env_app;
    }
  }
  if (app_name.empty()) {
    const char* mozc_app = getenv("MOZC_CURRENT_APP");
    if (mozc_app && *mozc_app) {
      app_name = mozc_app;
    }
  }
  if (IsCompositorOrProxyAppName(app_name)) {
    return "";
  }
  return app_name;
}

void GruConfigManager::AddOrUpdateAppProfileMapping(
    GruPenaltyConfig* config,
    absl::string_view app_name,
    absl::string_view profile_name) {
  if (config == nullptr || app_name.empty() || profile_name.empty()) {
    return;
  }
  std::string lower_app = absl::AsciiStrToLower(app_name);
  std::vector<std::string_view> pairs =
      absl::StrSplit(config->app_profile_map, ',', absl::SkipEmpty());
  std::vector<std::string> updated_pairs;
  bool found = false;
  for (auto pair : pairs) {
    std::pair<std::string_view, std::string_view> kv = absl::StrSplit(pair, ':');
    std::string key = std::string(absl::StripAsciiWhitespace(kv.first));
    std::string val = std::string(absl::StripAsciiWhitespace(kv.second));
    if (!key.empty()) {
      if (absl::AsciiStrToLower(key) == lower_app) {
        updated_pairs.push_back(absl::StrCat(key, ":", profile_name));
        found = true;
      } else {
        updated_pairs.push_back(absl::StrCat(key, ":", val));
      }
    }
  }
  if (!found) {
    updated_pairs.push_back(absl::StrCat(app_name, ":", profile_name));
  }
  config->app_profile_map = absl::StrJoin(updated_pairs, ",");
  config->enable_app_profile_switching = true;
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
  const std::string base_ini = absl::StrFormat(
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
      "# 【ライブ変換で除外する直近の平仮名文字数】 (初期値: 0)\n"
      "# - 入力途中の末尾アルファベットに加え、直近指定文字数の平仮名をライブ変換の対象から除外（遅延変換）します。\n"
      "# - 0: アルファベットのみ除外（従来通り）, 1以上: 直近N文字の平仮名も除外\n"
      "live_conversion_excluded_hiragana_characters = %d\n"
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
      "online_learning_save_interval = %d\n"
      "\n"
      "# 【高コスト英単語自動フォールバックの有効化】 (初期値: true)\n"
      "# - 通常変換でコストがしきい値を超えたセグメントに対し、英単語の自動検出を行います。\n"
      "enable_english_fallback = %s\n"
      "\n"
      "# 【英単語フォールバックを発動するコストしきい値】 (初期値: 15000.0)\n"
      "english_fallback_cost_threshold = %.1f\n"
      "\n"
      "# 【英単語フォールバック対象の最小単語長】 (初期値: 3)\n"
      "# - 2文字以下の短単語（in, to 等）による誤爆を防ぎます。\n"
      "english_fallback_min_word_length = %d\n"
      "\n"
      "# 【ライブ変換時の英単語自動フォールバックの有効化】 (初期値: true)\n"
      "enable_live_conversion_english_fallback = %s\n"
      "\n"
      "# 【ユーザー追加英単語リスト】 (カンマ区切り)\n"
      "# - 英語フォールバック対象として追加登録する英単語（例: kubernetes, terraform, rebase）\n"
      "user_english_words = %s\n"
      "\n"
      "# 【変換正確率統計の記録の有効化】 (初期値: true)\n"
      "# - ライブ変換およびスペースキー変換の正確率を記録して統計ファイルに保存します。\n"
      "enable_conversion_stats = %s\n"
      "\n"
      "# 【変換正確率統計ファイル (JSON) の保存先パス】 (空欄で ~/.config/mogru/conversion_stats.json)\n"
      "conversion_stats_path = %s\n"
      "\n"
      "# 【確定ごとの詳細履歴ログ (TSV) の記録の有効化】 (初期値: false)\n"
      "# - 有効にすると、確定ごとの詳細を conversion_history.tsv に追記します。\n"
      "enable_conversion_history_log = %s\n"
      "\n"
      "# 【現在有効なプロファイル名】 (初期値: default)\n"
      "# - default, business, coding, casual, creative, instant などから指定\n"
      "current_profile = %s\n"
      "\n"
      "# 【アプリケーション別プロファイル自動切り替えの有効化】 (初期値: true)\n"
      "enable_app_profile_switching = %s\n"
      "\n"
      "# 【アプリケーション名とプロファイルの紐付け定義】 (app_name:profile_name)\n"
      "app_profile_map = %s\n"
      "\n"
      "# 【インスタント速習モードの有効化】 (初期値: false)\n"
      "# - 1回の確定で対象単語を極限まで高速学習し、次回入力時の最上位候補へ昇格させます。\n"
      "enable_instant_mode = %s\n"
      "\n"
      "# 【インスタントモード時の学習率】 (初期値: 0.80)\n"
      "instant_learning_rate = %.5f\n"
      "\n"
      "# 【インスタントモード時の1確定あたりの最大重み変化量】 (初期値: 2.0)\n"
      "instant_max_weight_delta = %.2f\n",
      config.enable_gru_rewriter ? "true" : "false",
      config.enable_live_conversion ? "true" : "false",
      config.show_segment_separator_in_conversion ? "true" : "false",
      config.show_segment_separator_in_live_conversion ? "true" : "false",
      config.enable_live_conversion_prefix_freezing ? "true" : "false",
      config.enable_live_conversion_auto_commit ? "true" : "false",
      config.live_conversion_length_threshold,
      config.live_conversion_excluded_hiragana_characters,
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
      config.online_learning_save_interval,
      config.enable_english_fallback ? "true" : "false",
      config.english_fallback_cost_threshold,
      config.english_fallback_min_word_length,
      config.enable_live_conversion_english_fallback ? "true" : "false",
      absl::StrJoin(config.user_english_words, ", ").c_str(),
      config.enable_conversion_stats ? "true" : "false",
      config.conversion_stats_path,
      config.enable_conversion_history_log ? "true" : "false",
      config.current_profile.empty() ? "default" : config.current_profile.c_str(),
      config.enable_app_profile_switching ? "true" : "false",
      config.app_profile_map.c_str(),
      config.enable_instant_mode ? "true" : "false",
      config.instant_learning_rate,
      config.instant_max_weight_delta);

  std::map<std::string, GruPenaltyConfig> effective_profiles;
  for (const auto& kv : GetBuiltinDefaultProfiles(config)) {
    if (config.deleted_profiles.count(kv.first) == 0) {
      effective_profiles[kv.first] = kv.second;
    }
  }
  for (const auto& kv : config.profiles) {
    if (kv.first != "default" && config.deleted_profiles.count(kv.first) == 0) {
      effective_profiles[kv.first] = kv.second;
    }
  }

  std::string result = base_ini;
  if (!config.deleted_profiles.empty()) {
    result += absl::StrFormat(
        "\n# 【削除された組み込みプロファイル一覧】\n"
        "deleted_profiles = %s\n",
        absl::StrJoin(config.deleted_profiles, ","));
  }
  result +=
      "\n# ==============================================================================\n"
      "# プロファイル別オーバーライド設定\n"
      "# ==============================================================================\n"
      "# 指定したプロファイルが選択された際、上記の全体デフォルトから変更したい項目のみ記述します。\n"
      "# 記述されていない項目は全体デフォルト値が自動的に引き継がれます。\n";

  for (const auto& kv : effective_profiles) {
    const std::string& name = kv.first;
    const GruPenaltyConfig& p = kv.second;
    if (name == "default") continue;

    result += absl::StrFormat("\n[profile.%s]\n", name);
    if (p.gru_score_weight != config.gru_score_weight) {
      result += absl::StrFormat("gru_score_weight = %.1f\n", p.gru_score_weight);
    }
    if (p.katakana_multiplier != config.katakana_multiplier) {
      result += absl::StrFormat("katakana_multiplier = %.2f\n", p.katakana_multiplier);
    }
    if (p.hiragana_multiplier != config.hiragana_multiplier) {
      result += absl::StrFormat("hiragana_multiplier = %.2f\n", p.hiragana_multiplier);
    }
    if (p.hiragana_length_power != config.hiragana_length_power) {
      result += absl::StrFormat("hiragana_length_power = %.2f\n", p.hiragana_length_power);
    }
    if (p.emoji_penalty != config.emoji_penalty) {
      result += absl::StrFormat("emoji_penalty = %.1f\n", p.emoji_penalty);
    }
    if (p.alphabet_multiplier != config.alphabet_multiplier) {
      result += absl::StrFormat("alphabet_multiplier = %.2f\n", p.alphabet_multiplier);
    }
    if (p.enable_resegmentation != config.enable_resegmentation) {
      result += absl::StrFormat("enable_resegmentation = %s\n",
                                p.enable_resegmentation ? "true" : "false");
    }
    if (p.resegment_threshold != config.resegment_threshold) {
      result += absl::StrFormat("resegment_threshold = %.1f\n", p.resegment_threshold);
    }
    if (p.enable_online_learning != config.enable_online_learning) {
      result += absl::StrFormat("enable_online_learning = %s\n",
                                p.enable_online_learning ? "true" : "false");
    }
    if (p.online_learning_mode != config.online_learning_mode) {
      result += absl::StrFormat("online_learning_mode = %s\n",
                                p.online_learning_mode);
    }
    if (p.online_learning_rate != config.online_learning_rate) {
      result += absl::StrFormat("online_learning_rate = %.5f\n", p.online_learning_rate);
    }
    if (p.online_learning_weight_decay != config.online_learning_weight_decay) {
      result += absl::StrFormat("online_learning_weight_decay = %.5f\n", p.online_learning_weight_decay);
    }
    if (p.enable_english_fallback != config.enable_english_fallback) {
      result += absl::StrFormat("enable_english_fallback = %s\n",
                                p.enable_english_fallback ? "true" : "false");
    }
    if (p.english_fallback_cost_threshold != config.english_fallback_cost_threshold) {
      result += absl::StrFormat("english_fallback_cost_threshold = %.1f\n", p.english_fallback_cost_threshold);
    }
    if (p.user_english_words != config.user_english_words) {
      result += absl::StrFormat("user_english_words = %s\n",
                                absl::StrJoin(p.user_english_words, ", "));
    }
    if (p.enable_instant_mode != config.enable_instant_mode) {
      result += absl::StrFormat("enable_instant_mode = %s\n", p.enable_instant_mode ? "true" : "false");
    }
    if (p.instant_learning_rate != config.instant_learning_rate) {
      result += absl::StrFormat("instant_learning_rate = %.5f\n", p.instant_learning_rate);
    }
    if (p.instant_max_weight_delta != config.instant_max_weight_delta) {
      result += absl::StrFormat("instant_max_weight_delta = %.2f\n", p.instant_max_weight_delta);
    }
  }

  return result;
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
  if (content.find("live_conversion_excluded_hiragana_characters") == std::string::npos) {
    append_content +=
        "\n# 【ライブ変換で除外する直近の平仮名文字数】 (初期値: 0)\n"
        "live_conversion_excluded_hiragana_characters = 0\n";
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
  if (content.find("enable_english_fallback") == std::string::npos) {
    append_content +=
        "\n# 【高コスト英単語自動フォールバックの有効化】 (初期値: true)\n"
        "enable_english_fallback = true\n";
  }
  if (content.find("english_fallback_cost_threshold") == std::string::npos) {
    append_content +=
        "\n# 【英単語フォールバックを発動するコストしきい値】 (初期値: 15000.0)\n"
        "english_fallback_cost_threshold = 15000.0\n";
  }
  if (content.find("english_fallback_min_word_length") == std::string::npos) {
    append_content +=
        "\n# 【英単語フォールバック対象の最小単語長】 (初期値: 3)\n"
        "english_fallback_min_word_length = 3\n";
  }
  if (content.find("enable_live_conversion_english_fallback") == std::string::npos) {
    append_content +=
        "\n# 【ライブ変換時の英単語自動フォールバックの有効化】 (初期値: true)\n"
        "enable_live_conversion_english_fallback = true\n";
  }
  if (content.find("user_english_words") == std::string::npos) {
    append_content +=
        "\n# 【ユーザー追加英単語リスト】 (カンマ区切り)\n"
        "# - 英語フォールバック対象として追加登録する英単語（例: kubernetes, terraform, rebase）\n"
        "user_english_words = \n";
  }
  if (content.find("enable_conversion_stats") == std::string::npos) {
    append_content +=
        "\n# 【変換正確率統計の記録の有効化】 (初期値: true)\n"
        "enable_conversion_stats = true\n";
  }
  if (content.find("conversion_stats_path") == std::string::npos) {
    append_content +=
        "\n# 【変換正確率統計ファイル (JSON) の保存先パス】 (空欄で ~/.config/mogru/conversion_stats.json)\n"
        "conversion_stats_path = \n";
  }
  if (content.find("enable_conversion_history_log") == std::string::npos) {
    append_content +=
        "\n# 【確定ごとの詳細履歴ログ (TSV) の記録の有効化】 (初期値: false)\n"
        "enable_conversion_history_log = false\n";
  }
  if (content.find("current_profile") == std::string::npos) {
    append_content +=
        "\n# 【現在有効なプロファイル名】 (初期値: default)\n"
        "# - default, business, coding, casual, creative, instant などから指定\n"
        "current_profile = default\n";
  }
  if (content.find("enable_app_profile_switching") == std::string::npos) {
    append_content +=
        "\n# 【アプリケーション別プロファイル自動切り替えの有効化】 (初期値: true)\n"
        "enable_app_profile_switching = true\n";
  }
  if (content.find("app_profile_map") == std::string::npos) {
    append_content +=
        "\n# 【アプリケーション名とプロファイルの紐付け定義】 (app_name:profile_name)\n"
        "app_profile_map = code:coding,cursor:coding,slack:business,discord:casual\n";
  }
  if (content.find("enable_instant_mode") == std::string::npos) {
    append_content +=
        "\n# 【インスタント速習モードの有効化】 (初期値: false)\n"
        "# - 1回の確定で対象単語を極限まで高速学習し、次回入力時の最上位候補へ昇格させます。\n"
        "enable_instant_mode = false\n";
  }
  if (content.find("instant_learning_rate") == std::string::npos) {
    append_content +=
        "\n# 【インスタントモード時の学習率】 (初期値: 0.80)\n"
        "instant_learning_rate = 0.80\n";
  }
  if (content.find("instant_max_weight_delta") == std::string::npos) {
    append_content +=
        "\n# 【インスタントモード時の1確定あたりの最大重み変化量】 (初期値: 2.0)\n"
        "instant_max_weight_delta = 2.0\n";
  }

  if (!append_content.empty()) {
    std::ofstream ofs(path, std::ios::app);
    if (ofs.is_open()) {
      ofs << append_content;
      LOG(INFO) << "GruConfigManager: Appended missing config options to: " << path;
    }
  }
}

namespace {
void ApplyConfigKeyValue(GruPenaltyConfig* config,
                         absl::string_view key,
                         absl::string_view val,
                         bool in_test,
                         bool* has_katakana_mult = nullptr,
                         bool* has_hiragana_mult = nullptr,
                         double* legacy_katakana_penalty = nullptr,
                         bool* has_legacy_katakana_penalty = nullptr,
                         double* legacy_hiragana_penalty = nullptr,
                         bool* has_legacy_hiragana_penalty = nullptr) {
  if (config == nullptr) return;

  if (key == "gru_model_path") {
    if (!val.empty()) {
      config->gru_model_path = std::string(val);
    } else if (!in_test) {
      config->gru_model_path.clear();
    }
    return;
  }
  if (key == "gru_chardata_path") {
    if (!val.empty()) {
      config->gru_chardata_path = std::string(val);
    } else if (!in_test) {
      config->gru_chardata_path.clear();
    }
    return;
  }
  if (key == "gru_user_model_path") {
    if (!val.empty()) {
      config->gru_user_model_path = std::string(val);
    } else if (!in_test) {
      config->gru_user_model_path.clear();
    }
    return;
  }
  if (key == "online_learning_mode") {
    if (val == "full" || val == "full_bptt" || val == "bptt" ||
        val == "output_only") {
      config->online_learning_mode =
          (val == "full_bptt" || val == "bptt") ? "full" : std::string(val);
    }
    return;
  }
  if (key == "enable_gru_rewriter") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->enable_gru_rewriter = b;
    return;
  }
  if (key == "enable_live_conversion") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->enable_live_conversion = b;
    return;
  }
  if (key == "show_segment_separator_in_conversion" ||
      key == "show_segment_separator") {
    bool b = false;
    if (absl::SimpleAtob(val, &b)) config->show_segment_separator_in_conversion = b;
    return;
  }
  if (key == "show_segment_separator_in_live_conversion") {
    bool b = false;
    if (absl::SimpleAtob(val, &b)) config->show_segment_separator_in_live_conversion = b;
    return;
  }
  if (key == "enable_live_conversion_prefix_freezing") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->enable_live_conversion_prefix_freezing = b;
    return;
  }
  if (key == "enable_live_conversion_auto_commit") {
    bool b = false;
    if (absl::SimpleAtob(val, &b)) config->enable_live_conversion_auto_commit = b;
    return;
  }
  if (key == "live_conversion_length_threshold") {
    int32_t val_int = 24;
    if (absl::SimpleAtoi(val, &val_int)) {
      config->live_conversion_length_threshold = std::max<int32_t>(1, val_int);
    }
    return;
  }
  if (key == "live_conversion_excluded_hiragana_characters") {
    int32_t val_int = 0;
    if (absl::SimpleAtoi(val, &val_int)) {
      config->live_conversion_excluded_hiragana_characters = std::max<int32_t>(0, val_int);
    }
    return;
  }
  if (key == "show_gru_score") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->show_gru_score = b;
    return;
  }
  if (key == "enable_resegmentation") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->enable_resegmentation = b;
    return;
  }
  if (key == "enable_online_learning") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->enable_online_learning = b;
    return;
  }
  if (key == "online_learning_save_interval") {
    int32_t interval = 5;
    if (absl::SimpleAtoi(val, &interval) && interval >= 1) {
      config->online_learning_save_interval = interval;
    }
    return;
  }
  if (key == "enable_english_fallback") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->enable_english_fallback = b;
    return;
  }
  if (key == "enable_live_conversion_english_fallback") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->enable_live_conversion_english_fallback = b;
    return;
  }
  if (key == "user_english_words") {
    config->user_english_words.clear();
    for (absl::string_view w : absl::StrSplit(val, absl::ByAnyChar(",\r\n;"))) {
      absl::string_view trimmed = absl::StripAsciiWhitespace(w);
      if (!trimmed.empty()) {
        config->user_english_words.push_back(std::string(trimmed));
      }
    }
    return;
  }
  if (key == "enable_conversion_stats") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->enable_conversion_stats = b;
    return;
  }
  if (key == "conversion_stats_path") {
    config->conversion_stats_path = std::string(val);
    return;
  }
  if (key == "enable_conversion_history_log") {
    bool b = false;
    if (absl::SimpleAtob(val, &b)) config->enable_conversion_history_log = b;
    return;
  }
  if (key == "english_fallback_min_word_length") {
    int32_t min_len = 3;
    if (absl::SimpleAtoi(val, &min_len) && min_len >= 1) {
      config->english_fallback_min_word_length = min_len;
    }
    return;
  }

  // Profile and instant mode options
  if (key == "current_profile") {
    config->current_profile = std::string(val);
    return;
  }
  if (key == "enable_app_profile_switching") {
    bool b = true;
    if (absl::SimpleAtob(val, &b)) config->enable_app_profile_switching = b;
    return;
  }
  if (key == "app_profile_map") {
    config->app_profile_map = std::string(val);
    return;
  }
  if (key == "deleted_profiles") {
    config->deleted_profiles.clear();
    for (absl::string_view item : absl::StrSplit(val, ',', absl::SkipEmpty())) {
      const std::string prof =
          GruConfigManager::SanitizeProfileName(absl::StripAsciiWhitespace(item));
      if (!prof.empty() && prof != "default") {
        config->deleted_profiles.insert(prof);
        config->profiles.erase(prof);
      }
    }
    return;
  }
  if (key == "enable_instant_mode") {
    bool b = false;
    if (absl::SimpleAtob(val, &b)) config->enable_instant_mode = b;
    return;
  }

  double d = 0.0;
  if (absl::SimpleAtod(val, &d)) {
    if (key == "instant_learning_rate") {
      config->instant_learning_rate = d;
    } else if (key == "instant_max_weight_delta") {
      config->instant_max_weight_delta = d;
    } else if (key == "english_fallback_cost_threshold") {
      config->english_fallback_cost_threshold = d;
    } else if (key == "gru_score_weight" || key == "score_weight") {
      config->gru_score_weight = d;
    } else if (key == "katakana_multiplier") {
      config->katakana_multiplier = d;
      if (has_katakana_mult) *has_katakana_mult = true;
    } else if (key == "hiragana_multiplier") {
      config->hiragana_multiplier = d;
      if (has_hiragana_mult) *has_hiragana_mult = true;
    } else if (key == "hiragana_length_power") {
      config->hiragana_length_power = d;
    } else if (key == "katakana_penalty") {
      if (legacy_katakana_penalty) *legacy_katakana_penalty = d;
      if (has_legacy_katakana_penalty) *has_legacy_katakana_penalty = true;
    } else if (key == "hiragana_penalty") {
      if (legacy_hiragana_penalty) *legacy_hiragana_penalty = d;
      if (has_legacy_hiragana_penalty) *has_legacy_hiragana_penalty = true;
    } else if (key == "emoji_penalty") {
      config->emoji_penalty = d;
    } else if (key == "alphabet_multiplier") {
      config->alphabet_multiplier = d;
    } else if (key == "resegment_threshold") {
      config->resegment_threshold = d;
    } else if (key == "online_learning_rate") {
      config->online_learning_rate = d;
    } else if (key == "online_learning_weight_decay") {
      config->online_learning_weight_decay = d;
    }
  }
}
}  // namespace

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

  std::string active_section = "";
  std::string line;
  while (std::getline(ifs, line)) {
    absl::string_view sv = line;
    const size_t comment_pos = sv.find_first_of("#;");
    if (comment_pos != absl::string_view::npos) {
      sv = sv.substr(0, comment_pos);
    }
    sv = absl::StripAsciiWhitespace(sv);
    if (sv.empty()) continue;

    if (sv.front() == '[' && sv.back() == ']') {
      std::string sec = std::string(absl::StripAsciiWhitespace(sv.substr(1, sv.size() - 2)));
      if (absl::StartsWith(sec, "profile.")) {
        active_section = GruConfigManager::SanitizeProfileName(sec.substr(8));
      } else if (absl::StartsWith(sec, "profile:")) {
        active_section = GruConfigManager::SanitizeProfileName(sec.substr(8));
      } else if (sec == "default" || sec == "main" || sec == "root") {
        active_section = "";
      } else {
        active_section = GruConfigManager::SanitizeProfileName(sec);
      }
      if (active_section == "default") {
        active_section = "";
      }
      if (!active_section.empty()) {
        config.deleted_profiles.erase(active_section);
        if (config.profiles.find(active_section) == config.profiles.end()) {
          config.profiles[active_section] = config;
          config.profiles[active_section].current_profile = active_section;
          config.profiles[active_section].profiles.clear();
          config.profiles[active_section].deleted_profiles.clear();
        }
      }
      continue;
    }

    const size_t eq_pos = sv.find('=');
    if (eq_pos == absl::string_view::npos) continue;

    absl::string_view key = absl::StripAsciiWhitespace(sv.substr(0, eq_pos));
    absl::string_view val = absl::StripAsciiWhitespace(sv.substr(eq_pos + 1));

    if (active_section.empty()) {
      ApplyConfigKeyValue(&config, key, val, in_test,
                          &has_katakana_mult, &has_hiragana_mult,
                          &legacy_katakana_penalty, &has_legacy_katakana_penalty,
                          &legacy_hiragana_penalty, &has_legacy_hiragana_penalty);
    } else {
      if (config.profiles.find(active_section) == config.profiles.end()) {
        config.profiles[active_section] = config;
        config.profiles[active_section].current_profile = active_section;
      }
      ApplyConfigKeyValue(&config.profiles[active_section], key, val, in_test);
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
  if (const char* e = std::getenv("MOZC_GRU_PROFILE")) {
    if (e[0] != '\0') {
      config->current_profile = std::string(e);
    }
  }
  if (const char* e = std::getenv("MOZC_GRU_INSTANT_MODE")) {
    bool b = false;
    if (absl::SimpleAtob(e, &b)) {
      config->enable_instant_mode = b;
    }
  }
  if (const char* e = std::getenv("MOZC_GRU_INSTANT_LEARNING_RATE")) {
    double d = 0.0;
    if (absl::SimpleAtod(e, &d) && d > 0.0) {
      config->instant_learning_rate = d;
    }
  }
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
  if (const char* e = std::getenv("MOZC_GRU_LIVE_CONVERSION_EXCLUDED_HIRAGANA_CHARACTERS")) {
    int32_t val_int = 0;
    if (absl::SimpleAtoi(e, &val_int)) config->live_conversion_excluded_hiragana_characters = std::max<int32_t>(0, val_int);
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
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_ENGLISH_FALLBACK")) {
    bool b = true;
    if (absl::SimpleAtob(e, &b)) config->enable_english_fallback = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_ENGLISH_FALLBACK_COST_THRESHOLD")) {
    double d = 0.0;
    if (absl::SimpleAtod(e, &d)) config->english_fallback_cost_threshold = d;
  }
  if (const char* e = std::getenv("MOZC_GRU_ENGLISH_FALLBACK_MIN_WORD_LENGTH")) {
    int32_t val_int = 3;
    if (absl::SimpleAtoi(e, &val_int)) config->english_fallback_min_word_length = val_int;
  }
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_LIVE_CONVERSION_ENGLISH_FALLBACK")) {
    bool b = true;
    if (absl::SimpleAtob(e, &b)) config->enable_live_conversion_english_fallback = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_USER_ENGLISH_WORDS")) {
    config->user_english_words.clear();
    for (absl::string_view w : absl::StrSplit(e, absl::ByAnyChar(",\r\n;"))) {
      absl::string_view trimmed = absl::StripAsciiWhitespace(w);
      if (!trimmed.empty()) {
        config->user_english_words.push_back(std::string(trimmed));
      }
    }
  }
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_CONVERSION_STATS")) {
    bool b = true;
    if (absl::SimpleAtob(e, &b)) config->enable_conversion_stats = b;
  }
  if (const char* e = std::getenv("MOZC_GRU_CONVERSION_STATS_PATH")) {
    config->conversion_stats_path = e;
  }
  if (const char* e = std::getenv("MOZC_GRU_ENABLE_CONVERSION_HISTORY_LOG")) {
    bool b = false;
    if (absl::SimpleAtob(e, &b)) config->enable_conversion_history_log = b;
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

  if (any_saved) {
    auto& state = GetActiveProfileCacheState();
    absl::MutexLock lock(state.mutex);
    state.cached_root_config = config;
    ApplyEnvOverrides(&state.cached_root_config);
    state.active_profile =
        SanitizeProfileName(state.cached_root_config.current_profile);
    state.last_path = targets.front();
    state.last_mtime = GetFileMtime(targets.front());
    state.last_check_time = absl::Now();
    state.has_cached = true;
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
  if (path.empty() || IsInTestEnvironment()) {
    GruPenaltyConfig cfg;
    cfg.enable_live_conversion = flag_default;
    if (!path.empty()) {
      cfg = ParseConfigFile(path, cfg);
    }
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
  if (path.empty() || IsInTestEnvironment()) {
    GruPenaltyConfig cfg;
    cfg.enable_live_conversion_prefix_freezing = flag_default;
    if (!path.empty()) {
      cfg = ParseConfigFile(path, cfg);
    }
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
  if (path.empty() || IsInTestEnvironment()) {
    GruPenaltyConfig cfg;
    cfg.enable_live_conversion_auto_commit = flag_default;
    if (!path.empty()) {
      cfg = ParseConfigFile(path, cfg);
    }
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
  if (path.empty() || IsInTestEnvironment()) {
    GruPenaltyConfig cfg;
    cfg.live_conversion_length_threshold = flag_default;
    if (!path.empty()) {
      cfg = ParseConfigFile(path, cfg);
    }
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

int32_t GruConfigManager::GetLiveConversionExcludedHiraganaCharacters(
    int32_t flag_default,
    const std::string& explicit_config_path) {
  static absl::NoDestructor<absl::Mutex> mutex;
  static absl::Time last_check_time = absl::InfinitePast();
  static int64_t last_mtime = -1;
  static std::string last_path;
  static int32_t cached_value = 0;
  static bool has_cached = false;

  absl::MutexLock lock(*mutex);
  const std::string path =
      ResolveConfigFilePath(explicit_config_path, /*allow_in_test=*/false);
  if (path.empty() || IsInTestEnvironment()) {
    GruPenaltyConfig cfg;
    cfg.live_conversion_excluded_hiragana_characters = flag_default;
    if (!path.empty()) {
      cfg = ParseConfigFile(path, cfg);
    }
    ApplyEnvOverrides(&cfg);
    return cfg.live_conversion_excluded_hiragana_characters;
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
    base.live_conversion_excluded_hiragana_characters = flag_default;
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.live_conversion_excluded_hiragana_characters;
    has_cached = true;
  }
  return cached_value;
}

bool GruConfigManager::IsConversionStatsEnabled(
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
  if (path.empty() || IsInTestEnvironment()) {
    GruPenaltyConfig cfg;
    cfg.enable_conversion_stats = flag_default;
    if (!path.empty()) {
      cfg = ParseConfigFile(path, cfg);
    }
    ApplyEnvOverrides(&cfg);
    return cfg.enable_conversion_stats;
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
    base.enable_conversion_stats = flag_default;
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.enable_conversion_stats;
    has_cached = true;
  }
  return cached_value;
}

std::string GruConfigManager::GetConversionStatsPath(
    const std::string& explicit_config_path) {
  static absl::NoDestructor<absl::Mutex> mutex;
  static absl::Time last_check_time = absl::InfinitePast();
  static int64_t last_mtime = -1;
  static std::string last_path;
  static std::string cached_value = "";
  static bool has_cached = false;

  absl::MutexLock lock(*mutex);
  const std::string path =
      ResolveConfigFilePath(explicit_config_path, /*allow_in_test=*/false);
  if (path.empty() || IsInTestEnvironment()) {
    GruPenaltyConfig cfg;
    if (!path.empty()) {
      cfg = ParseConfigFile(path, cfg);
    }
    ApplyEnvOverrides(&cfg);
    return cfg.conversion_stats_path;
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
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.conversion_stats_path;
    has_cached = true;
  }
  return cached_value;
}

bool GruConfigManager::IsConversionHistoryLogEnabled(
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
    cfg.enable_conversion_history_log = flag_default;
    ApplyEnvOverrides(&cfg);
    return cfg.enable_conversion_history_log;
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
    base.enable_conversion_history_log = flag_default;
    GruPenaltyConfig parsed = ParseConfigFile(path, base);
    ApplyEnvOverrides(&parsed);
    cached_value = parsed.enable_conversion_history_log;
    has_cached = true;
  }
  return cached_value;
}

}  // namespace config
}  // namespace mozc
