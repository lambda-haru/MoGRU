// Copyright 2010-2021, Google Inc.
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
//     * Neither the name of Google Inc. nor the names of its
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

#include <memory>
#include <string>

#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "composer/composer.h"
#include "composer/table.h"
#include "converter/converter_interface.h"
#include "converter/segments.h"
#include "engine/engine.h"
#include "engine/engine_factory.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

namespace mozc {
namespace {

ConversionRequest ConvReq(absl::string_view key) {
  composer::Composer composer;
  composer.SetPreeditTextForTestOnly(key);
  return ConversionRequestBuilder().SetComposer(composer).Build();
}

class ConverterRegressionTest : public testing::TestWithTempUserProfile {};

TEST_F(ConverterRegressionTest, QueryOfDeathTest) {
  std::unique_ptr<Engine> engine = EngineFactory::Create().value();
  std::shared_ptr<const ConverterInterface> converter = engine->GetConverter();

  CHECK(converter);
  {
    Segments segments;
    EXPECT_TRUE(
        converter->StartConversion(ConvReq("りゅきゅけmぽ"), &segments));
  }
  {
    Segments segments;
    EXPECT_TRUE(converter->StartConversion(ConvReq("5.1,||t:1"), &segments));
  }
  {
    Segments segments;
    // Converter returns false, but not crash.
    EXPECT_FALSE(converter->StartConversion(ConvReq(""), &segments));
  }
  {
    Segments segments;
    const ConversionRequest conv_request;
    // Converter returns false, but not crash.
    EXPECT_FALSE(converter->StartConversion(conv_request, &segments));
  }
}

TEST_F(ConverterRegressionTest, Regression3323108) {
  std::unique_ptr<Engine> engine = EngineFactory::Create().value();
  std::shared_ptr<const ConverterInterface> converter = engine->GetConverter();
  Segments segments;

  EXPECT_TRUE(
      converter->StartConversion(ConvReq("ここではきものをぬぐ"), &segments));
  EXPECT_EQ(segments.conversion_segments_size(), 3);
  const ConversionRequest default_request;
  EXPECT_TRUE(converter->ResizeSegment(&segments, default_request, 1, 2));
  EXPECT_EQ(segments.conversion_segments_size(), 2);
  EXPECT_EQ(segments.conversion_segment(1).key(), "きものをぬぐ");
}

TEST_F(ConverterRegressionTest, PunctuationTest) {
  std::unique_ptr<Engine> engine = EngineFactory::Create().value();
  std::shared_ptr<const ConverterInterface> converter = engine->GetConverter();

  // Test 1: Japanese '、' and '。'
  {
    Segments segments;
    EXPECT_TRUE(converter->StartConversion(ConvReq("きょうは、よいてんきですね。"), &segments));
    ASSERT_EQ(segments.conversion_segments_size(), 4);
    EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "今日は");
    EXPECT_EQ(segments.conversion_segment(0).candidate(0).cost, 5549);
    EXPECT_EQ(segments.conversion_segment(1).candidate(0).value, "、");
    EXPECT_EQ(segments.conversion_segment(2).candidate(0).value, "良い天気ですね");
    EXPECT_EQ(segments.conversion_segment(2).candidate(0).cost, 11632);
    EXPECT_EQ(segments.conversion_segment(3).candidate(0).value, "。");
  }

  // Test 2: Fullwidth '，' and '．' directly in input
  {
    Segments segments;
    EXPECT_TRUE(converter->StartConversion(ConvReq("きょうは，よいてんきですね．"), &segments));
    ASSERT_EQ(segments.conversion_segments_size(), 4);
    EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "今日は");
    EXPECT_EQ(segments.conversion_segment(0).candidate(0).cost, 5549);
    EXPECT_EQ(segments.conversion_segment(1).candidate(0).value, "，");
    EXPECT_EQ(segments.conversion_segment(2).candidate(0).value, "良い天気ですね");
    EXPECT_EQ(segments.conversion_segment(2).candidate(0).cost, 11632);
    EXPECT_EQ(segments.conversion_segment(3).candidate(0).value, "．");
  }

  // Test 3: ASCII ',' and '.' directly in input
  {
    Segments segments;
    EXPECT_TRUE(converter->StartConversion(ConvReq("きょうは,よいてんきですね."), &segments));
    ASSERT_EQ(segments.conversion_segments_size(), 4);
    EXPECT_EQ(segments.conversion_segment(0).candidate(0).value, "今日は");
    EXPECT_EQ(segments.conversion_segment(0).candidate(0).cost, 5549);
    EXPECT_EQ(segments.conversion_segment(1).candidate(0).value, "，");
    EXPECT_EQ(segments.conversion_segment(2).candidate(0).value, "良い天気ですね");
    EXPECT_EQ(segments.conversion_segment(2).candidate(0).cost, 11632);
    EXPECT_EQ(segments.conversion_segment(3).candidate(0).value, "．");
  }
}

TEST_F(ConverterRegressionTest, AtomicMultiCharRomajiChunkNotSplitOnOss) {
  std::unique_ptr<Engine> engine = EngineFactory::Create().value();
  std::shared_ptr<const ConverterInterface> converter = engine->GetConverter();

  auto table = std::make_shared<composer::Table>();
  commands::Request client_request;
  config::Config default_config;
  ASSERT_TRUE(
      table->InitializeWithRequestAndConfig(client_request, default_config));

  auto type_raw = [&](absl::string_view raw) {
    composer::Composer composer(table, client_request, default_config);
    for (char c : raw) {
      commands::KeyEvent key;
      key.set_key_code(c);
      composer.InsertCharacterKeyEvent(key);
    }
    return composer;
  };

  // Multi-char Romaji entries ("gwo" -> "ぐぉ", "swo" -> "すぉ", "two" -> "とぉ")
  // must stay together in a single segment rather than being split as "武具/ぉ".
  for (const char* raw : {"bugwo", "watashinobugwo", "bagwo", "debugwo"}) {
    composer::Composer composer = type_raw(raw);
    const ConversionRequest req =
        ConversionRequestBuilder()
            .SetComposer(composer)
            .SetRequestType(ConversionRequest::CONVERSION)
            .Build();
    Segments segments;
    ASSERT_TRUE(converter->StartConversion(req, &segments)) << raw;
    bool has_atomic_gwo = false;
    for (const Segment& seg : segments.conversion_segments()) {
      EXPECT_NE(seg.key(), "ぉ") << raw << ": " << segments.DebugString();
      if (seg.key().find("ぐぉ") != absl::string_view::npos) {
        has_atomic_gwo = true;
      }
    }
    EXPECT_TRUE(has_atomic_gwo) << raw << ": " << segments.DebugString();
  }

  // Normal Japanese words (including double consonants like "tsukatta" where
  // "っ" and "た" belong to two sequential table entries within one chunk)
  // convert normally.
  struct ControlCase {
    const char* raw;
    const char* expected_surface;
  };
  const ControlCase controls[] = {
      {"buguwo", "武具を"},
      {"fonto", "フォント"},
      {"hwonto", "フォント"},
      {"fuxonto", "フォント"},
      {"kyouwo", "今日を"},
      {"syuwo", "種を"},
      {"tsukatta", "使った"},
  };
  for (const ControlCase& tc : controls) {
    composer::Composer composer = type_raw(tc.raw);
    const ConversionRequest req =
        ConversionRequestBuilder()
            .SetComposer(composer)
            .SetRequestType(ConversionRequest::CONVERSION)
            .Build();
    Segments segments;
    ASSERT_TRUE(converter->StartConversion(req, &segments)) << tc.raw;
    std::string surface;
    for (const Segment& seg : segments.conversion_segments()) {
      ASSERT_GT(seg.candidates_size(), 0) << tc.raw;
      surface.append(seg.candidate(0).value);
    }
    EXPECT_EQ(surface, tc.expected_surface) << tc.raw;
  }
}

}  // namespace
}  // namespace mozc
