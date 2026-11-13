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

#include "rewriter/english_fallback_rewriter.h"

#include <memory>
#include <string>

#include "absl/strings/string_view.h"
#include "composer/composer.h"
#include "composer/table.h"
#include "config/gru_config.h"
#include "converter/segments.h"
#include "data_manager/testing/mock_data_manager.h"
#include "dictionary/english_dictionary.h"
#include "dictionary/pos_matcher.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

namespace mozc {
namespace {

using config::GruConfigManager;

#if defined(_WIN32)
inline int SetEnv(const char* name, const char* value) {
  return _putenv_s(name, value);
}
inline int UnsetEnv(const char* name) {
  return _putenv_s(name, "");
}
#else
inline int SetEnv(const char* name, const char* value) {
  return ::setenv(name, value, 1);
}
inline int UnsetEnv(const char* name) {
  return ::unsetenv(name);
}
#endif

void InsertASCIISequence(absl::string_view text,
                         composer::Composer* composer) {
  for (char c : text) {
    commands::KeyEvent key;
    key.set_key_code(c);
    composer->InsertCharacterKeyEvent(key);
  }
}

class EnglishFallbackRewriterTest : public testing::TestWithTempUserProfile {
 protected:
  void SetUp() override {
    data_manager_ = std::make_unique<testing::MockDataManager>();
    pos_matcher_ = std::make_unique<dictionary::PosMatcher>(
        data_manager_->GetPosMatcherData());

    // Reset GruConfig to defaults for predictable tests.
    GruPenaltyConfig config;
    config.enable_english_fallback = true;
    config.english_fallback_cost_threshold = 15000.0;
    config.english_fallback_min_word_length = 3;
    config.enable_live_conversion_english_fallback = true;
    GruConfigManager::SaveConfig(config);
  }

  composer::Composer CreateComposer(absl::string_view raw_input) {
    auto table = std::make_shared<composer::Table>();
    commands::Request client_request;
    config::Config default_config;
    table->InitializeWithRequestAndConfig(client_request, default_config);

    composer::Composer composer(table, client_request, default_config);
    InsertASCIISequence(raw_input, &composer);
    return composer;
  }

  std::unique_ptr<testing::MockDataManager> data_manager_;
  std::unique_ptr<dictionary::PosMatcher> pos_matcher_;
};

TEST_F(EnglishFallbackRewriterTest, HighCostFallbackMergesSegments) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("merge");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  // Simulating "mergesuru" -> preedit "めｒげする"
  composer::Composer composer = CreateComposer("mergesuru");
  ASSERT_EQ(composer.GetStringForPreedit(), "めｒげする");

  Segments segments;
  // Segment 0: "め" (normal cost)
  Segment* seg0 = segments.add_segment();
  seg0->set_key("め");
  auto* c0 = seg0->add_candidate();
  c0->key = "め";
  c0->value = "目";
  c0->cost = 5000;

  // Segment 1: "ｒ" (high cost single alphabet!)
  Segment* seg1 = segments.add_segment();
  seg1->set_key("ｒ");
  auto* c1 = seg1->add_candidate();
  c1->key = "ｒ";
  c1->value = "ｒ";
  c1->cost = 35609;

  // Segment 2: "げ" (normal cost)
  Segment* seg2 = segments.add_segment();
  seg2->set_key("げ");
  auto* c2 = seg2->add_candidate();
  c2->key = "げ";
  c2->value = "ゲ";
  c2->cost = 6000;

  // Segment 3: "する" (normal cost)
  Segment* seg3 = segments.add_segment();
  seg3->set_key("する");
  auto* c3 = seg3->add_candidate();
  c3->key = "する";
  c3->value = "する";
  c3->cost = 4000;

  ASSERT_EQ(segments.conversion_segments_size(), 4);

  const ConversionRequest request = ConversionRequestBuilder()
                                        .SetComposer(composer)
                                        .SetRequestType(ConversionRequest::CONVERSION)
                                        .Build();

  EXPECT_TRUE(rewriter.Rewrite(request, &segments));

  // Should merge segments 0, 1, 2 into 1 segment ("めｒげ" -> "merge")
  // and preserve segment 3 ("する").
  ASSERT_EQ(segments.conversion_segments_size(), 2);

  const Segment& merged_seg = segments.conversion_segment(0);
  EXPECT_EQ(merged_seg.key(), "めｒげ");
  ASSERT_EQ(merged_seg.candidates_size(), 1);
  EXPECT_EQ(merged_seg.candidate(0).value, "merge");
  EXPECT_EQ(merged_seg.candidate(0).cost, 6000);
  EXPECT_EQ(merged_seg.candidate(0).lid, pos_matcher_->GetUnknownId());
  EXPECT_EQ(merged_seg.candidate(0).rid, pos_matcher_->GetUnknownId());

  const Segment& tail_seg = segments.conversion_segment(1);
  EXPECT_EQ(tail_seg.key(), "する");
  ASSERT_GE(tail_seg.candidates_size(), 1);
  EXPECT_EQ(tail_seg.candidate(0).value, "する");
}

TEST_F(EnglishFallbackRewriterTest, NormalConversionNoFallback) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("test");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  composer::Composer composer = CreateComposer("test");
  Segments segments;
  Segment* seg = segments.add_segment();
  seg->set_key("てすと");
  auto* c = seg->add_candidate();
  c->key = "てすと";
  c->value = "テスト";
  c->cost = 5000;  // Normal cost (< 20000)

  const ConversionRequest request = ConversionRequestBuilder()
                                        .SetComposer(composer)
                                        .SetRequestType(ConversionRequest::CONVERSION)
                                        .Build();

  // No high-cost segment -> Rewrite should return false
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segments_size(), 1);
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "テスト");
}

TEST_F(EnglishFallbackRewriterTest, NoMatchingEnglishWord) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("merge");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  composer::Composer composer = CreateComposer("xyzsuru");
  Segments segments;
  Segment* seg0 = segments.add_segment();
  seg0->set_key("xyz");
  auto* c0 = seg0->add_candidate();
  c0->key = "xyz";
  c0->value = "xyz";
  c0->cost = 35000;  // High cost, but not in dictionary

  Segment* seg1 = segments.add_segment();
  seg1->set_key("する");
  auto* c1 = seg1->add_candidate();
  c1->key = "する";
  c1->value = "する";
  c1->cost = 4000;

  const ConversionRequest request = ConversionRequestBuilder()
                                        .SetComposer(composer)
                                        .SetRequestType(ConversionRequest::CONVERSION)
                                        .Build();

  // No dictionary match -> should not modify
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segments_size(), 2);
}

TEST_F(EnglishFallbackRewriterTest, MinWordLengthFilter) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("in");  // 2 letters

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  composer::Composer composer = CreateComposer("insuru");
  Segments segments;
  Segment* seg0 = segments.add_segment();
  seg0->set_key("in");
  auto* c0 = seg0->add_candidate();
  c0->cost = 35000;

  Segment* seg1 = segments.add_segment();
  seg1->set_key("する");
  auto* c1 = seg1->add_candidate();
  c1->cost = 4000;

  const ConversionRequest request = ConversionRequestBuilder()
                                        .SetComposer(composer)
                                        .SetRequestType(ConversionRequest::CONVERSION)
                                        .Build();

  // Default min_word_length is 3, "in" is length 2 -> should not trigger
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));
}

TEST_F(EnglishFallbackRewriterTest, DisabledConfig) {
  GruPenaltyConfig config;
  config.enable_english_fallback = false;  // Disabled
  GruConfigManager::SaveConfig(config);

  dictionary::EnglishDictionary dict;
  dict.AddWord("merge");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  composer::Composer composer = CreateComposer("mergesuru");
  Segments segments;
  Segment* seg0 = segments.add_segment();
  seg0->set_key("め");
  seg0->add_candidate()->cost = 5000;
  Segment* seg1 = segments.add_segment();
  seg1->set_key("ｒ");
  seg1->add_candidate()->cost = 35000;
  Segment* seg2 = segments.add_segment();
  seg2->set_key("げ");
  seg2->add_candidate()->cost = 6000;

  const ConversionRequest request = ConversionRequestBuilder()
                                        .SetComposer(composer)
                                        .SetRequestType(ConversionRequest::CONVERSION)
                                        .Build();

  EXPECT_FALSE(rewriter.Rewrite(request, &segments));
}

TEST_F(EnglishFallbackRewriterTest, LiveConversionToggle) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("merge");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  // 1. Live conversion fallback disabled
  GruPenaltyConfig config;
  config.enable_english_fallback = true;
  config.enable_live_conversion_english_fallback = false;
  GruConfigManager::SaveConfig(config);

  composer::Composer composer = CreateComposer("mergesuru");
  Segments segments;
  Segment* seg0 = segments.add_segment();
  seg0->set_key("め");
  seg0->add_candidate()->cost = 5000;
  Segment* seg1 = segments.add_segment();
  seg1->set_key("ｒ");
  seg1->add_candidate()->cost = 35000;
  Segment* seg2 = segments.add_segment();
  seg2->set_key("げ");
  seg2->add_candidate()->cost = 6000;
  Segment* seg3 = segments.add_segment();
  seg3->set_key("する");
  seg3->add_candidate()->cost = 4000;

  ConversionRequest::Options live_options;
  live_options.composer_key_selection = ConversionRequest::PREDICTION_KEY;
  const ConversionRequest live_request =
      ConversionRequestBuilder()
          .SetComposer(composer)
          .SetOptions(std::move(live_options))
          .SetRequestType(ConversionRequest::CONVERSION)
          .Build();

  EXPECT_FALSE(rewriter.Rewrite(live_request, &segments));

  // 2. Live conversion fallback enabled
  config.enable_live_conversion_english_fallback = true;
  GruConfigManager::SaveConfig(config);

  EXPECT_TRUE(rewriter.Rewrite(live_request, &segments));
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "merge");
}

TEST_F(EnglishFallbackRewriterTest, TrailingParticleSplit) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("repository");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  composer::Composer composer = CreateComposer("repositorywotsukuru");
  ASSERT_EQ(composer.GetStringForPreedit(), "れぽしとｒｙをつくる");

  Segments segments;
  // Segment 0: "れぽ"
  Segment* seg0 = segments.add_segment();
  seg0->set_key("れぽ");
  seg0->add_candidate()->cost = 8485;

  // Segment 1: "しと"
  Segment* seg1 = segments.add_segment();
  seg1->set_key("しと");
  seg1->add_candidate()->cost = 6585;

  // Segment 2: "ｒｙを" (High cost! covers tail of word "ｒｙ" + particle "を")
  Segment* seg2 = segments.add_segment();
  seg2->set_key("ｒｙを");
  auto* c2 = seg2->add_candidate();
  c2->key = "ｒｙを";
  c2->value = "ryを";
  c2->cost = 19994;
  c2->wcost = 17655;

  // Segment 3: "つくる"
  Segment* seg3 = segments.add_segment();
  seg3->set_key("つくる");
  auto* c3 = seg3->add_candidate();
  c3->key = "つくる";
  c3->value = "つくる";
  c3->cost = 3261;

  ASSERT_EQ(segments.conversion_segments_size(), 4);

  const ConversionRequest request = ConversionRequestBuilder()
                                        .SetComposer(composer)
                                        .SetRequestType(ConversionRequest::CONVERSION)
                                        .Build();

  EXPECT_TRUE(rewriter.Rewrite(request, &segments));

  // Should split into:
  // Segment 0: "れぽしとｒｙ" -> "repository"
  // Segment 1: "を" -> "を"
  // Segment 2: "つくる" -> "つくる"
  ASSERT_EQ(segments.conversion_segments_size(), 3);

  const Segment& rep_seg = segments.conversion_segment(0);
  EXPECT_EQ(rep_seg.key(), "れぽしとｒｙ");
  EXPECT_EQ(rep_seg.candidate(0).value, "repository");
  EXPECT_EQ(rep_seg.candidate(0).cost, 6000);

  const Segment& wo_seg = segments.conversion_segment(1);
  EXPECT_EQ(wo_seg.key(), "を");
  EXPECT_EQ(wo_seg.candidate(0).value, "を");

  const Segment& tsukuru_seg = segments.conversion_segment(2);
  EXPECT_EQ(tsukuru_seg.key(), "つくる");
  EXPECT_EQ(tsukuru_seg.candidate(0).value, "つくる");
}

TEST_F(EnglishFallbackRewriterTest, EmbeddedDictionaryFallbackWithoutExternalFile) {
  // Point to a non-existent file to ensure external file loading fails,
  // verifying that the embedded dictionary takes effect.
  SetEnv("MOZC_ENGLISH_DICTIONARY_PATH", "/non/existent/google-10000-english.txt");
  EnglishFallbackRewriter rewriter(*pos_matcher_);
  UnsetEnv("MOZC_ENGLISH_DICTIONARY_PATH");

  composer::Composer composer = CreateComposer("mergesuru");
  Segments segments;

  Segment* seg0 = segments.add_segment();
  seg0->set_key("めｒげ");
  auto* c0 = seg0->add_candidate();
  c0->key = "めｒげ";
  c0->value = "めrげ";
  c0->cost = 20569;
  c0->wcost = 17793;

  Segment* seg1 = segments.add_segment();
  seg1->set_key("する");
  auto* c1 = seg1->add_candidate();
  c1->key = "する";
  c1->value = "する";
  c1->cost = 4229;

  const ConversionRequest request = ConversionRequestBuilder()
                                        .SetComposer(composer)
                                        .SetRequestType(ConversionRequest::CONVERSION)
                                        .Build();

  EXPECT_TRUE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segments_size(), 2);
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "merge");
  EXPECT_EQ(segments.conversion_segment(1).candidate(0).value, "する");
}

TEST_F(EnglishFallbackRewriterTest, CapitalizedWordPreservesCaseAndAddsLower) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("merge");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  // "Mergesuru": Shift+M -> "M", then non-shift "ergesuru" reverts to Romaji -> "Mえｒげする"
  composer::Composer composer = CreateComposer("Mergesuru");
  ASSERT_EQ(composer.GetStringForPreedit(), "Mえｒげする");

  Segments segments;
  Segment* seg0 = segments.add_segment();
  seg0->set_key("Mえｒげ");
  auto* c0 = seg0->add_candidate();
  c0->key = "Mえｒげ";
  c0->value = "Mえrげ";
  c0->cost = 25000;

  Segment* seg1 = segments.add_segment();
  seg1->set_key("する");
  auto* c1 = seg1->add_candidate();
  c1->key = "する";
  c1->value = "する";
  c1->cost = 4000;

  const ConversionRequest request = ConversionRequestBuilder()
                                        .SetComposer(composer)
                                        .SetRequestType(ConversionRequest::CONVERSION)
                                        .Build();

  EXPECT_TRUE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segments_size(), 2);

  const Segment& merged_seg = segments.conversion_segment(0);
  EXPECT_EQ(merged_seg.key(), "Mえｒげ");
  ASSERT_GE(merged_seg.candidates_size(), 2);
  // Candidate 0: user raw case ("Merge")
  EXPECT_EQ(merged_seg.candidate(0).value, "Merge");
  EXPECT_EQ(merged_seg.candidate(0).cost, 6000);
  // Candidate 1: lower case ("merge")
  EXPECT_EQ(merged_seg.candidate(1).value, "merge");
  EXPECT_EQ(merged_seg.candidate(1).cost, 6100);

  const Segment& tail_seg = segments.conversion_segment(1);
  EXPECT_EQ(tail_seg.key(), "する");
  EXPECT_EQ(tail_seg.candidate(0).value, "する");
}

TEST_F(EnglishFallbackRewriterTest, CheckResizeSegmentsRequestMergesAndSplits) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("repository");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  composer::Composer composer = CreateComposer("repositorywotsukuru");
  Segments segments;

  Segment* seg0 = segments.add_segment();
  seg0->set_key("れぽ");
  seg0->add_candidate()->cost = 8485;

  Segment* seg1 = segments.add_segment();
  seg1->set_key("しと");
  seg1->add_candidate()->cost = 6585;

  Segment* seg2 = segments.add_segment();
  seg2->set_key("ｒｙを");
  seg2->add_candidate()->cost = 19994;

  Segment* seg3 = segments.add_segment();
  seg3->set_key("つくる");
  seg3->add_candidate()->cost = 3261;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetComposer(composer)
          .SetRequestType(ConversionRequest::CONVERSION)
          .Build();

  const auto resize_req =
      rewriter.CheckResizeSegmentsRequest(request, segments);
  ASSERT_TRUE(resize_req.has_value());
  EXPECT_EQ(resize_req->segment_index, 0);
  EXPECT_EQ(resize_req->segment_sizes[0], 6);  // "れぽしとｒｙ" = 6 chars

  // Once resized is true, CheckResizeSegmentsRequest must return nullopt.
  segments.set_resized(true);
  EXPECT_FALSE(
      rewriter.CheckResizeSegmentsRequest(request, segments).has_value());
}

TEST_F(EnglishFallbackRewriterTest, UserResizedShrinkDoesNotRemerge) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("merge");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  composer::Composer composer = CreateComposer("mergesuru");
  Segments segments;
  segments.set_resized(true);

  // User shrunk segment 0 from "めｒげ" (3 chars) to "めｒ" (2 chars, FIXED_BOUNDARY)
  Segment* seg0 = segments.add_segment();
  seg0->set_key("めｒ");
  seg0->set_segment_type(Segment::FIXED_BOUNDARY);
  auto* c0 = seg0->add_candidate();
  c0->key = "めｒ";
  c0->value = "めr";
  c0->cost = 20000;

  Segment* seg1 = segments.add_segment();
  seg1->set_key("げ");
  seg1->set_segment_type(Segment::FREE);
  auto* c1 = seg1->add_candidate();
  c1->key = "げ";
  c1->value = "下";
  c1->cost = 5000;

  Segment* seg2 = segments.add_segment();
  seg2->set_key("する");
  seg2->set_segment_type(Segment::FREE);
  auto* c2 = seg2->add_candidate();
  c2->key = "する";
  c2->value = "する";
  c2->cost = 4000;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetComposer(composer)
          .SetRequestType(ConversionRequest::CONVERSION)
          .Build();

  // Must NOT re-merge "めｒ" and "げ" back into "merge".
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segments_size(), 3);
  EXPECT_EQ(segments.conversion_segment(0).key(), "めｒ");
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "めr");
  EXPECT_EQ(segments.conversion_segment(1).key(), "げ");
  EXPECT_EQ(segments.conversion_segment(2).key(), "する");
}

TEST_F(EnglishFallbackRewriterTest, UserResizedExpandDoesNotSplit) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("merge");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  composer::Composer composer = CreateComposer("mergesuru");
  Segments segments;
  segments.set_resized(true);

  // User expanded segment 0 from "めｒげ" (3 chars) to "めｒげす" (4 chars, FIXED_BOUNDARY)
  Segment* seg0 = segments.add_segment();
  seg0->set_key("めｒげす");
  seg0->set_segment_type(Segment::FIXED_BOUNDARY);
  auto* c0 = seg0->add_candidate();
  c0->key = "めｒげす";
  c0->value = "目rゲス";
  c0->cost = 22000;

  Segment* seg1 = segments.add_segment();
  seg1->set_key("る");
  seg1->set_segment_type(Segment::FREE);
  auto* c1 = seg1->add_candidate();
  c1->key = "る";
  c1->value = "る";
  c1->cost = 4000;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetComposer(composer)
          .SetRequestType(ConversionRequest::CONVERSION)
          .Build();

  // Must NOT split "めｒげす" back into "merge" + "す".
  EXPECT_FALSE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segments_size(), 2);
  EXPECT_EQ(segments.conversion_segment(0).key(), "めｒげす");
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "目rゲス");
  EXPECT_EQ(segments.conversion_segment(1).key(), "る");
}

TEST_F(EnglishFallbackRewriterTest,
       ExactFixedBoundaryStillReceivesEnglishCandidate) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("merge");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  composer::Composer composer = CreateComposer("mergesuru");
  Segments segments;
  segments.set_resized(true);

  // Segment 0 is FIXED_BOUNDARY with exact length of "merge" ("めｒげ", 3 chars).
  Segment* seg0 = segments.add_segment();
  seg0->set_key("めｒげ");
  seg0->set_segment_type(Segment::FIXED_BOUNDARY);
  auto* c0 = seg0->add_candidate();
  c0->key = "めｒげ";
  c0->value = "めrげ";
  c0->cost = 20569;

  Segment* seg1 = segments.add_segment();
  seg1->set_key("する");
  seg1->set_segment_type(Segment::FREE);
  auto* c1 = seg1->add_candidate();
  c1->key = "する";
  c1->value = "する";
  c1->cost = 4000;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetComposer(composer)
          .SetRequestType(ConversionRequest::CONVERSION)
          .Build();

  EXPECT_TRUE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segments_size(), 2);
  EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "merge");
  EXPECT_EQ(segments.conversion_segment(1).candidate(0).value, "する");
}

TEST_F(EnglishFallbackRewriterTest,
       PrecedingFixedBoundarySplittingWordSuppressesSuffixMatch) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("branch");
  dict.AddWord("ranch");

  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  // "watashihabranchwokiru" -> "わたしはｂらんｃｈをきる"
  composer::Composer composer = CreateComposer("watashihabranchwokiru");
  ASSERT_EQ(composer.GetStringForPreedit(), "わたしはｂらんｃｈをきる");

  Segments segments;
  segments.set_resized(true);

  // User expanded segment 0 to "わたしはｂ" (5 chars, FIXED_BOUNDARY), consuming 'b' of "branch".
  Segment* seg0 = segments.add_segment();
  seg0->set_key("わたしはｂ");
  seg0->set_segment_type(Segment::FIXED_BOUNDARY);
  auto* c0 = seg0->add_candidate();
  c0->key = "わたしはｂ";
  c0->value = "私はb";
  c0->cost = 20000;

  // Remaining "らんｃｈ" (FREE) should not match "ranch" because it is a suffix fragment of "branch".
  Segment* seg1 = segments.add_segment();
  seg1->set_key("らんｃｈ");
  seg1->set_segment_type(Segment::FREE);
  auto* c1 = seg1->add_candidate();
  c1->key = "らんｃｈ";
  c1->value = "らんch";
  c1->cost = 25000;

  Segment* seg2 = segments.add_segment();
  seg2->set_key("をきる");
  seg2->set_segment_type(Segment::FREE);
  auto* c2 = seg2->add_candidate();
  c2->key = "をきる";
  c2->value = "を切る";
  c2->cost = 4000;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetComposer(composer)
          .SetRequestType(ConversionRequest::CONVERSION)
          .Build();

  EXPECT_FALSE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segments_size(), 3);
  EXPECT_EQ(segments.conversion_segment(1).candidate(0).value, "らんch");
}

TEST_F(EnglishFallbackRewriterTest, UserConfigEnglishWordsFallback) {
  const std::string config_path =
      config::GruConfigManager::ResolveConfigFilePath("", /*allow_in_test=*/true);
  GruPenaltyConfig cfg;
  cfg.enable_english_fallback = true;
  cfg.user_english_words = {"Kubernetes", "terraform"};
  config::GruConfigManager::SaveConfig(cfg, config_path);

  // Injected dictionary has NO words at all.
  dictionary::EnglishDictionary empty_dict;
  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(empty_dict));

  composer::Composer composer = CreateComposer("kubernetesga");
  // "kubernetesga" in preedit: "くべｒねてｓが"
  Segments segments;
  Segment* seg0 = segments.add_segment();
  seg0->set_key("くべｒねてｓ");
  auto* c0 = seg0->add_candidate();
  c0->key = "くべｒねてｓ";
  c0->value = "くべrねてs";
  c0->cost = 25000;

  Segment* seg1 = segments.add_segment();
  seg1->set_key("が");
  auto* c1 = seg1->add_candidate();
  c1->key = "が";
  c1->value = "が";
  c1->cost = 2000;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetComposer(composer)
          .SetRequestType(ConversionRequest::CONVERSION)
          .Build();

  EXPECT_TRUE(rewriter.Rewrite(request, &segments));
  ASSERT_EQ(segments.conversion_segments_size(), 2);

  const Segment& merged_seg = segments.conversion_segment(0);
  EXPECT_EQ(merged_seg.key(), "くべｒねてｓ");
  ASSERT_GE(merged_seg.candidates_size(), 2);
  // User typed lowercase -> raw is "kubernetes"
  EXPECT_EQ(merged_seg.candidate(0).value, "kubernetes");
  // Original casing from user_english_words is "Kubernetes"
  EXPECT_EQ(merged_seg.candidate(1).value, "Kubernetes");
}

TEST_F(EnglishFallbackRewriterTest, UserEnglishWordsReload) {
  const std::string config_path =
      config::GruConfigManager::ResolveConfigFilePath("", /*allow_in_test=*/true);
  GruPenaltyConfig cfg;
  cfg.enable_english_fallback = true;
  cfg.user_english_words = {"rebase"};
  config::GruConfigManager::SaveConfig(cfg, config_path);

  dictionary::EnglishDictionary empty_dict;
  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(empty_dict));

  // 1. "rebase" should be detected
  {
    composer::Composer composer = CreateComposer("rebase");
    Segments segments;
    Segment* seg = segments.add_segment();
    seg->set_key("ればせ");
    auto* c = seg->add_candidate();
    c->key = "ればせ";
    c->value = "ればせ";
    c->cost = 25000;

    const ConversionRequest request =
        ConversionRequestBuilder()
            .SetComposer(composer)
            .SetRequestType(ConversionRequest::CONVERSION)
            .Build();

    EXPECT_TRUE(rewriter.Rewrite(request, &segments));
    EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "rebase");
  }

  // 2. "cherrypick" is NOT yet registered
  {
    composer::Composer composer = CreateComposer("cherrypick");
    Segments segments;
    Segment* seg = segments.add_segment();
    seg->set_key("ちぇｒｒｙぴｃｋ");
    auto* c = seg->add_candidate();
    c->key = "ちぇｒｒｙぴｃｋ";
    c->value = "ちぇrryぴck";
    c->cost = 25000;

    const ConversionRequest request =
        ConversionRequestBuilder()
            .SetComposer(composer)
            .SetRequestType(ConversionRequest::CONVERSION)
            .Build();

    EXPECT_FALSE(rewriter.Rewrite(request, &segments));
  }

  // 3. Update config with "cherrypick" and reload
  cfg.user_english_words.push_back("cherrypick");
  config::GruConfigManager::SaveConfig(cfg, config_path);
  EXPECT_TRUE(rewriter.Reload());

  // 4. Now "cherrypick" should be detected
  {
    composer::Composer composer = CreateComposer("cherrypick");
    Segments segments;
    Segment* seg = segments.add_segment();
    seg->set_key("ちぇｒｒｙぴｃｋ");
    auto* c = seg->add_candidate();
    c->key = "ちぇｒｒｙぴｃｋ";
    c->value = "ちぇrryぴck";
    c->cost = 25000;

    const ConversionRequest request =
        ConversionRequestBuilder()
            .SetComposer(composer)
            .SetRequestType(ConversionRequest::CONVERSION)
            .Build();

    EXPECT_TRUE(rewriter.Rewrite(request, &segments));
    EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "cherrypick");
  }
}

TEST_F(EnglishFallbackRewriterTest, CrossBoundaryDoesNotCorruptMidChunkSplit) {
  dictionary::EnglishDictionary dict;
  dict.AddWord("test");
  dict.AddWord("git");
  dict.AddWord("class");
  EnglishFallbackRewriter rewriter(*pos_matcher_, std::move(dict));

  for (const char* input : {"testwo", "gitwo", "classwo", "githa"}) {
    composer::Composer composer = CreateComposer(input);
    Segments segments;
    Segment* seg = segments.add_segment();
    const std::string preedit = composer.GetQueryForConversion();
    seg->set_key(preedit);
    auto* c = seg->add_candidate();
    c->key = preedit;
    c->value = preedit;
    c->cost = 25000;

    const ConversionRequest request =
        ConversionRequestBuilder()
            .SetComposer(composer)
            .SetRequestType(ConversionRequest::CONVERSION)
            .Build();

    EXPECT_FALSE(rewriter.CheckResizeSegmentsRequest(request, segments).has_value())
        << input;
    EXPECT_FALSE(rewriter.Rewrite(request, &segments)) << input;
  }
}

TEST_F(EnglishFallbackRewriterTest, FindCrossBoundarySplitOffsetsAllPatterns) {
  const std::string config_path =
      config::GruConfigManager::ResolveConfigFilePath("", /*allow_in_test=*/true);
  GruPenaltyConfig cfg;
  cfg.enable_english_fallback = true;
  cfg.user_english_words = {"docker", "git", "kubernetes"};
  config::GruConfigManager::SaveConfig(cfg, config_path);

  struct Case {
    const char* raw_input;
    size_t expected_offset;
    const char* expected_english;
    const char* expected_trailing_kana;
  };
  const Case cases[] = {
      {"wordapuri", 4, "word", "あぷり"},
      {"dockera", 6, "docker", "あ"},
      {"englisha", 7, "english", "あ"},
      {"gita", 3, "git", "あ"},
      {"gitsu", 3, "git", "す"},
      {"gitwo", 3, "git", "を"},
      {"testwo", 4, "test", "を"},
      {"classwo", 5, "class", "を"},
      {"githa", 3, "git", "は"},
      {"loginno", 5, "login", "の"},
      {"linuxno", 5, "linux", "の"},
      {"kuberneteswo", 10, "kubernetes", "を"},
  };

  EnglishFallbackRewriter rewriter(*pos_matcher_);

  for (const auto& tc : cases) {
    composer::Composer composer = CreateComposer(tc.raw_input);
    Segments segments;
    Segment* seg = segments.add_segment();
    const std::string key = composer.GetQueryForConversion();
    seg->set_key(key);
    auto* c = seg->add_candidate();
    c->key = key;
    c->value = key;
    c->cost = 25000;

    const ConversionRequest request =
        ConversionRequestBuilder()
            .SetComposer(composer)
            .SetRequestType(ConversionRequest::CONVERSION)
            .Build();

    const std::vector<size_t> offsets =
        EnglishFallbackRewriter::FindCrossBoundarySplitOffsets(
            request.composer(), segments, request);
    ASSERT_EQ(offsets.size(), 1) << tc.raw_input;
    EXPECT_EQ(offsets[0], tc.expected_offset) << tc.raw_input;

    // Applying the separation and re-running rewriter yields clean English +
    // intended trailing Japanese reading.
    EXPECT_TRUE(composer.SeparateChunksAtRawBoundaries(offsets)) << tc.raw_input;
    const std::string sep_key = composer.GetQueryForConversion();

    Segments sep_segments;
    Segment* sep_seg = sep_segments.add_segment();
    sep_seg->set_key(sep_key);
    auto* sc = sep_seg->add_candidate();
    sc->key = sep_key;
    sc->value = sep_key;
    sc->cost = 25000;

    const ConversionRequest sep_request =
        ConversionRequestBuilder()
            .SetComposer(composer)
            .SetRequestType(ConversionRequest::CONVERSION)
            .Build();
    EXPECT_TRUE(rewriter.Rewrite(sep_request, &sep_segments)) << tc.raw_input;
    ASSERT_EQ(sep_segments.conversion_segments_size(), 2) << tc.raw_input;
    EXPECT_EQ(sep_segments.conversion_segment(0).candidate(0).value,
              tc.expected_english)
        << tc.raw_input;
    EXPECT_EQ(sep_segments.conversion_segment(1).key(),
              tc.expected_trailing_kana)
        << tc.raw_input;
  }

  GruPenaltyConfig clean_cfg;
  clean_cfg.enable_english_fallback = true;
  config::GruConfigManager::SaveConfig(clean_cfg, config_path);
}

TEST_F(EnglishFallbackRewriterTest,
       FindCrossBoundarySplitOffsetsSkipsLowCostAndFixedSegments) {
  for (const char* input : {"mainichi", "sinka", "matta", "kita", "tenki"}) {
    composer::Composer composer = CreateComposer(input);
    Segments segments;
    Segment* seg = segments.add_segment();
    const std::string key = composer.GetQueryForConversion();
    seg->set_key(key);
    auto* c = seg->add_candidate();
    c->key = key;
    c->value = key;
    c->cost = 5000;  // Below threshold (normal Japanese).

    const ConversionRequest request =
        ConversionRequestBuilder()
            .SetComposer(composer)
            .SetRequestType(ConversionRequest::CONVERSION)
            .Build();
    EXPECT_TRUE(EnglishFallbackRewriter::FindCrossBoundarySplitOffsets(
                    request.composer(), segments, request)
                    .empty())
        << input;
  }

  // Respects user-fixed boundaries even on high-cost segments.
  composer::Composer composer = CreateComposer("wordapuri");
  Segments segments;
  Segment* seg = segments.add_segment();
  const std::string key = composer.GetQueryForConversion();
  seg->set_key(key);
  seg->set_segment_type(Segment::FIXED_BOUNDARY);
  auto* c = seg->add_candidate();
  c->key = key;
  c->value = key;
  c->cost = 25000;

  const ConversionRequest request =
      ConversionRequestBuilder()
          .SetComposer(composer)
          .SetRequestType(ConversionRequest::CONVERSION)
          .Build();
  EXPECT_TRUE(EnglishFallbackRewriter::FindCrossBoundarySplitOffsets(
                  request.composer(), segments, request)
                  .empty());
}

}  // namespace
}  // namespace mozc
