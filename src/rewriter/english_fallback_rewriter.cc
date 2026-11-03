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

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/string_view.h"
#include "base/file_util.h"
#include "base/japanese_util.h"
#include "base/util.h"
#include "composer/composer.h"
#include "config/gru_config.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/segments.h"
#include "dictionary/english_dictionary.h"
#include "dictionary/pos_matcher.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"

namespace mozc {
namespace {

using config::GruConfigManager;

std::string ResolveDictionaryPath() {
  if (const char* env_path = std::getenv("MOZC_ENGLISH_DICTIONARY_PATH")) {
    if (std::filesystem::exists(env_path)) {
      return env_path;
    }
  }
  if (const char* test_srcdir = std::getenv("TEST_SRCDIR")) {
    const std::vector<std::string> test_candidates = {
        FileUtil::JoinPath(test_srcdir, "_main/src/data/english/google-10000-english.txt"),
        FileUtil::JoinPath(test_srcdir, "src/data/english/google-10000-english.txt"),
        FileUtil::JoinPath(test_srcdir, "_main/data/english/google-10000-english.txt"),
        FileUtil::JoinPath(test_srcdir, "data/english/google-10000-english.txt"),
    };
    for (const auto& c : test_candidates) {
      if (std::filesystem::exists(c)) {
        return c;
      }
    }
  }

  const std::vector<std::string> candidates = {
      "data/english/google-10000-english.txt",
      "src/data/english/google-10000-english.txt",
      "/home/haruhiro/repos/C++/MoGRU/src/data/english/google-10000-english.txt",
      "/usr/share/mogru/google-10000-english.txt",
      "models/google-10000-english.txt",
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c)) {
      return c;
    }
  }
  return "";
}

}  // namespace

EnglishFallbackRewriter::EnglishFallbackRewriter(
    const dictionary::PosMatcher& pos_matcher)
    : unknown_id_(pos_matcher.GetUnknownId()) {
  const std::string path = ResolveDictionaryPath();
  if (!path.empty()) {
    if (!dictionary_.LoadFromFile(path)) {
      LOG(WARNING) << "Failed to load English dictionary from: " << path;
    } else {
      VLOG(1) << "Loaded " << dictionary_.size()
              << " English words from: " << path;
    }
  } else {
    LOG(WARNING) << "English dictionary file google-10000-english.txt not found.";
  }
}

EnglishFallbackRewriter::EnglishFallbackRewriter(
    const dictionary::PosMatcher& pos_matcher,
    dictionary::EnglishDictionary dictionary)
    : unknown_id_(pos_matcher.GetUnknownId()),
      dictionary_(std::move(dictionary)) {}

int EnglishFallbackRewriter::capability(const ConversionRequest& request) const {
  const auto config = GruConfigManager::LoadConfig();
  if (config.enable_live_conversion_english_fallback) {
    return CONVERSION | PREDICTION | SUGGESTION;
  }
  return CONVERSION;
}

void EnglishFallbackRewriter::AddEnglishCandidates(
    absl::string_view word, absl::string_view key, Segment* segment) const {
  const std::string lower = absl::AsciiStrToLower(word);

  std::string capitalized = lower;
  Util::CapitalizeString(&capitalized);

  const std::string upper = absl::AsciiStrToUpper(word);
  const std::string fullwidth = japanese_util::HalfWidthToFullWidth(lower);

  struct CandidateVariant {
    std::string value;
    int32_t cost;
  };

  std::vector<CandidateVariant> variants;
  variants.push_back({lower, 6000});
  if (capitalized != lower) {
    variants.push_back({capitalized, 6100});
  }
  if (upper != lower && upper != capitalized) {
    variants.push_back({upper, 6200});
  }
  if (!fullwidth.empty() && fullwidth != lower) {
    variants.push_back({fullwidth, 6300});
  }

  for (const auto& variant : variants) {
    converter::Candidate* cand = segment->add_candidate();
    cand->key = std::string(key);
    cand->content_key = std::string(key);
    cand->value = variant.value;
    cand->content_value = variant.value;
    cand->cost = variant.cost;
    cand->wcost = variant.cost - 1000;
    cand->structure_cost = 1000;
    cand->lid = unknown_id_;
    cand->rid = unknown_id_;
    cand->converted_segment_count = 1;
    cand->attributes = converter::Attribute::NO_VARIANTS_EXPANSION;
  }
}

bool EnglishFallbackRewriter::Rewrite(const ConversionRequest& request,
                                      Segments* segments) const {
  const auto config = GruConfigManager::LoadConfig();
  if (!config.enable_english_fallback) {
    return false;
  }

  const bool is_conversion =
      (request.request_type() == ConversionRequest::CONVERSION);
  if (!is_conversion && !config.enable_live_conversion_english_fallback) {
    return false;
  }

  if (segments->conversion_segments_size() == 0) {
    return false;
  }
  if (request.composer().GetLength() == 0) {
    return false;
  }
  if (dictionary_.empty()) {
    return false;
  }

  // 1. Check if any segment exceeds the abnormal cost threshold.
  bool has_high_cost_segment = false;
  for (size_t i = 0; i < segments->conversion_segments_size(); ++i) {
    const Segment& seg = segments->conversion_segment(i);
    if (seg.candidates_size() > 0 &&
        (seg.candidate(0).cost >= config.english_fallback_cost_threshold ||
         seg.candidate(0).wcost >= config.english_fallback_cost_threshold)) {
      has_high_cost_segment = true;
      break;
    }
  }
  if (!has_high_cost_segment) {
    return false;
  }

  const composer::ComposerData& composer = request.composer();

  // Total character length across all conversion segments.
  size_t total_composed_len = 0;
  for (size_t i = 0; i < segments->conversion_segments_size(); ++i) {
    total_composed_len += Util::CharsLen(segments->conversion_segment(i).key());
  }

  bool modified = false;
  size_t current_seg_idx = 0;
  size_t current_preedit_offset = 0;

  while (current_seg_idx < segments->conversion_segments_size()) {
    const size_t remaining_preedit_len =
        total_composed_len - current_preedit_offset;
    const std::string remaining_raw = composer.GetRawSubString(
        current_preedit_offset, remaining_preedit_len);

    const size_t matched_raw_len = dictionary_.LongestPrefixMatch(
        remaining_raw, config.english_fallback_min_word_length);

    if (matched_raw_len == 0) {
      current_preedit_offset +=
          Util::CharsLen(segments->conversion_segment(current_seg_idx).key());
      current_seg_idx++;
      continue;
    }

    const std::string matched_word = remaining_raw.substr(0, matched_raw_len);

    // Find the number of preedit characters corresponding to matched_raw_len.
    size_t matched_preedit_chars = 0;
    bool found_preedit_alignment = false;
    for (size_t p = 1; p <= remaining_preedit_len; ++p) {
      const std::string raw_sub =
          composer.GetRawSubString(current_preedit_offset, p);
      if (raw_sub.length() == matched_raw_len) {
        if (raw_sub == matched_word) {
          matched_preedit_chars = p;
          found_preedit_alignment = true;
        }
        break;
      } else if (raw_sub.length() > matched_raw_len) {
        // Exceeded without exact match (e.g. boundary split within Romaji token)
        break;
      }
    }

    if (!found_preedit_alignment) {
      current_preedit_offset +=
          Util::CharsLen(segments->conversion_segment(current_seg_idx).key());
      current_seg_idx++;
      continue;
    }

    // Determine how many conversion segments are covered by matched_preedit_chars.
    size_t covered_segments = 0;
    size_t accumulated_chars = 0;
    bool contains_high_cost = false;

    for (size_t j = current_seg_idx; j < segments->conversion_segments_size();
         ++j) {
      const Segment& seg = segments->conversion_segment(j);
      accumulated_chars += Util::CharsLen(seg.key());
      covered_segments++;
      if (seg.candidates_size() > 0 &&
          (seg.candidate(0).cost >= config.english_fallback_cost_threshold ||
           seg.candidate(0).wcost >= config.english_fallback_cost_threshold)) {
        contains_high_cost = true;
      }
      if (accumulated_chars >= matched_preedit_chars) {
        break;
      }
    }

    // Fallback if the word covers at least one high-cost segment.
    if (accumulated_chars >= matched_preedit_chars && contains_high_cost &&
        covered_segments > 0) {
      const size_t last_seg_idx = current_seg_idx + covered_segments - 1;
      const size_t extra_chars = accumulated_chars - matched_preedit_chars;

      std::string merged_key;
      for (size_t j = 0; j < covered_segments - 1; ++j) {
        merged_key += segments->conversion_segment(current_seg_idx + j).key();
      }

      if (extra_chars == 0) {
        merged_key += segments->conversion_segment(last_seg_idx).key();

        Segment* seg = segments->mutable_conversion_segment(current_seg_idx);
        seg->set_key(merged_key);
        seg->clear_candidates();
        AddEnglishCandidates(matched_word, merged_key, seg);

        if (covered_segments > 1) {
          const size_t raw_idx =
              segments->history_segments_size() + current_seg_idx;
          segments->erase_segments(raw_idx + 1, covered_segments - 1);
        }
      } else {
        // The last segment covers the tail of the English word plus trailing
        // characters (e.g. "ryを" -> "ry" + "を").
        const Segment& last_seg = segments->conversion_segment(last_seg_idx);
        const size_t last_seg_len = Util::CharsLen(last_seg.key());
        const size_t used_chars = last_seg_len - extra_chars;

        const std::string used_key =
            std::string(Util::Utf8SubString(last_seg.key(), 0, used_chars));
        const std::string extra_key =
            std::string(Util::Utf8SubString(last_seg.key(), used_chars, extra_chars));
        merged_key += used_key;

        std::string extra_val = extra_key;
        uint16_t extra_lid = 0, extra_rid = 0;
        if (last_seg.candidates_size() > 0) {
          extra_lid = last_seg.candidate(0).lid;
          extra_rid = last_seg.candidate(0).rid;
          const size_t val_len = Util::CharsLen(last_seg.candidate(0).value);
          if (val_len >= extra_chars) {
            extra_val = std::string(Util::Utf8SubString(
                last_seg.candidate(0).value, val_len - extra_chars, extra_chars));
          }
        }

        const size_t raw_start_idx =
            segments->history_segments_size() + current_seg_idx;

        if (covered_segments == 1) {
          Segment* seg = segments->mutable_conversion_segment(current_seg_idx);
          seg->set_key(merged_key);
          seg->clear_candidates();
          AddEnglishCandidates(matched_word, merged_key, seg);

          Segment* extra_seg = segments->insert_segment(raw_start_idx + 1);
          extra_seg->set_key(extra_key);
          extra_seg->clear_candidates();
          auto* cand = extra_seg->add_candidate();
          cand->key = extra_key;
          cand->content_key = extra_key;
          cand->value = extra_val;
          cand->content_value = extra_val;
          cand->cost = 3000;
          cand->wcost = 2000;
          cand->structure_cost = 1000;
          cand->lid = extra_lid;
          cand->rid = extra_rid;
          cand->converted_segment_count = 1;
        } else {
          Segment* seg = segments->mutable_conversion_segment(current_seg_idx);
          seg->set_key(merged_key);
          seg->clear_candidates();
          AddEnglishCandidates(matched_word, merged_key, seg);

          if (covered_segments > 2) {
            segments->erase_segments(raw_start_idx + 1, covered_segments - 2);
          }

          Segment* extra_seg =
              segments->mutable_conversion_segment(current_seg_idx + 1);
          extra_seg->set_key(extra_key);
          extra_seg->clear_candidates();
          auto* cand = extra_seg->add_candidate();
          cand->key = extra_key;
          cand->content_key = extra_key;
          cand->value = extra_val;
          cand->content_value = extra_val;
          cand->cost = 3000;
          cand->wcost = 2000;
          cand->structure_cost = 1000;
          cand->lid = extra_lid;
          cand->rid = extra_rid;
          cand->converted_segment_count = 1;
        }
      }

      modified = true;
      current_preedit_offset += matched_preedit_chars;
      current_seg_idx++;
    } else {
      current_preedit_offset +=
          Util::CharsLen(segments->conversion_segment(current_seg_idx).key());
      current_seg_idx++;
    }
  }

  return modified;
}

}  // namespace mozc
