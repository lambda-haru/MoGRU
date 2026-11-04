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

#ifndef MOZC_CONFIG_CONVERSION_STATS_H_
#define MOZC_CONFIG_CONVERSION_STATS_H_

#include <cstdint>
#include <string>

#include "absl/strings/string_view.h"

namespace mozc {
namespace config {

struct LiveConversionStats {
  uint64_t commit_count = 0;
  uint64_t fallback_to_space_count = 0;
  uint64_t total_segments = 0;
  uint64_t total_characters = 0;

  double AccuracyRate() const {
    const uint64_t total = commit_count + fallback_to_space_count;
    return (total == 0) ? 0.0 : static_cast<double>(commit_count) / total;
  }
};

struct SpaceConversionStats {
  uint64_t commit_count = 0;
  uint64_t top1_commit_count = 0;
  uint64_t total_segments = 0;
  uint64_t top1_segments = 0;
  uint64_t resized_commit_count = 0;
  uint64_t total_characters = 0;

  double CommitAccuracyRate() const {
    return (commit_count == 0) ? 0.0
                               : static_cast<double>(top1_commit_count) /
                                     commit_count;
  }

  double SegmentAccuracyRate() const {
    return (total_segments == 0) ? 0.0
                                 : static_cast<double>(top1_segments) /
                                       total_segments;
  }
};

struct ConversionStats {
  LiveConversionStats live;
  SpaceConversionStats space;
  std::string last_updated;

  uint64_t TotalCommits() const {
    return live.commit_count + space.commit_count;
  }

  uint64_t TotalCharacters() const {
    return live.total_characters + space.total_characters;
  }

  double OverallAccuracyRate() const {
    const uint64_t total = TotalCommits();
    if (total == 0) {
      return 0.0;
    }
    const uint64_t top1_total = live.commit_count + space.top1_commit_count;
    return static_cast<double>(top1_total) / total;
  }
};

class ConversionStatsManager {
 public:
  // Resolves the path to conversion_stats.json.
  static std::string ResolveStatsFilePath(
      const std::string& user_specified = "",
      bool allow_in_test = false);

  // Resolves the path to conversion_history.tsv.
  static std::string ResolveHistoryLogFilePath(
      const std::string& user_specified = "",
      bool allow_in_test = false);

  // Serializes `stats` into a JSON string.
  static std::string ToJson(const ConversionStats& stats);

  // Parses a JSON string into `stats`. Returns true on success.
  static bool FromJson(absl::string_view json, ConversionStats* stats);

  // Loads stats from disk (creating file if needed).
  static ConversionStats LoadStats(const std::string& explicit_path = "");

  // Saves stats to disk atomically.
  static bool SaveStats(const ConversionStats& stats,
                        const std::string& explicit_path = "");

  // Resets stats to zero and saves to disk.
  static bool ResetStats(const std::string& explicit_path = "");

  // Recording API
  static void RecordLiveConversionCommit(
      size_t segment_count,
      size_t char_count,
      absl::string_view key = "",
      absl::string_view value = "",
      const std::string& explicit_path = "");

  static void RecordLiveConversionFallbackToSpace(
      size_t char_count,
      absl::string_view key = "",
      const std::string& explicit_path = "");

  static void RecordSpaceConversionCommit(
      size_t segment_count,
      size_t top1_segment_count,
      bool is_top1_commit,
      bool was_resized,
      size_t char_count,
      absl::string_view key = "",
      absl::string_view value = "",
      const std::string& explicit_path = "");

  // Appends a single TSV entry to conversion_history.tsv if history logging is enabled.
  static void AppendHistoryLog(
      absl::string_view type,
      bool is_top1,
      size_t segment_count,
      size_t char_count,
      absl::string_view key,
      absl::string_view value,
      const std::string& explicit_path = "");
};

}  // namespace config
}  // namespace mozc

#endif  // MOZC_CONFIG_CONVERSION_STATS_H_
