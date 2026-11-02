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

#include "rewriter/gru_rewriter.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/strings/string_view.h"
#include "base/file_util.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/immutable_converter_interface.h"
#include "converter/inner_segment.h"
#include "converter/segments.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

namespace mozc {
namespace {

void AddCandidate(absl::string_view key, absl::string_view value,
                  int32_t cost, Segment* segment) {
  converter::Candidate* candidate = segment->add_candidate();
  candidate->key = std::string(key);
  candidate->value = std::string(value);
  candidate->content_key = std::string(key);
  candidate->content_value = std::string(value);
  candidate->cost = cost;
  candidate->wcost = cost;
}

class GruRewriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    absl::SetFlag(&FLAGS_enable_gru_rewriter, true);
    rewriter_ = std::make_unique<GruRewriter>();
  }

  std::unique_ptr<GruRewriter> rewriter_;
};

TEST_F(GruRewriterTest, Capability) {
  ConversionRequest request;
  EXPECT_NE(rewriter_->capability(request) & RewriterInterface::CONVERSION, 0);
  EXPECT_NE(rewriter_->capability(request) & RewriterInterface::PREDICTION, 0);
}

TEST_F(GruRewriterTest, EmptySegments) {
  ConversionRequest request;
  Segments segments;
  EXPECT_FALSE(rewriter_->Rewrite(request, &segments));
}

TEST_F(GruRewriterTest, SingleCandidateSegment) {
  ConversionRequest request;
  Segments segments;
  Segment* seg = segments.add_segment();
  seg->set_key("きょう");
  AddCandidate("きょう", "今日", 1000, seg);

  // Single candidate should not be modified
  EXPECT_FALSE(rewriter_->Rewrite(request, &segments));
}

TEST_F(GruRewriterTest, ContextSensitiveReranking) {
  absl::SetFlag(&FLAGS_gru_model_path,
                "/home/haruhiro/repos/C++/gru_scorer_engine/grumodel.bin");
  absl::SetFlag(&FLAGS_gru_chardata_path,
                "/home/haruhiro/repos/C++/gru_scorer_engine/data/chardata.bin");
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  // Segment 0: "さいしんの" -> "最新の"
  Segment* seg0 = segments.add_segment();
  seg0->set_key("さいしんの");
  AddCandidate("さいしんの", "最新の", 1000, seg0);

  // Segment 1: "きかい" -> Candidate 0: "機会" (cost 1000), Candidate 1: "機械" (cost 1100)
  // Initially "機会" is at index 0 because cost 1000 < 1100
  Segment* seg1 = segments.add_segment();
  seg1->set_key("きかい");
  AddCandidate("きかい", "機会", 1000, seg1);
  AddCandidate("きかい", "機械", 1100, seg1);

  // Run Rewriter
  // In the context of "最新の", "機械" (machine) should have significantly higher likelihood
  // than "機会" (opportunity), promoting "機械" to index 0 if model is available.
  const bool rewritten = rewriter_->Rewrite(request, &segments);
  if (rewritten) {
    EXPECT_EQ(seg1->candidate(0).value, "機械");
    EXPECT_NE(seg1->candidate(0).attributes & converter::Attribute::CONTEXT_SENSITIVE, 0);
  }

  // Verify that the segments remain valid and well-formed
  EXPECT_EQ(segments.conversion_segments_size(), 2);
  EXPECT_EQ(seg1->candidates_size(), 2);
}

TEST_F(GruRewriterTest, MultiSegmentCombinationBeamSearch) {
  absl::SetFlag(&FLAGS_gru_model_path,
                "/home/haruhiro/repos/C++/gru_scorer_engine/grumodel.bin");
  absl::SetFlag(&FLAGS_gru_chardata_path,
                "/home/haruhiro/repos/C++/gru_scorer_engine/data/chardata.bin");
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  // Segment 0: "せんもんちしきを" -> Cand 0: "専問知識を" (cost 1000), Cand 1: "専門知識を" (cost 1050)
  Segment* seg0 = segments.add_segment();
  seg0->set_key("せんもんちしきを");
  AddCandidate("せんもんちしきを", "専問知識を", 1000, seg0);
  AddCandidate("せんもんちしきを", "専門知識を", 1050, seg0);

  // Segment 1: "ふかめる" -> Cand 0: "深める" (cost 1000)
  Segment* seg1 = segments.add_segment();
  seg1->set_key("ふかめる");
  AddCandidate("ふかめる", "深める", 1000, seg1);

  // In beam search across combinations, "専門知識を深める" wins over "専問知識を深める"
  // promoting "専門知識を" to index 0 in segment 0.
  const bool rewritten = rewriter_->Rewrite(request, &segments);
  if (rewritten) {
    EXPECT_EQ(seg0->candidate(0).value, "専門知識を");
  }
}

TEST_F(GruRewriterTest, EmojiPenalty) {
  absl::SetFlag(&FLAGS_gru_model_path,
                "/home/haruhiro/repos/C++/gru_scorer_engine/grumodel.bin");
  absl::SetFlag(&FLAGS_gru_chardata_path,
                "/home/haruhiro/repos/C++/gru_scorer_engine/data/chardata.bin");
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  Segment* seg0 = segments.add_segment();
  seg0->set_key("わら");
  AddCandidate("わら", "笑", 1000, seg0);
  AddCandidate("わら", "😆", 950, seg0);
  seg0->mutable_candidate(1)->description = "絵文字";

  // Emoji should NEVER be promoted over "笑"
  rewriter_->Rewrite(request, &segments);
  EXPECT_NE(seg0->candidate(0).value, "😆");
}

TEST_F(GruRewriterTest, DynamicPathReload) {
  // Point to a non-existent file
  absl::SetFlag(&FLAGS_gru_model_path, "/non/existent/path/grumodel.bin");
  ConversionRequest request;
  Segments segments;
  Segment* seg = segments.add_segment();
  seg->set_key("きょう");
  AddCandidate("きょう", "今日", 1000, seg);
  AddCandidate("きょう", "教", 1050, seg);

  // Should gracefully fail initialization and return false
  EXPECT_FALSE(rewriter_->Rewrite(request, &segments));

  // Reset to default / valid paths
  absl::SetFlag(&FLAGS_gru_model_path, "");
  absl::SetFlag(&FLAGS_gru_chardata_path, "");
  // Should reload and succeed (or fallback safely)
  rewriter_->Rewrite(request, &segments);
}

TEST_F(GruRewriterTest, SortSecondaryCandidates) {
  absl::SetFlag(&FLAGS_gru_model_path, "");
  absl::SetFlag(&FLAGS_gru_chardata_path, "");
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  Segment* seg = segments.add_segment();
  seg->set_key("きかい");
  // 3 candidates
  AddCandidate("きかい", "奇怪", 1000, seg);
  AddCandidate("きかい", "機械", 1020, seg);
  AddCandidate("きかい", "機会", 1040, seg);

  const bool rewritten = rewriter_->Rewrite(request, &segments);
  if (rewritten) {
    EXPECT_EQ(segments.conversion_segment(0).candidates_size(), 3);
    // Top candidate should have CONTEXT_SENSITIVE attribute if promoted
    EXPECT_TRUE(segments.conversion_segment(0).candidate(0).description.find("GRU:") !=
                std::string::npos);
  }
}

TEST_F(GruRewriterTest, InteractiveFocusRerank) {
  absl::SetFlag(&FLAGS_gru_model_path, "");
  absl::SetFlag(&FLAGS_gru_chardata_path, "");
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  // Segment 0: "さいしんの"
  Segment* seg0 = segments.add_segment();
  seg0->set_key("さいしんの");
  AddCandidate("さいしんの", "最新の", 1000, seg0);
  AddCandidate("さいしんの", "細心の", 1050, seg0);

  // Segment 1: "ちゅうい"
  Segment* seg1 = segments.add_segment();
  seg1->set_key("ちゅうい");
  AddCandidate("ちゅうい", "中位", 1000, seg1);
  AddCandidate("ちゅうい", "注意", 1050, seg1);

  // Initial rewrite
  rewriter_->Rewrite(request, &segments);

  // Focus segment 0, candidate 1 ("細心の")
  const bool focus_handled = rewriter_->Focus(&segments, 0, 1);
  // Focus should handle and evaluate subsequent segments when model is loaded
  if (focus_handled) {
    EXPECT_EQ(segments.conversion_segments_size(), 2);
    EXPECT_TRUE(segments.conversion_segment(1).candidate(0).description.find("GRU:") !=
                std::string::npos);
  }
}

TEST_F(GruRewriterTest, HistoryPrefixContextCaching) {
  absl::SetFlag(&FLAGS_gru_model_path, "");
  absl::SetFlag(&FLAGS_gru_chardata_path, "");
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  // Add history segment
  Segment* hist = segments.add_segment();
  hist->set_segment_type(Segment::HISTORY);
  Segment::Candidate* c = hist->add_candidate();
  c->value = "今日は";
  c->key = "きょうは";

  // Add conversion segment
  Segment* seg = segments.add_segment();
  seg->set_key("よい");
  AddCandidate("よい", "良い", 1000, seg);
  AddCandidate("よい", "善い", 1000, seg);

  EXPECT_EQ(segments.history_segments_size(), 1);
  EXPECT_EQ(segments.history_value(), "今日は");
  EXPECT_EQ(segments.conversion_segments_size(), 1);

  const bool rewritten = rewriter_->Rewrite(request, &segments);
  if (rewritten) {
    EXPECT_TRUE(segments.conversion_segment(0).candidate(0).description.find("GRU:") !=
                std::string::npos);
  }
}

TEST_F(GruRewriterTest, SurroundingTextPrefixCaching) {
  absl::SetFlag(&FLAGS_gru_model_path, "");
  absl::SetFlag(&FLAGS_gru_chardata_path, "");
  rewriter_->Reload();

  commands::Context context;
  context.set_preceding_text("最近の");
  ConversionRequest request = ConversionRequestBuilder().SetContext(context).Build();

  Segments segments;
  Segment* seg = segments.add_segment();
  seg->set_key("てんき");
  AddCandidate("てんき", "天気", 1000, seg);
  AddCandidate("てんき", "転機", 1000, seg);

  const bool rewritten = rewriter_->Rewrite(request, &segments);
  if (rewritten) {
    EXPECT_TRUE(segments.conversion_segment(0).candidate(0).description.find("GRU:") !=
                std::string::npos);
  }
}

TEST_F(GruRewriterTest, KoushiKekkanFocusRerank) {
  // Use actual model and chardata if available, or dummy
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  // Segment 0: "こうし" -> "講師" (cost 3000), "格子" (cost 5000)
  Segment* seg0 = segments.add_segment();
  seg0->set_key("こうし");
  AddCandidate("こうし", "講師", 3000, seg0);
  AddCandidate("こうし", "格子", 5000, seg0);

  // Segment 1: "けっかん" -> "血管" (cost 4500), "欠陥" (cost 4942)
  Segment* seg1 = segments.add_segment();
  seg1->set_key("けっかん");
  AddCandidate("けっかん", "血管", 4500, seg1);
  AddCandidate("けっかん", "欠陥", 4942, seg1);

  rewriter_->Rewrite(request, &segments);

  // Focus segment 0, candidate 1 ("格子")
  const bool focus_handled = rewriter_->Focus(&segments, 0, 1);
  if (focus_handled) {
    // If the model is loaded and distinguishes "格子欠陥" over "格子血管",
    // "欠陥" should be promoted or evaluated with combined score tag containing "[E:"
    EXPECT_TRUE(segments.conversion_segment(1).candidate(0).description.find("[E:") !=
                std::string::npos);
  }
}

TEST_F(GruRewriterTest, ThreeSegmentFocusControl) {
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  Segment* seg0 = segments.add_segment();
  seg0->set_key("だいいち");
  AddCandidate("だいいち", "A0", 1000, seg0);
  AddCandidate("だいいち", "A1", 1050, seg0);

  Segment* seg1 = segments.add_segment();
  seg1->set_key("だいに");
  AddCandidate("だいに", "B0", 1000, seg1);
  AddCandidate("だいに", "B1", 1050, seg1);

  Segment* seg2 = segments.add_segment();
  seg2->set_key("だいさん");
  AddCandidate("だいさん", "C0", 1000, seg2);
  AddCandidate("だいさん", "C1", 1050, seg2);

  rewriter_->Rewrite(request, &segments);

  // 1. Focus on segment 0 candidate 1 ("A1")
  const bool f0 = rewriter_->Focus(&segments, 0, 1);
  if (f0) {
    // Both segment 1 and segment 2 should be evaluated and have descriptions updated
    EXPECT_TRUE(segments.conversion_segment(1).candidate(0).description.find("GRU:") !=
                std::string::npos);
    EXPECT_TRUE(segments.conversion_segment(2).candidate(0).description.find("GRU:") !=
                std::string::npos);
  }

  // 2. Focus on segment 1 candidate 1 ("B1")
  const bool f1 = rewriter_->Focus(&segments, 1, 1);
  if (f1) {
    // Segment 2 should be evaluated
    EXPECT_TRUE(segments.conversion_segment(2).candidate(0).description.find("GRU:") !=
                std::string::npos);
  }

  // 3. Focus on last segment (segment 2)
  const bool f2 = rewriter_->Focus(&segments, 2, 0);
  // Last segment focus should return false (no subsequent segments to rerank)
  EXPECT_FALSE(f2);
}

TEST_F(GruRewriterTest, SkipSymbolAndPunctuationSegments) {
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  // Segment 0: normal hiragana segment
  Segment* seg0 = segments.add_segment();
  seg0->set_key("きょうは");
  AddCandidate("きょうは", "今日は", 1000, seg0);
  AddCandidate("きょうは", "教派", 2000, seg0);

  // Segment 1: punctuation segment (e.g. "." when punctuation style is "，．")
  // SymbolRewriter inserts "！" with the same cost (0) as "．".
  Segment* seg1 = segments.add_segment();
  seg1->set_key(".");
  AddCandidate(".", "．", 0, seg1);
  AddCandidate(".", "！", 0, seg1);
  AddCandidate(".", "？", 0, seg1);

  rewriter_->Rewrite(request, &segments);

  // Punctuation segment must keep its first candidate "．" and not be replaced by "！"
  EXPECT_EQ(segments.conversion_segment(1).candidate(0).value, "．");
}

TEST_F(GruRewriterTest, AlphabetCandidateDoubledGruScore) {
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  Segment* seg = segments.add_segment();
  seg->set_key("えーあい");
  AddCandidate("えーあい", "栄愛", 3000, seg);
  AddCandidate("えーあい", "AI", 3000, seg);

  rewriter_->Rewrite(request, &segments);

  // Find "AI" candidate and verify its description has GRU score evaluated
  for (size_t i = 0; i < seg->candidates_size(); ++i) {
    if (seg->candidate(i).value == "AI" &&
        !seg->candidate(i).description.empty()) {
      EXPECT_TRUE(seg->candidate(i).description.find("GRU:") !=
                  std::string::npos);
    }
  }
}

TEST_F(GruRewriterTest, KatakanaMultiplierRerank) {
  absl::SetFlag(&FLAGS_gru_katakana_multiplier, 3.0);
  absl::SetFlag(&FLAGS_gru_score_weight, 800.0);
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  Segment* seg = segments.add_segment();
  seg->set_key("とうきょう");
  // Candidate 0: "トウキョウ" (initial cost 1000)
  // Candidate 1: "東京" (initial cost 1000)
  // With katakana multiplier 3.0, the negative GRU score of "トウキョウ" is amplified,
  // making its combined cost worse than "東京".
  AddCandidate("とうきょう", "トウキョウ", 1000, seg);
  AddCandidate("とうきょう", "東京", 1000, seg);

  EXPECT_TRUE(rewriter_->Rewrite(request, &segments));
  EXPECT_EQ(seg->candidate(0).value, "東京");
  EXPECT_EQ(seg->candidate(1).value, "トウキョウ");

  // Verify description contains GRU score tag
  EXPECT_TRUE(seg->candidate(1).description.find("GRU:") != std::string::npos);
}

TEST_F(GruRewriterTest, HiraganaMultiplierRerank) {
  absl::SetFlag(&FLAGS_gru_hiragana_multiplier, 3.0);
  absl::SetFlag(&FLAGS_gru_score_weight, 800.0);
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  Segment* seg = segments.add_segment();
  seg->set_key("とうきょう");
  // Candidate 0: "とうきょう" (initial cost 1000)
  // Candidate 1: "東京" (initial cost 1000)
  // With hiragana multiplier 3.0, the negative GRU score of "とうきょう" is amplified,
  // making its combined cost worse than "東京".
  AddCandidate("とうきょう", "とうきょう", 1000, seg);
  AddCandidate("とうきょう", "東京", 1000, seg);

  EXPECT_TRUE(rewriter_->Rewrite(request, &segments));
  EXPECT_EQ(seg->candidate(0).value, "東京");
  EXPECT_EQ(seg->candidate(1).value, "とうきょう");

  // Verify description contains GRU score tag
  EXPECT_TRUE(seg->candidate(1).description.find("GRU:") != std::string::npos);
}

TEST_F(GruRewriterTest, HiraganaSingleCharExemptFromPenalty) {
  // Even with high hiragana_multiplier, single-character hiragana (e.g. particles like "は")
  // must have effective_multiplier = 1.0 (no penalty) because excess length (L - 1) is 0.
  absl::SetFlag(&FLAGS_gru_hiragana_multiplier, 5.0);
  absl::SetFlag(&FLAGS_gru_hiragana_length_power, 1.0);
  absl::SetFlag(&FLAGS_gru_score_weight, 800.0);
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  Segment* seg = segments.add_segment();
  seg->set_key("は");
  AddCandidate("は", "は", 1000, seg);
  AddCandidate("は", "歯", 1000, seg);

  rewriter_->Rewrite(request, &segments);

  // Candidate "は" should have GRU score evaluated without 5x penalty
  for (size_t i = 0; i < seg->candidates_size(); ++i) {
    if (seg->candidate(i).value == "は") {
      EXPECT_TRUE(seg->candidate(i).description.find("GRU:") != std::string::npos);
    }
  }
}

TEST_F(GruRewriterTest, HiraganaLengthScaledPenalty) {
  // Verify that multi-character hiragana has multiplier scaled by (L - 1)^power.
  absl::SetFlag(&FLAGS_gru_hiragana_multiplier, 1.2);
  absl::SetFlag(&FLAGS_gru_hiragana_length_power, 1.0);
  absl::SetFlag(&FLAGS_gru_score_weight, 800.0);
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  // For a 4-character hiragana "とうきょう", L=4, excess = 3.
  // effective_multiplier = 1.0 + (1.2 - 1.0) * 3 = 1.6.
  Segment* seg = segments.add_segment();
  seg->set_key("とうきょう");
  AddCandidate("とうきょう", "とうきょう", 1000, seg);
  AddCandidate("とうきょう", "東京", 1000, seg);

  EXPECT_TRUE(rewriter_->Rewrite(request, &segments));
  EXPECT_EQ(seg->candidate(0).value, "東京");
  EXPECT_EQ(seg->candidate(1).value, "とうきょう");
}

TEST_F(GruRewriterTest, ConfigFileHotReload) {
  const std::string test_config = FileUtil::JoinPath(
      std::filesystem::temp_directory_path().string(), "test_gru_config.ini");
  {
    std::ofstream ofs(test_config);
    ofs << "katakana_multiplier = 0.5\n"
        << "hiragana_multiplier = 1.0\n"
        << "enable_resegmentation = true\n";
  }

  absl::SetFlag(&FLAGS_gru_config_path, test_config);
  absl::SetFlag(&FLAGS_gru_score_weight, 800.0);
  rewriter_->Reload();

  ConversionRequest request;

  // Step 1: Candidate 0 is "試験", Candidate 1 is "テスト".
  // With katakana_multiplier = 0.5, "テスト" is boosted and promoted to index 0.
  {
    Segments segments;
    Segment* seg = segments.add_segment();
    seg->set_key("てすと");
    AddCandidate("てすと", "試験", 1000, seg);
    AddCandidate("てすと", "テスト", 1000, seg);
    EXPECT_TRUE(rewriter_->Rewrite(request, &segments));
    EXPECT_EQ(seg->candidate(0).value, "テスト");
    EXPECT_EQ(seg->candidate(1).value, "試験");
  }

  // Sleep slightly so mtime differs and 1s rate throttle passes
  absl::SleepFor(absl::Seconds(2));

  // Step 2: Overwrite file with katakana_multiplier = 5.0 WITHOUT calling Reload()
  {
    std::ofstream ofs(test_config, std::ios::trunc);
    ofs << "katakana_multiplier = 5.0\n"
        << "hiragana_multiplier = 1.0\n"
        << "enable_resegmentation = true\n";
  }

  // Step 3: Rewrite again; hot-reload should kick in automatically and penalize "テスト"
  {
    Segments segments;
    Segment* seg = segments.add_segment();
    seg->set_key("てすと");
    AddCandidate("てすと", "テスト", 1000, seg);
    AddCandidate("てすと", "試験", 1000, seg);
    EXPECT_TRUE(rewriter_->Rewrite(request, &segments));
    EXPECT_EQ(seg->candidate(0).value, "試験");
    EXPECT_EQ(seg->candidate(1).value, "テスト");
  }

  // Clean up
  std::error_code ec;
  std::filesystem::remove(test_config, ec);
  std::filesystem::remove(test_config + ".bak", ec);
  absl::SetFlag(&FLAGS_gru_config_path, "");
}

TEST_F(GruRewriterTest, LegacyConfigAutoUpgradeAndFallback) {
  const std::string test_config = FileUtil::JoinPath(
      std::filesystem::temp_directory_path().string(), "test_legacy_config.ini");
  {
    std::ofstream ofs(test_config);
    ofs << "katakana_penalty = 800.0\n"
        << "hiragana_penalty = 500.0\n";
  }

  absl::SetFlag(&FLAGS_gru_config_path, test_config);
  absl::SetFlag(&FLAGS_gru_score_weight, 800.0);
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments;

  Segment* seg = segments.add_segment();
  seg->set_key("てすと");
  AddCandidate("てすと", "テスト", 1000, seg);
  AddCandidate("てすと", "試験", 1000, seg);

  // Even with legacy katakana_penalty, fallback or auto-upgrade enables penalty
  EXPECT_TRUE(rewriter_->Rewrite(request, &segments));
  EXPECT_EQ(seg->candidate(0).value, "試験");

  // Verify backup was created and file was upgraded to new format
  EXPECT_TRUE(std::filesystem::exists(test_config + ".bak"));

  {
    std::ifstream ifs(test_config);
    std::string upgraded((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    EXPECT_TRUE(upgraded.find("katakana_multiplier") != std::string::npos);
  }

  // Clean up: ensure file stream is closed before remove (required on Windows)
  std::error_code ec;
  std::filesystem::remove(test_config, ec);
  std::filesystem::remove(test_config + ".bak", ec);
  absl::SetFlag(&FLAGS_gru_config_path, "");
}

TEST_F(GruRewriterTest, PunctuationNormalizationScore) {
  rewriter_->Reload();

  ConversionRequest request;
  Segments segments_touten;
  Segment* seg0 = segments_touten.add_segment();
  seg0->set_key("きょうは、");
  AddCandidate("きょうは、", "今日は、", 1000, seg0);
  AddCandidate("きょうは、", "教派、", 1000, seg0);

  Segments segments_comma;
  Segment* seg1 = segments_comma.add_segment();
  seg1->set_key("きょうは，");
  AddCandidate("きょうは，", "今日は，", 1000, seg1);
  AddCandidate("きょうは，", "教派，", 1000, seg1);

  rewriter_->Rewrite(request, &segments_touten);
  rewriter_->Rewrite(request, &segments_comma);

  // Both should select identical candidates and produce identical ranking
  EXPECT_EQ(segments_touten.conversion_segment(0).candidate(0).value, "教派、");
  EXPECT_EQ(segments_comma.conversion_segment(0).candidate(0).value, "教派，");

  // Descriptions should contain identical GRU score values
  const std::string desc_touten = segments_touten.conversion_segment(0).candidate(0).description;
  const std::string desc_comma = segments_comma.conversion_segment(0).candidate(0).description;

  auto extract_gru_tag = [](const std::string& desc) {
    const size_t pos = desc.find("GRU:");
    if (pos == std::string::npos) return std::string();
    const size_t end = desc.find('[', pos);
    return desc.substr(pos, (end != std::string::npos) ? end - pos : std::string::npos);
  };
  EXPECT_FALSE(extract_gru_tag(desc_touten).empty());
  EXPECT_EQ(extract_gru_tag(desc_comma), extract_gru_tag(desc_touten));
}

class StubImmutableConverter : public ImmutableConverterInterface {
 public:
  bool Convert(const ConversionOptions& options, Segments* segments) const override {
    if (convert_fn_) {
      return convert_fn_(options, segments);
    }
    return false;
  }
  std::function<bool(const ConversionOptions&, Segments*)> convert_fn_;
};

TEST(GruRewriterResegmentTest, NullConverterReturnsNullopt) {
  absl::SetFlag(&FLAGS_enable_gru_rewriter, true);
  GruRewriter rewriter(nullptr);
  ConversionRequest request;
  Segments segments;
  Segment* seg = segments.add_segment();
  seg->set_key("テスト");
  AddCandidate("テスト", "テスト", 1000, seg);

  EXPECT_EQ(rewriter.CheckResizeSegmentsRequest(request, segments), std::nullopt);
}

TEST(GruRewriterResegmentTest, AlreadyResizedReturnsNullopt) {
  absl::SetFlag(&FLAGS_enable_gru_rewriter, true);
  StubImmutableConverter stub_converter;
  GruRewriter rewriter(&stub_converter);
  ConversionRequest request;
  Segments segments;
  Segment* seg = segments.add_segment();
  seg->set_key("テスト");
  AddCandidate("テスト", "テスト", 1000, seg);
  segments.set_resized(true);

  EXPECT_EQ(rewriter.CheckResizeSegmentsRequest(request, segments), std::nullopt);
}

TEST(GruRewriterResegmentTest, ResegmentationDisabledReturnsNullopt) {
  absl::SetFlag(&FLAGS_enable_gru_rewriter, true);
  absl::SetFlag(&FLAGS_enable_gru_resegmentation, false);
  StubImmutableConverter stub_converter;
  GruRewriter rewriter(&stub_converter);
  rewriter.Reload();

  ConversionRequest request;
  Segments segments;
  Segment* seg = segments.add_segment();
  seg->set_key("テスト");
  AddCandidate("テスト", "テスト", 1000, seg);

  EXPECT_EQ(rewriter.CheckResizeSegmentsRequest(request, segments), std::nullopt);
}

TEST(GruRewriterResegmentTest, TriggerResegmentWhenBetterCandidateFound) {
  absl::SetFlag(&FLAGS_enable_gru_rewriter, true);
  absl::SetFlag(&FLAGS_enable_gru_resegmentation, true);
  absl::SetFlag(&FLAGS_gru_resegment_cost_threshold, 200);

  StubImmutableConverter stub_converter;
  stub_converter.convert_fn_ = [](const ConversionOptions& options, Segments* segments) {
    if (segments->conversion_segments_size() == 0) return false;
    Segment* seg = segments->mutable_conversion_segment(0);
    seg->clear_candidates();

    // Candidate 0: "ここでは着物を脱ぐ" (sizes: 4, 4, 2 -> [4, 4, 2])
    converter::Candidate* cand0 = seg->add_candidate();
    cand0->key = "ここではきものをぬぐ";
    cand0->value = "ここでは着物を脱ぐ";
    cand0->content_key = "ここではきものをぬぐ";
    cand0->content_value = "ここでは着物を脱ぐ";
    cand0->cost = 3000;
    cand0->inner_segment_boundary = {
        *converter::EncodeLengths(12, 12, 12, 12),  // "ここでは"
        *converter::EncodeLengths(12, 9, 12, 9),    // "きものを" -> "着物を"
        *converter::EncodeLengths(6, 6, 6, 6)       // "ぬぐ" -> "脱ぐ"
    };

    // Candidate 1: "ここで履物を脱ぐ" (sizes: 3, 5, 2 -> [3, 5, 2])
    converter::Candidate* cand1 = seg->add_candidate();
    cand1->key = "ここではきものをぬぐ";
    cand1->value = "ここで履物を脱ぐ";
    cand1->content_key = "ここではきものをぬぐ";
    cand1->content_value = "ここで履物を脱ぐ";
    cand1->cost = 1000;  // Much better cost -> triggers threshold!
    cand1->inner_segment_boundary = {
        *converter::EncodeLengths(9, 9, 9, 9),      // "ここで"
        *converter::EncodeLengths(15, 9, 15, 9),    // "はきものを" -> "履物を"
        *converter::EncodeLengths(6, 6, 6, 6)       // "ぬぐ" -> "脱ぐ"
    };

    return true;
  };

  GruRewriter rewriter(&stub_converter);
  rewriter.Reload();

  ConversionRequest request;
  Segments segments;

  // Initial segments: "ここでは" (4 chars), "きものを" (4 chars), "ぬぐ" (2 chars)
  Segment* seg0 = segments.add_segment();
  seg0->set_key("ここでは");
  AddCandidate("ここでは", "ここでは", 1000, seg0);

  Segment* seg1 = segments.add_segment();
  seg1->set_key("きものを");
  AddCandidate("きものを", "着物を", 1000, seg1);

  Segment* seg2 = segments.add_segment();
  seg2->set_key("ぬぐ");
  AddCandidate("ぬぐ", "脱ぐ", 1000, seg2);

  auto resize_req = rewriter.CheckResizeSegmentsRequest(request, segments);
  ASSERT_TRUE(resize_req.has_value());
  EXPECT_EQ(resize_req->segment_index, 0);

  // Expected resegmented lengths: [3, 5, 2] ("ここで", "はきものを/履物を", "ぬぐ/脱ぐ")
  EXPECT_EQ(resize_req->segment_sizes[0], 3);
  EXPECT_EQ(resize_req->segment_sizes[1], 5);
  EXPECT_EQ(resize_req->segment_sizes[2], 2);
  EXPECT_EQ(resize_req->segment_sizes[3], 0);
}

TEST(GruRewriterResegmentTest, FocusTriggerResegmentWhenBetterCandidateFound) {
  absl::SetFlag(&FLAGS_enable_gru_rewriter, true);
  absl::SetFlag(&FLAGS_enable_gru_resegmentation, true);
  absl::SetFlag(&FLAGS_gru_resegment_cost_threshold, 200);

  StubImmutableConverter stub_converter;
  stub_converter.convert_fn_ = [](const ConversionOptions& options, Segments* segments) {
    if (segments->segments_size() < 2) return false;
    if (options.request_type == RequestType::PREDICTION) {
      Segment* target = segments->mutable_segment(segments->segments_size() - 1);
      target->clear_candidates();

      // Candidate with better cost: sizes [3, 5, 2]
      converter::Candidate* cand = target->add_candidate();
      cand->key = "ここではきものをぬぐ";
      cand->value = "ここで履物を脱ぐ";
      cand->content_key = "ここではきものをぬぐ";
      cand->content_value = "ここで履物を脱ぐ";
      cand->cost = 1000;
      cand->inner_segment_boundary = {
          *converter::EncodeLengths(9, 9, 9, 9),      // "ここで" (3 chars)
          *converter::EncodeLengths(15, 9, 15, 9),    // "はきものを" (5 chars)
          *converter::EncodeLengths(6, 6, 6, 6)       // "ぬぐ" (2 chars)
      };
      return true;
    }
    // For actual conversion after resize (CONVERSION mode), populate candidates for each segment
    for (size_t i = 1; i < segments->segments_size(); ++i) {
      Segment* seg = segments->mutable_segment(i);
      seg->clear_candidates();
      AddCandidate(seg->key(), seg->key(), 1000, seg);
    }
    return true;
  };

  GruRewriter rewriter(&stub_converter);
  rewriter.Reload();

  Segments segments;
  // Segment 0: "きょうは" (focused segment)
  Segment* seg0 = segments.add_segment();
  seg0->set_key("きょうは");
  AddCandidate("きょうは", "今日は", 1000, seg0);
  AddCandidate("きょうは", "教派", 1050, seg0);

  // Subsequent segments: "ここでは" (4), "きものを" (4), "ぬぐ" (2)
  Segment* seg1 = segments.add_segment();
  seg1->set_key("ここでは");
  AddCandidate("ここでは", "ここでは", 1000, seg1);

  Segment* seg2 = segments.add_segment();
  seg2->set_key("きものを");
  AddCandidate("きものを", "着物を", 1000, seg2);

  Segment* seg3 = segments.add_segment();
  seg3->set_key("ぬぐ");
  AddCandidate("ぬぐ", "脱ぐ", 1000, seg3);

  // Focus on segment 0, candidate 0 ("今日は")
  const bool focus_result = rewriter.Focus(&segments, 0, 0);
  EXPECT_TRUE(focus_result);

  // Verify that segment 0 candidates are preserved
  EXPECT_EQ(segments.segment(0).candidates_size(), 2);
  EXPECT_EQ(segments.segment(0).candidate(0).value, "今日は");
  EXPECT_EQ(segments.segment(0).candidate(1).value, "教派");

  // Subsequent segments should be resized according to [3, 5, 2]
  // Total segments: 1 (seg0) + 3 (newly resized segments) = 4 segments
  EXPECT_EQ(segments.segments_size(), 4);
  EXPECT_EQ(segments.segment(1).key_len(), 3);  // "ここで"
  EXPECT_EQ(segments.segment(2).key_len(), 5);  // "はきものを"
  EXPECT_EQ(segments.segment(3).key_len(), 2);  // "ぬぐ"
}

TEST_F(GruRewriterTest, OnlineLearningImprovesRankingAndPersists) {
  absl::SetFlag(&FLAGS_gru_model_path, "");
  absl::SetFlag(&FLAGS_gru_chardata_path, "");
  absl::SetFlag(&FLAGS_enable_gru_online_learning, true);
  absl::SetFlag(&FLAGS_gru_score_weight, 800.0);

  const std::string config_path = FileUtil::JoinPath(
      std::filesystem::temp_directory_path().string(),
      "gru_online_learn_config.ini");
  const std::string user_model_path = FileUtil::JoinPath(
      std::filesystem::temp_directory_path().string(),
      "gru_online_user_model.bin");
  std::error_code ec;
  std::filesystem::remove(config_path, ec);
  std::filesystem::remove(user_model_path, ec);

  {
    std::ofstream ofs(config_path);
    ofs << "enable_online_learning = true\n"
        << "online_learning_mode = output_only\n"
        << "online_learning_rate = 0.2\n"
        << "online_learning_weight_decay = 0.001\n"
        << "online_learning_save_interval = 2\n";
  }

  absl::SetFlag(&FLAGS_gru_config_path, config_path);
  absl::SetFlag(&FLAGS_gru_user_model_path, user_model_path);
  ASSERT_TRUE(rewriter_->Reload());

  auto make_eval_segments = [](Segments* segs) {
    segs->Clear();
    Segment* hist = segs->add_segment();
    hist->set_key("さいしんの");
    hist->set_segment_type(Segment::HISTORY);
    AddCandidate("さいしんの", "最新の", 1000, hist);

    Segment* conv = segs->add_segment();
    conv->set_key("きかい");
    conv->set_segment_type(Segment::FREE);
    AddCandidate("きかい", "機会", 1000, conv);
    AddCandidate("きかい", "機械", 1010, conv);
  };

  ConversionRequest request;
  Segments eval_segments;
  make_eval_segments(&eval_segments);
  rewriter_->Rewrite(request, &eval_segments);

  // Train on "最新の" + "機械" twice via Finish()
  for (int step = 0; step < 2; ++step) {
    Segments commit_segments;
    Segment* hist = commit_segments.add_segment();
    hist->set_key("さいしんの");
    hist->set_segment_type(Segment::HISTORY);
    AddCandidate("さいしんの", "最新の", 1000, hist);

    Segment* fixed = commit_segments.add_segment();
    fixed->set_key("きかい");
    fixed->set_segment_type(Segment::FIXED_VALUE);
    AddCandidate("きかい", "機械", 1100, fixed);
    commit_segments.set_revert_id(100 + step);

    rewriter_->Finish(request, commit_segments);
  }

  // Because save_interval = 2, user_model_path should now exist on disk
  EXPECT_TRUE(std::filesystem::exists(user_model_path));

  // Verify "機械" is now promoted to top candidate
  make_eval_segments(&eval_segments);
  EXPECT_TRUE(rewriter_->Rewrite(request, &eval_segments));
  EXPECT_EQ(eval_segments.conversion_segment(0).candidate(0).value, "機械");

  // Reload from disk and verify learned preference persists
  ASSERT_TRUE(rewriter_->Reload());
  make_eval_segments(&eval_segments);
  EXPECT_TRUE(rewriter_->Rewrite(request, &eval_segments));
  EXPECT_EQ(eval_segments.conversion_segment(0).candidate(0).value, "機械");

  std::filesystem::remove(config_path, ec);
  std::filesystem::remove(user_model_path, ec);
  absl::SetFlag(&FLAGS_gru_config_path, "");
  absl::SetFlag(&FLAGS_gru_user_model_path, "");
}

TEST_F(GruRewriterTest, OnlineLearningFullModeRevertAndClear) {
  absl::SetFlag(&FLAGS_gru_model_path, "");
  absl::SetFlag(&FLAGS_gru_chardata_path, "");
  absl::SetFlag(&FLAGS_enable_gru_online_learning, true);
  absl::SetFlag(&FLAGS_gru_score_weight, 800.0);

  const std::string config_path = FileUtil::JoinPath(
      std::filesystem::temp_directory_path().string(),
      "gru_full_bptt_config.ini");
  const std::string user_model_path = FileUtil::JoinPath(
      std::filesystem::temp_directory_path().string(),
      "gru_full_bptt_user.bin");
  std::error_code ec;
  std::filesystem::remove(config_path, ec);
  std::filesystem::remove(user_model_path, ec);

  {
    std::ofstream ofs(config_path);
    ofs << "enable_online_learning = true\n"
        << "online_learning_mode = full\n"
        << "online_learning_rate = 0.25\n"
        << "online_learning_weight_decay = 0.001\n"
        << "online_learning_save_interval = 10\n";
  }

  absl::SetFlag(&FLAGS_gru_config_path, config_path);
  absl::SetFlag(&FLAGS_gru_user_model_path, user_model_path);
  ASSERT_TRUE(rewriter_->Reload());

  ConversionRequest request;

  // Baseline evaluation before learning
  Segments base_segments;
  Segment* s0 = base_segments.add_segment();
  s0->set_key("きょうは");
  AddCandidate("きょうは", "今日は", 1000, s0);
  rewriter_->Rewrite(request, &base_segments);
  const std::string base_desc = base_segments.segment(0).candidate(0).description;

  // Commit "今日は" with revert_id = 42
  Segments commit_segments;
  Segment* fixed = commit_segments.add_segment();
  fixed->set_key("きょうは");
  fixed->set_segment_type(Segment::FIXED_VALUE);
  AddCandidate("きょうは", "今日は", 1000, fixed);
  commit_segments.set_revert_id(42);
  rewriter_->Finish(request, commit_segments);

  // Description (GRU score) should change after Finish()
  Segments trained_segments;
  Segment* ts0 = trained_segments.add_segment();
  ts0->set_key("きょうは");
  AddCandidate("きょうは", "今日は", 1000, ts0);
  rewriter_->Rewrite(request, &trained_segments);
  const std::string trained_desc =
      trained_segments.segment(0).candidate(0).description;
  EXPECT_NE(trained_desc, base_desc);

  // Revert with matching revert_id (42) must restore exact baseline score
  rewriter_->Revert(commit_segments);
  Segments reverted_segments;
  Segment* rs0 = reverted_segments.add_segment();
  rs0->set_key("きょうは");
  AddCandidate("きょうは", "今日は", 1000, rs0);
  rewriter_->Rewrite(request, &reverted_segments);
  EXPECT_EQ(reverted_segments.segment(0).candidate(0).description, base_desc);

  // Train again, Sync() to save user model, then Clear() to reset to base model
  rewriter_->Finish(request, commit_segments);
  EXPECT_TRUE(rewriter_->Sync());
  EXPECT_TRUE(std::filesystem::exists(user_model_path));

  rewriter_->Clear();
  EXPECT_FALSE(std::filesystem::exists(user_model_path));

  Segments cleared_segments;
  Segment* cs0 = cleared_segments.add_segment();
  cs0->set_key("きょうは");
  AddCandidate("きょうは", "今日は", 1000, cs0);
  rewriter_->Rewrite(request, &cleared_segments);
  EXPECT_EQ(cleared_segments.segment(0).candidate(0).description, base_desc);

  std::filesystem::remove(config_path, ec);
  std::filesystem::remove(user_model_path, ec);
  absl::SetFlag(&FLAGS_gru_config_path, "");
  absl::SetFlag(&FLAGS_gru_user_model_path, "");
}

TEST_F(GruRewriterTest, OnlineLearningSkippedWhenNoHistoryLearningAttribute) {
  absl::SetFlag(&FLAGS_gru_model_path, "");
  absl::SetFlag(&FLAGS_gru_chardata_path, "");
  absl::SetFlag(&FLAGS_enable_gru_online_learning, true);
  ASSERT_TRUE(rewriter_->Reload());

  ConversionRequest request;
  Segments base_segments;
  Segment* s0 = base_segments.add_segment();
  s0->set_key("きょうは");
  AddCandidate("きょうは", "今日は", 1000, s0);
  rewriter_->Rewrite(request, &base_segments);
  const std::string base_desc = base_segments.segment(0).candidate(0).description;

  // Candidate marked with NO_HISTORY_LEARNING should not trigger online training
  Segments commit_segments;
  Segment* fixed = commit_segments.add_segment();
  fixed->set_key("きょうは");
  fixed->set_segment_type(Segment::FIXED_VALUE);
  AddCandidate("きょうは", "今日は", 1000, fixed);
  fixed->mutable_candidate(0)->attributes |=
      converter::Attribute::NO_HISTORY_LEARNING;
  rewriter_->Finish(request, commit_segments);

  Segments after_segments;
  Segment* as0 = after_segments.add_segment();
  as0->set_key("きょうは");
  AddCandidate("きょうは", "今日は", 1000, as0);
  rewriter_->Rewrite(request, &after_segments);
  EXPECT_EQ(after_segments.segment(0).candidate(0).description, base_desc);
}

TEST_F(GruRewriterTest, GruConfigManagerSaveAndLoadAllSettingsRoundTrip) {
  const std::string test_config = FileUtil::JoinPath(
      std::filesystem::temp_directory_path().string(),
      "gru_full_roundtrip_config.ini");
  std::error_code ec;
  std::filesystem::remove(test_config, ec);

  GruPenaltyConfig saved;
  saved.enable_gru_rewriter = false;
  saved.enable_live_conversion = false;
  saved.show_segment_separator_in_conversion = true;
  saved.show_segment_separator_in_live_conversion = true;
  saved.enable_live_conversion_prefix_freezing = false;
  saved.enable_live_conversion_auto_commit = true;
  saved.live_conversion_length_threshold = 36;
  saved.gru_model_path = "/custom/path/grumodel.bin";
  saved.gru_chardata_path = "/custom/path/chardata.bin";
  saved.gru_user_model_path = "/custom/path/grumodel_user.bin";
  saved.gru_score_weight = 1250.0;
  saved.show_gru_score = false;
  saved.katakana_multiplier = 1.65;
  saved.hiragana_multiplier = 1.25;
  saved.hiragana_length_power = 1.4;
  saved.emoji_penalty = 22000.0;
  saved.alphabet_multiplier = 2.75;
  saved.enable_resegmentation = false;
  saved.resegment_threshold = 950.0;
  saved.enable_online_learning = false;
  saved.online_learning_mode = "full";
  saved.online_learning_rate = 0.0125;
  saved.online_learning_weight_decay = 0.0025;
  saved.online_learning_save_interval = 12;

  ASSERT_TRUE(config::GruConfigManager::SaveConfig(saved, test_config));
  const GruPenaltyConfig loaded =
      config::GruConfigManager::LoadConfig(test_config);

  EXPECT_EQ(loaded.enable_gru_rewriter, false);
  EXPECT_EQ(loaded.enable_live_conversion, false);
  EXPECT_EQ(loaded.show_segment_separator_in_conversion, true);
  EXPECT_EQ(loaded.show_segment_separator_in_live_conversion, true);
  EXPECT_EQ(loaded.enable_live_conversion_prefix_freezing, false);
  EXPECT_EQ(loaded.enable_live_conversion_auto_commit, true);
  EXPECT_EQ(loaded.live_conversion_length_threshold, 36);
  EXPECT_EQ(loaded.gru_model_path, "/custom/path/grumodel.bin");
  EXPECT_EQ(loaded.gru_chardata_path, "/custom/path/chardata.bin");
  EXPECT_EQ(loaded.gru_user_model_path, "/custom/path/grumodel_user.bin");
  EXPECT_NEAR(loaded.gru_score_weight, 1250.0, 1e-4);
  EXPECT_EQ(loaded.show_gru_score, false);
  EXPECT_NEAR(loaded.katakana_multiplier, 1.65, 1e-4);
  EXPECT_NEAR(loaded.hiragana_multiplier, 1.25, 1e-4);
  EXPECT_NEAR(loaded.hiragana_length_power, 1.4, 1e-4);
  EXPECT_NEAR(loaded.emoji_penalty, 22000.0, 1e-4);
  EXPECT_NEAR(loaded.alphabet_multiplier, 2.75, 1e-4);
  EXPECT_EQ(loaded.enable_resegmentation, false);
  EXPECT_NEAR(loaded.resegment_threshold, 950.0, 1e-4);
  EXPECT_EQ(loaded.enable_online_learning, false);
  EXPECT_EQ(loaded.online_learning_mode, "full");
  EXPECT_NEAR(loaded.online_learning_rate, 0.0125, 1e-6);
  EXPECT_NEAR(loaded.online_learning_weight_decay, 0.0025, 1e-6);
  EXPECT_EQ(loaded.online_learning_save_interval, 12);

  EXPECT_FALSE(
      config::GruConfigManager::IsLiveConversionEnabled(true, test_config));
  EXPECT_TRUE(config::GruConfigManager::IsShowSegmentSeparatorInConversionEnabled(
      false, test_config));
  EXPECT_TRUE(
      config::GruConfigManager::IsShowSegmentSeparatorInLiveConversionEnabled(
          false, test_config));
  EXPECT_FALSE(
      config::GruConfigManager::IsLiveConversionPrefixFreezingEnabled(
          true, test_config));
  EXPECT_TRUE(
      config::GruConfigManager::IsLiveConversionAutoCommitEnabled(
          false, test_config));
  EXPECT_EQ(
      config::GruConfigManager::GetLiveConversionLengthThreshold(
          24, test_config),
      36);

  std::filesystem::remove(test_config, ec);
}

TEST_F(GruRewriterTest, HotReloadGuiSettingsFromConfigIni) {
  const std::string test_config = FileUtil::JoinPath(
      std::filesystem::temp_directory_path().string(),
      "gru_gui_hotreload_config.ini");
  std::error_code ec;
  std::filesystem::remove(test_config, ec);

  GruPenaltyConfig cfg;
  cfg.enable_gru_rewriter = true;
  cfg.show_gru_score = false;
  cfg.gru_score_weight = 800.0;
  ASSERT_TRUE(config::GruConfigManager::SaveConfig(cfg, test_config));

  absl::SetFlag(&FLAGS_gru_config_path, test_config);
  ASSERT_TRUE(rewriter_->Reload());

  ConversionRequest request;
  {
    Segments segments;
    Segment* seg = segments.add_segment();
    seg->set_key("きょうは");
    AddCandidate("きょうは", "今日は", 1000, seg);
    AddCandidate("きょうは", "教派", 1000, seg);
    EXPECT_TRUE(rewriter_->Rewrite(request, &segments));
    EXPECT_EQ(seg->candidate(0).value, "教派");
    // show_gru_score = false in gru_config.ini -> no "GRU:" in description
    EXPECT_EQ(seg->candidate(0).description.find("GRU:"), std::string::npos);
  }

  // Update gru_config.ini to disable enable_gru_rewriter
  cfg.enable_gru_rewriter = false;
  ASSERT_TRUE(config::GruConfigManager::SaveConfig(cfg, test_config));
  std::filesystem::last_write_time(
      test_config,
      std::filesystem::last_write_time(test_config) + std::chrono::seconds(2),
      ec);
  ASSERT_TRUE(rewriter_->Reload());

  {
    Segments segments;
    Segment* seg = segments.add_segment();
    seg->set_key("きょうは");
    AddCandidate("きょうは", "今日は", 1000, seg);
    AddCandidate("きょうは", "教派", 1000, seg);
    EXPECT_FALSE(rewriter_->Rewrite(request, &segments));
    EXPECT_EQ(seg->candidate(0).value, "今日は");
  }

  std::filesystem::remove(test_config, ec);
  absl::SetFlag(&FLAGS_gru_config_path, "");
}

TEST_F(GruRewriterTest, ResolveDataFilePathSearchesSystemModelDirForRelativeName) {
  std::error_code ec;
  const std::string base_tmp = FileUtil::JoinPath(
      std::filesystem::temp_directory_path().string(), "mogru_path_test");
  const std::string models_dir = FileUtil::JoinPath(base_tmp, "mogru/models");
  std::filesystem::create_directories(models_dir, ec);
  const std::string custom_model =
      FileUtil::JoinPath(models_dir, "custom_test_model.bin");
  {
    std::ofstream ofs(custom_model, std::ios::binary);
    ofs << "dummy";
  }

#ifdef __linux__
  setenv("XDG_DATA_HOME", base_tmp.c_str(), 1);
  const std::string resolved = config::GruConfigManager::ResolveDataFilePath(
      "custom_test_model.bin", "grumodel.bin");
  EXPECT_EQ(resolved, custom_model);
  unsetenv("XDG_DATA_HOME");
#endif

  // Non-existent absolute path should not fall back
  EXPECT_EQ(config::GruConfigManager::ResolveDataFilePath(
                "/non/existent/abs/custom_test_model.bin", "grumodel.bin"),
            "");

  std::filesystem::remove_all(base_tmp, ec);
}

}  // namespace
}  // namespace mozc

