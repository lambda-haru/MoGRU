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

#include "config/conversion_stats.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/synchronization/mutex.h"
#include "base/file_util.h"
#include "base/system_util.h"
#include "config/gru_config.h"

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

std::string GetCurrentTimestampString() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_buf;
#if defined(_WIN32)
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
  return oss.str();
}

std::string GetPrimaryUserProfileDirectory() {
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
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) {
    add_unique(FileUtil::JoinPath(home, "Library/Application Support/MoGRU"));
    add_unique(FileUtil::JoinPath(home, ".config/mogru"));
    add_unique(FileUtil::JoinPath(home, ".mogru"));
  }
  add_unique(SystemUtil::GetUserProfileDirectory());
#else
  if (const char* userprofile = std::getenv("USERPROFILE")) {
    add_unique(FileUtil::JoinPath(userprofile, "AppData/LocalLow/MoGRU"));
  }
  add_unique(SystemUtil::GetUserProfileDirectory());
#endif

  if (!dirs.empty()) {
    return dirs.front();
  }
  return SystemUtil::GetUserProfileDirectory();
}

absl::Mutex& GetStatsMutex() {
  static absl::NoDestructor<absl::Mutex> mutex;
  return *mutex;
}

}  // namespace

std::string ConversionStatsManager::ResolveStatsFilePath(
    const std::string& user_specified, bool allow_in_test) {
  if (!user_specified.empty()) {
    return ExpandTilde(user_specified);
  }
  if (!allow_in_test && IsInTestEnvironment()) {
    const char* test_tmpdir = std::getenv("TEST_TMPDIR");
    if (test_tmpdir != nullptr && test_tmpdir[0] != '\0') {
      return FileUtil::JoinPath(test_tmpdir, "conversion_stats.json");
    }
    return "";
  }
  const std::string dir = GetPrimaryUserProfileDirectory();
  if (dir.empty()) {
    return "";
  }
  return FileUtil::JoinPath(dir, "conversion_stats.json");
}

std::string ConversionStatsManager::ResolveHistoryLogFilePath(
    const std::string& user_specified, bool allow_in_test) {
  if (!user_specified.empty()) {
    return ExpandTilde(user_specified);
  }
  if (!allow_in_test && IsInTestEnvironment()) {
    const char* test_tmpdir = std::getenv("TEST_TMPDIR");
    if (test_tmpdir != nullptr && test_tmpdir[0] != '\0') {
      return FileUtil::JoinPath(test_tmpdir, "conversion_history.tsv");
    }
    return "";
  }
  const std::string dir = GetPrimaryUserProfileDirectory();
  if (dir.empty()) {
    return "";
  }
  return FileUtil::JoinPath(dir, "conversion_history.tsv");
}

std::string ConversionStatsManager::ToJson(const ConversionStats& stats) {
  std::string ts = stats.last_updated;
  if (ts.empty()) {
    ts = GetCurrentTimestampString();
  }
  return absl::StrFormat(
      "{\n"
      "  \"version\": 1,\n"
      "  \"last_updated\": \"%s\",\n"
      "  \"live_conversion\": {\n"
      "    \"commit_count\": %u,\n"
      "    \"fallback_to_space_count\": %u,\n"
      "    \"total_segments\": %u,\n"
      "    \"total_characters\": %u,\n"
      "    \"accuracy_rate\": %.4f\n"
      "  },\n"
      "  \"space_conversion\": {\n"
      "    \"commit_count\": %u,\n"
      "    \"top1_commit_count\": %u,\n"
      "    \"total_segments\": %u,\n"
      "    \"top1_segments\": %u,\n"
      "    \"resized_commit_count\": %u,\n"
      "    \"total_characters\": %u,\n"
      "    \"commit_accuracy_rate\": %.4f,\n"
      "    \"segment_accuracy_rate\": %.4f\n"
      "  },\n"
      "  \"overall\": {\n"
      "    \"total_commits\": %u,\n"
      "    \"total_characters\": %u,\n"
      "    \"overall_accuracy_rate\": %.4f\n"
      "  }\n"
      "}\n",
      ts,
      stats.live.commit_count,
      stats.live.fallback_to_space_count,
      stats.live.total_segments,
      stats.live.total_characters,
      stats.live.AccuracyRate(),
      stats.space.commit_count,
      stats.space.top1_commit_count,
      stats.space.total_segments,
      stats.space.top1_segments,
      stats.space.resized_commit_count,
      stats.space.total_characters,
      stats.space.CommitAccuracyRate(),
      stats.space.SegmentAccuracyRate(),
      stats.TotalCommits(),
      stats.TotalCharacters(),
      stats.OverallAccuracyRate());
}

bool ConversionStatsManager::FromJson(absl::string_view json,
                                      ConversionStats* stats) {
  if (stats == nullptr) {
    return false;
  }
  *stats = ConversionStats();

  std::string current_section;
  for (absl::string_view line : absl::StrSplit(json, '\n')) {
    line = absl::StripAsciiWhitespace(line);
    if (line.empty()) continue;

    if (line.find("\"live_conversion\"") != absl::string_view::npos) {
      current_section = "live";
      continue;
    } else if (line.find("\"space_conversion\"") != absl::string_view::npos) {
      current_section = "space";
      continue;
    } else if (line.find("\"overall\"") != absl::string_view::npos) {
      current_section = "overall";
      continue;
    }

    const auto colon_pos = line.find(':');
    if (colon_pos == absl::string_view::npos) {
      continue;
    }

    absl::string_view key_part =
        absl::StripAsciiWhitespace(line.substr(0, colon_pos));
    absl::string_view val_part =
        absl::StripAsciiWhitespace(line.substr(colon_pos + 1));
    while (!val_part.empty() && (val_part.back() == ',' || val_part.back() == '}')) {
      val_part.remove_suffix(1);
      val_part = absl::StripAsciiWhitespace(val_part);
    }

    std::string key = std::string(absl::StripPrefix(absl::StripSuffix(key_part, "\""), "\""));
    std::string val_str = std::string(absl::StripPrefix(absl::StripSuffix(val_part, "\""), "\""));

    if (key == "last_updated") {
      stats->last_updated = val_str;
      continue;
    }

    uint64_t val_u64 = 0;
    (void)absl::SimpleAtoi(val_str, &val_u64);

    if (current_section == "live") {
      if (key == "commit_count") stats->live.commit_count = val_u64;
      else if (key == "fallback_to_space_count") stats->live.fallback_to_space_count = val_u64;
      else if (key == "total_segments") stats->live.total_segments = val_u64;
      else if (key == "total_characters") stats->live.total_characters = val_u64;
    } else if (current_section == "space") {
      if (key == "commit_count") stats->space.commit_count = val_u64;
      else if (key == "top1_commit_count") stats->space.top1_commit_count = val_u64;
      else if (key == "total_segments") stats->space.total_segments = val_u64;
      else if (key == "top1_segments") stats->space.top1_segments = val_u64;
      else if (key == "resized_commit_count") stats->space.resized_commit_count = val_u64;
      else if (key == "total_characters") stats->space.total_characters = val_u64;
    }
  }

  return true;
}

ConversionStats ConversionStatsManager::LoadStats(
    const std::string& explicit_path) {
  const std::string path = ResolveStatsFilePath(explicit_path);
  if (path.empty()) {
    return ConversionStats();
  }

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return ConversionStats();
  }

  std::ifstream ifs(path);
  if (!ifs) {
    return ConversionStats();
  }
  std::stringstream ss;
  ss << ifs.rdbuf();

  ConversionStats stats;
  FromJson(ss.str(), &stats);
  return stats;
}

bool ConversionStatsManager::SaveStats(const ConversionStats& stats,
                                       const std::string& explicit_path) {
  const std::string path = ResolveStatsFilePath(explicit_path);
  if (path.empty()) {
    return false;
  }

  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path(), ec);
  }

  const std::string tmp_path = path + ".tmp";
  {
    std::ofstream ofs(tmp_path, std::ios::trunc);
    if (!ofs) {
      return false;
    }
    ofs << ToJson(stats);
    if (!ofs.good()) {
      return false;
    }
  }

  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path, ec);
    return false;
  }
  return true;
}

bool ConversionStatsManager::ResetStats(const std::string& explicit_path) {
  absl::MutexLock lock(GetStatsMutex());
  ConversionStats empty_stats;
  empty_stats.last_updated = GetCurrentTimestampString();
  return SaveStats(empty_stats, explicit_path);
}

void ConversionStatsManager::RecordLiveConversionCommit(
    size_t segment_count,
    size_t char_count,
    absl::string_view key,
    absl::string_view value,
    const std::string& explicit_path) {
  absl::MutexLock lock(GetStatsMutex());
  ConversionStats stats = LoadStats(explicit_path);
  stats.live.commit_count += 1;
  stats.live.total_segments += segment_count;
  stats.live.total_characters += char_count;
  stats.last_updated = GetCurrentTimestampString();
  SaveStats(stats, explicit_path);

  AppendHistoryLog("LIVE", true, segment_count, char_count, key, value,
                   explicit_path);
}

void ConversionStatsManager::RecordLiveConversionFallbackToSpace(
    size_t char_count,
    absl::string_view key,
    const std::string& explicit_path) {
  absl::MutexLock lock(GetStatsMutex());
  ConversionStats stats = LoadStats(explicit_path);
  stats.live.fallback_to_space_count += 1;
  stats.last_updated = GetCurrentTimestampString();
  SaveStats(stats, explicit_path);

  AppendHistoryLog("LIVE_FALLBACK", false, 0, char_count, key, "",
                   explicit_path);
}

void ConversionStatsManager::RecordSpaceConversionCommit(
    size_t segment_count,
    size_t top1_segment_count,
    bool is_top1_commit,
    bool was_resized,
    size_t char_count,
    absl::string_view key,
    absl::string_view value,
    const std::string& explicit_path) {
  absl::MutexLock lock(GetStatsMutex());
  ConversionStats stats = LoadStats(explicit_path);
  stats.space.commit_count += 1;
  if (is_top1_commit) {
    stats.space.top1_commit_count += 1;
  }
  stats.space.total_segments += segment_count;
  stats.space.top1_segments += top1_segment_count;
  if (was_resized) {
    stats.space.resized_commit_count += 1;
  }
  stats.space.total_characters += char_count;
  stats.last_updated = GetCurrentTimestampString();
  SaveStats(stats, explicit_path);

  AppendHistoryLog("SPACE", is_top1_commit, segment_count, char_count, key,
                   value, explicit_path);
}

void ConversionStatsManager::AppendHistoryLog(
    absl::string_view type,
    bool is_top1,
    size_t segment_count,
    size_t char_count,
    absl::string_view key,
    absl::string_view value,
    const std::string& explicit_path) {
  const std::string history_path = ResolveHistoryLogFilePath(
      explicit_path.empty() ? "" : explicit_path + ".history.tsv");
  if (history_path.empty()) {
    return;
  }

  std::error_code ec;
  const std::filesystem::path p(history_path);
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path(), ec);
  }

  const bool file_exists = std::filesystem::exists(history_path, ec);
  std::ofstream ofs(history_path, std::ios::app);
  if (!ofs) {
    return;
  }

  if (!file_exists) {
    ofs << "timestamp\ttype\ttop1\tsegments\tcharacters\tkey\tvalue\n";
  }

  ofs << GetCurrentTimestampString() << "\t"
      << type << "\t"
      << (is_top1 ? "1" : "0") << "\t"
      << segment_count << "\t"
      << char_count << "\t"
      << key << "\t"
      << value << "\n";
}

}  // namespace config
}  // namespace mozc
