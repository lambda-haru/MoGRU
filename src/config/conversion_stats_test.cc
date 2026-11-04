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

#include <filesystem>
#include <string>

#include "base/file_util.h"
#include "testing/gmock.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

namespace mozc {
namespace config {
namespace {

class ConversionStatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* tmp = std::getenv("TEST_TMPDIR");
    if (tmp != nullptr && tmp[0] != '\0') {
      test_dir_ = tmp;
    } else {
      test_dir_ = testing::GetSourcePath({"config", "test_tmp"});
    }
    stats_file_ = FileUtil::JoinPath(test_dir_, "test_conversion_stats.json");
    std::error_code ec;
    std::filesystem::remove(stats_file_, ec);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove(stats_file_, ec);
  }

  std::string test_dir_;
  std::string stats_file_;
};

TEST_F(ConversionStatsTest, AccuracyCalculations) {
  LiveConversionStats live;
  EXPECT_DOUBLE_EQ(live.AccuracyRate(), 0.0);

  live.commit_count = 8;
  live.fallback_to_space_count = 2;
  EXPECT_DOUBLE_EQ(live.AccuracyRate(), 0.8);

  SpaceConversionStats space;
  EXPECT_DOUBLE_EQ(space.CommitAccuracyRate(), 0.0);
  EXPECT_DOUBLE_EQ(space.SegmentAccuracyRate(), 0.0);

  space.commit_count = 10;
  space.top1_commit_count = 7;
  space.total_segments = 25;
  space.top1_segments = 20;
  EXPECT_DOUBLE_EQ(space.CommitAccuracyRate(), 0.7);
  EXPECT_DOUBLE_EQ(space.SegmentAccuracyRate(), 0.8);

  ConversionStats stats;
  stats.live = live;
  stats.space = space;
  EXPECT_EQ(stats.TotalCommits(), 18);
  // overall = (live.commit_count + space.top1_commit_count) / TotalCommits
  // = (8 + 7) / 18 = 15 / 18 = 0.8333333333333334
  EXPECT_NEAR(stats.OverallAccuracyRate(), 15.0 / 18.0, 1e-6);
}

TEST_F(ConversionStatsTest, JsonRoundTrip) {
  ConversionStats original;
  original.last_updated = "2026-11-04 11:00:00";
  original.live.commit_count = 42;
  original.live.fallback_to_space_count = 8;
  original.live.total_segments = 84;
  original.live.total_characters = 168;

  original.space.commit_count = 20;
  original.space.top1_commit_count = 15;
  original.space.total_segments = 50;
  original.space.top1_segments = 45;
  original.space.resized_commit_count = 2;
  original.space.total_characters = 100;

  const std::string json = ConversionStatsManager::ToJson(original);
  EXPECT_FALSE(json.empty());

  ConversionStats restored;
  EXPECT_TRUE(ConversionStatsManager::FromJson(json, &restored));
  EXPECT_EQ(restored.last_updated, original.last_updated);
  EXPECT_EQ(restored.live.commit_count, 42);
  EXPECT_EQ(restored.live.fallback_to_space_count, 8);
  EXPECT_EQ(restored.live.total_segments, 84);
  EXPECT_EQ(restored.live.total_characters, 168);

  EXPECT_EQ(restored.space.commit_count, 20);
  EXPECT_EQ(restored.space.top1_commit_count, 15);
  EXPECT_EQ(restored.space.total_segments, 50);
  EXPECT_EQ(restored.space.top1_segments, 45);
  EXPECT_EQ(restored.space.resized_commit_count, 2);
  EXPECT_EQ(restored.space.total_characters, 100);
}

TEST_F(ConversionStatsTest, SaveAndLoadAndReset) {
  ConversionStats initial = ConversionStatsManager::LoadStats(stats_file_);
  EXPECT_EQ(initial.live.commit_count, 0);
  EXPECT_EQ(initial.space.commit_count, 0);

  ConversionStatsManager::RecordLiveConversionCommit(2, 5, "きょう", "今日", stats_file_);
  ConversionStatsManager::RecordLiveConversionFallbackToSpace(4, "あした", stats_file_);
  ConversionStatsManager::RecordSpaceConversionCommit(3, 3, true, false, 8, "とうきょう", "東京", stats_file_);
  ConversionStatsManager::RecordSpaceConversionCommit(2, 1, false, true, 6, "にほん", "日本", stats_file_);

  ConversionStats loaded = ConversionStatsManager::LoadStats(stats_file_);
  EXPECT_EQ(loaded.live.commit_count, 1);
  EXPECT_EQ(loaded.live.fallback_to_space_count, 1);
  EXPECT_EQ(loaded.live.total_segments, 2);
  EXPECT_EQ(loaded.live.total_characters, 5);

  EXPECT_EQ(loaded.space.commit_count, 2);
  EXPECT_EQ(loaded.space.top1_commit_count, 1);
  EXPECT_EQ(loaded.space.total_segments, 5);
  EXPECT_EQ(loaded.space.top1_segments, 4);
  EXPECT_EQ(loaded.space.resized_commit_count, 1);
  EXPECT_EQ(loaded.space.total_characters, 14);

  EXPECT_TRUE(ConversionStatsManager::ResetStats(stats_file_));
  ConversionStats reset_loaded = ConversionStatsManager::LoadStats(stats_file_);
  EXPECT_EQ(reset_loaded.live.commit_count, 0);
  EXPECT_EQ(reset_loaded.space.commit_count, 0);
}

}  // namespace
}  // namespace config
}  // namespace mozc
