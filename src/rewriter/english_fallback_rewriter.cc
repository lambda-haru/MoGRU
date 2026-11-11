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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
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
#include "rewriter/embedded_english_dictionary_data.inc"
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

  // Check system/user data directories (including macOS Application Support,
  // application bundles, Linux /usr/share/mogru, etc.).
  const std::string default_path =
      GruConfigManager::GetDefaultDataFilePath("google-10000-english.txt");
  if (!default_path.empty() && std::filesystem::exists(default_path)) {
    return default_path;
  }

  const std::vector<std::string> candidates = {
      "data/english/google-10000-english.txt",
      "src/data/english/google-10000-english.txt",
      "models/google-10000-english.txt",
      "/usr/share/mogru/google-10000-english.txt",
      "/usr/share/mogru/models/google-10000-english.txt",
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c)) {
      return c;
    }
  }
  return "";
}

}  // namespace

void EnglishFallbackRewriter::InitBaseDictionary() {
  absl::MutexLock lock(mutex_);
  base_dictionary_.Clear();
  const std::string path = ResolveDictionaryPath();
  bool loaded = false;
  if (!path.empty()) {
    if (!base_dictionary_.LoadFromFile(path)) {
      LOG(WARNING) << "Failed to load English dictionary from: " << path;
    } else {
      VLOG(1) << "Loaded " << base_dictionary_.size()
              << " English words from: " << path;
      loaded = true;
    }
  }

  // Fallback to embedded dictionary data if external file is absent or failed to load.
  if (!loaded) {
    if (base_dictionary_.LoadFromString(kEmbeddedEnglishDictionaryData)) {
      VLOG(1) << "Loaded " << base_dictionary_.size()
              << " English words from embedded dictionary.";
    } else {
      LOG(ERROR) << "Failed to load embedded English dictionary data.";
    }
  }
}

void EnglishFallbackRewriter::MaybeUpdateUserDictionary(
    const GruPenaltyConfig& config) const {
  absl::MutexLock lock(mutex_);
  std::string file_path = user_dict_file_path_;
  if (file_path.empty()) {
    file_path = GruConfigManager::GetDefaultDataFilePath("user_english_words.txt");
    user_dict_file_path_ = file_path;
  }

  int64_t file_mtime = -1;
  if (!file_path.empty() && std::filesystem::exists(file_path)) {
    file_mtime = GruConfigManager::GetFileMtime(file_path);
  }

  if (loaded_user_words_ == config.user_english_words &&
      last_user_dict_file_mtime_ == file_mtime) {
    return;
  }

  dictionary::EnglishDictionary new_user_dict;
  for (const auto& word : config.user_english_words) {
    new_user_dict.AddWord(word);
  }

  if (file_mtime != -1 && !file_path.empty()) {
    new_user_dict.LoadFromFile(file_path);
  }

  user_dictionary_ = std::move(new_user_dict);
  loaded_user_words_ = config.user_english_words;
  last_user_dict_file_mtime_ = file_mtime;
  VLOG(1) << "Loaded " << user_dictionary_.size() << " user English words.";
}

bool EnglishFallbackRewriter::Reload() {
  if (!is_injected_dictionary_) {
    InitBaseDictionary();
  }
  {
    absl::MutexLock lock(mutex_);
    loaded_user_words_.clear();
    last_user_dict_file_mtime_ = -1;
  }
  const auto config = GruConfigManager::LoadConfig();
  MaybeUpdateUserDictionary(config);
  return true;
}

EnglishFallbackRewriter::EnglishFallbackRewriter(
    const dictionary::PosMatcher& pos_matcher)
    : unknown_id_(pos_matcher.GetUnknownId()) {
  InitBaseDictionary();
  MaybeUpdateUserDictionary(GruConfigManager::LoadConfig());
}

EnglishFallbackRewriter::EnglishFallbackRewriter(
    const dictionary::PosMatcher& pos_matcher,
    dictionary::EnglishDictionary dictionary)
    : unknown_id_(pos_matcher.GetUnknownId()),
      is_injected_dictionary_(true) {
  {
    absl::MutexLock lock(mutex_);
    base_dictionary_ = std::move(dictionary);
  }
  MaybeUpdateUserDictionary(GruConfigManager::LoadConfig());
}

int EnglishFallbackRewriter::capability(const ConversionRequest& request) const {
  if (request.config().preedit_method() == config::Config::KANA) {
    return NOT_AVAILABLE;
  }
  return CONVERSION;
}

void EnglishFallbackRewriter::AddEnglishCandidates(
    absl::string_view word, absl::string_view original_word,
    absl::string_view key, Segment* segment) const {
  const std::string raw = std::string(word);
  const std::string lower = absl::AsciiStrToLower(word);
  const std::string orig = std::string(original_word);

  std::vector<std::string> candidates;
  auto add_if_new = [&](const std::string& val) {
    if (val.empty()) return;
    if (std::find(candidates.begin(), candidates.end(), val) == candidates.end()) {
      candidates.push_back(val);
    }
  };

  add_if_new(raw);
  if (!orig.empty() && orig != raw) {
    add_if_new(orig);
  }
  if (!lower.empty() && lower != raw && lower != orig) {
    add_if_new(lower);
  }

  int32_t cost = 6000;
  for (const auto& val : candidates) {
    converter::Candidate* cand = segment->add_candidate();
    cand->key = std::string(key);
    cand->content_key = std::string(key);
    cand->value = val;
    cand->content_value = val;
    cand->cost = cost;
    cand->wcost = cost - 1000;
    cand->structure_cost = 1000;
    cand->lid = unknown_id_;
    cand->rid = unknown_id_;
    cand->converted_segment_count = 1;
    cand->attributes = converter::Attribute::NO_VARIANTS_EXPANSION;
    cost += 100;
  }
}

bool EnglishFallbackRewriter::ShouldTriggerFallback(
    const ConversionRequest& request, const Segments& segments,
    const GruPenaltyConfig& config) const {
  if (!config.enable_english_fallback) {
    return false;
  }

  if (request.request_type() != ConversionRequest::CONVERSION) {
    return false;
  }
  if (request.config().preedit_method() == config::Config::KANA) {
    return false;
  }

  const bool is_live_conversion =
      (request.options().composer_key_selection ==
       ConversionRequest::PREDICTION_KEY);
  if (is_live_conversion && !config.enable_live_conversion_english_fallback) {
    return false;
  }

  if (segments.conversion_segments_size() == 0) {
    return false;
  }
  if (request.composer().GetLength() == 0) {
    return false;
  }
  {
    absl::MutexLock lock(mutex_);
    if (base_dictionary_.empty() && user_dictionary_.empty()) {
      return false;
    }
  }

  for (size_t i = 0; i < segments.conversion_segments_size(); ++i) {
    const Segment& seg = segments.conversion_segment(i);
    if (seg.candidates_size() > 0 &&
        (seg.candidate(0).cost >= config.english_fallback_cost_threshold ||
         seg.candidate(0).wcost >= config.english_fallback_cost_threshold)) {
      return true;
    }
  }
  return false;
}

bool EnglishFallbackRewriter::MatchWordAtOffset(
    const composer::ComposerData& composer, size_t offset,
    size_t total_composed_len, size_t min_word_length,
    std::string* matched_word, std::string* matched_original_word,
    size_t* matched_preedit_chars) const {
  if (offset >= total_composed_len) {
    return false;
  }
  const size_t remaining_preedit_len = total_composed_len - offset;
  const std::string remaining_raw =
      composer.GetRawSubString(offset, remaining_preedit_len);

  size_t matched_raw_len = 0;
  std::string orig_word;
  {
    absl::MutexLock lock(mutex_);
    std::string user_orig;
    const size_t user_len = user_dictionary_.LongestPrefixMatch(
        remaining_raw, min_word_length, &user_orig);
    std::string base_orig;
    const size_t base_len = base_dictionary_.LongestPrefixMatch(
        remaining_raw, min_word_length, &base_orig);
    if (user_len >= base_len && user_len > 0) {
      matched_raw_len = user_len;
      orig_word = user_orig;
    } else if (base_len > 0) {
      matched_raw_len = base_len;
      orig_word = base_orig;
    }
  }
  if (matched_raw_len == 0) {
    return false;
  }

  const std::string word = remaining_raw.substr(0, matched_raw_len);

  // Find the number of preedit characters corresponding to matched_raw_len.
  for (size_t p = 1; p <= remaining_preedit_len; ++p) {
    const std::string raw_sub = composer.GetRawSubString(offset, p);
    if (raw_sub.length() == matched_raw_len) {
      if (raw_sub == word) {
        *matched_word = word;
        if (matched_original_word != nullptr) {
          *matched_original_word = orig_word;
        }
        *matched_preedit_chars = p;
        return true;
      }
      break;
    } else if (raw_sub.length() > matched_raw_len) {
      // Exceeded without exact match (e.g. boundary split within Romaji token)
      break;
    }
  }
  return false;
}

std::optional<RewriterInterface::ResizeSegmentsRequest>
EnglishFallbackRewriter::CheckResizeSegmentsRequest(
    const ConversionRequest& request, const Segments& segments) const {
  if (segments.resized()) {
    return std::nullopt;
  }

  const auto config = GruConfigManager::LoadConfig();
  MaybeUpdateUserDictionary(config);
  if (!ShouldTriggerFallback(request, segments, config)) {
    return std::nullopt;
  }

  const composer::ComposerData& composer = request.composer();

  size_t total_composed_len = 0;
  for (size_t i = 0; i < segments.conversion_segments_size(); ++i) {
    total_composed_len += Util::CharsLen(segments.conversion_segment(i).key());
  }

  size_t current_seg_idx = 0;
  size_t current_preedit_offset = 0;
  size_t skip_until_preedit_offset = 0;

  while (current_seg_idx < segments.conversion_segments_size()) {
    const Segment& cur_seg = segments.conversion_segment(current_seg_idx);
    const size_t cur_seg_len = Util::CharsLen(cur_seg.key());

    if (cur_seg.segment_type() != Segment::FREE) {
      for (size_t p = current_preedit_offset;
           p < current_preedit_offset + cur_seg_len; ++p) {
        if (p < skip_until_preedit_offset) {
          continue;
        }
        std::string word;
        std::string orig_word;
        size_t word_preedit_chars = 0;
        if (MatchWordAtOffset(composer, p, total_composed_len,
                              config.english_fallback_min_word_length, &word,
                              &orig_word, &word_preedit_chars)) {
          if (p != current_preedit_offset ||
              word_preedit_chars != cur_seg_len) {
            skip_until_preedit_offset =
                std::max(skip_until_preedit_offset, p + word_preedit_chars);
          } else {
            break;
          }
        }
      }
      current_preedit_offset += cur_seg_len;
      current_seg_idx++;
      continue;
    }

    if (current_preedit_offset < skip_until_preedit_offset) {
      current_preedit_offset += cur_seg_len;
      current_seg_idx++;
      continue;
    }

    std::string matched_word;
    std::string matched_original_word;
    size_t matched_preedit_chars = 0;
    if (!MatchWordAtOffset(composer, current_preedit_offset, total_composed_len,
                           config.english_fallback_min_word_length,
                           &matched_word, &matched_original_word,
                           &matched_preedit_chars)) {
      current_preedit_offset += cur_seg_len;
      current_seg_idx++;
      continue;
    }

    size_t covered_segments = 0;
    size_t accumulated_chars = 0;
    bool contains_high_cost = false;
    bool all_free = true;

    for (size_t j = current_seg_idx; j < segments.conversion_segments_size();
         ++j) {
      const Segment& seg = segments.conversion_segment(j);
      accumulated_chars += Util::CharsLen(seg.key());
      covered_segments++;
      if (seg.segment_type() != Segment::FREE) {
        all_free = false;
      }
      if (seg.candidates_size() > 0 &&
          (seg.candidate(0).cost >= config.english_fallback_cost_threshold ||
           seg.candidate(0).wcost >= config.english_fallback_cost_threshold)) {
        contains_high_cost = true;
      }
      if (accumulated_chars >= matched_preedit_chars) {
        break;
      }
    }

    if (accumulated_chars >= matched_preedit_chars && covered_segments > 0) {
      const size_t extra_chars = accumulated_chars - matched_preedit_chars;
      if (!all_free) {
        skip_until_preedit_offset = std::max(
            skip_until_preedit_offset,
            current_preedit_offset + matched_preedit_chars);
      } else if (contains_high_cost) {
        if ((covered_segments > 1 || extra_chars > 0) &&
            matched_preedit_chars <= std::numeric_limits<uint8_t>::max()) {
          ResizeSegmentsRequest resize_request;
          resize_request.segment_index = current_seg_idx;
          resize_request.segment_sizes[0] =
              static_cast<uint8_t>(matched_preedit_chars);
          return resize_request;
        }
        current_preedit_offset += matched_preedit_chars;
        current_seg_idx++;
        continue;
      }
    }

    current_preedit_offset += cur_seg_len;
    current_seg_idx++;
  }

  return std::nullopt;
}

bool EnglishFallbackRewriter::Rewrite(const ConversionRequest& request,
                                      Segments* segments) const {
  const auto config = GruConfigManager::LoadConfig();
  MaybeUpdateUserDictionary(config);
  if (!ShouldTriggerFallback(request, *segments, config)) {
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
  size_t skip_until_preedit_offset = 0;

  while (current_seg_idx < segments->conversion_segments_size()) {
    const Segment& cur_seg = segments->conversion_segment(current_seg_idx);
    const size_t cur_seg_len = Util::CharsLen(cur_seg.key());

    // If the segment boundary was fixed (either by user resize or by
    // CheckResizeSegmentsRequest), never split or merge it. Only allow
    // replacing its candidate if the segment's exact boundary matches the
    // English word (and it is FIXED_BOUNDARY, not FIXED_VALUE).
    if (cur_seg.segment_type() != Segment::FREE) {
      bool exact_segment_match = false;
      for (size_t p = current_preedit_offset;
           p < current_preedit_offset + cur_seg_len; ++p) {
        if (p < skip_until_preedit_offset) {
          continue;
        }
        std::string word;
        std::string orig_word;
        size_t word_preedit_chars = 0;
        if (MatchWordAtOffset(composer, p, total_composed_len,
                              config.english_fallback_min_word_length, &word,
                              &orig_word, &word_preedit_chars)) {
          if (p == current_preedit_offset &&
              word_preedit_chars == cur_seg_len &&
              cur_seg.segment_type() == Segment::FIXED_BOUNDARY) {
            exact_segment_match = true;
            break;
          }
          skip_until_preedit_offset =
              std::max(skip_until_preedit_offset, p + word_preedit_chars);
        }
      }
      if (!exact_segment_match) {
        current_preedit_offset += cur_seg_len;
        current_seg_idx++;
        continue;
      }
    }

    if (current_preedit_offset < skip_until_preedit_offset) {
      current_preedit_offset += cur_seg_len;
      current_seg_idx++;
      continue;
    }

    std::string matched_word;
    std::string matched_original_word;
    size_t matched_preedit_chars = 0;
    if (!MatchWordAtOffset(composer, current_preedit_offset, total_composed_len,
                           config.english_fallback_min_word_length,
                           &matched_word, &matched_original_word,
                           &matched_preedit_chars)) {
      current_preedit_offset += cur_seg_len;
      current_seg_idx++;
      continue;
    }

    // Determine how many conversion segments are covered by matched_preedit_chars.
    size_t covered_segments = 0;
    size_t accumulated_chars = 0;
    bool contains_high_cost = false;
    bool all_free = true;

    for (size_t j = current_seg_idx; j < segments->conversion_segments_size();
         ++j) {
      const Segment& seg = segments->conversion_segment(j);
      accumulated_chars += Util::CharsLen(seg.key());
      covered_segments++;
      if (seg.segment_type() != Segment::FREE) {
        all_free = false;
      }
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
    if (accumulated_chars >= matched_preedit_chars && covered_segments > 0) {
      const size_t last_seg_idx = current_seg_idx + covered_segments - 1;
      const size_t extra_chars = accumulated_chars - matched_preedit_chars;

      // If a boundary change (merge or split) is required, all covered segments
      // must be Segment::FREE. Do not override user-resized boundaries.
      if ((covered_segments > 1 || extra_chars > 0) && !all_free) {
        skip_until_preedit_offset = std::max(
            skip_until_preedit_offset,
            current_preedit_offset + matched_preedit_chars);
        current_preedit_offset += cur_seg_len;
        current_seg_idx++;
        continue;
      }

      if (contains_high_cost) {
        std::string merged_key;
        for (size_t j = 0; j < covered_segments - 1; ++j) {
          merged_key += segments->conversion_segment(current_seg_idx + j).key();
        }

        if (extra_chars == 0) {
          merged_key += segments->conversion_segment(last_seg_idx).key();

          Segment* seg = segments->mutable_conversion_segment(current_seg_idx);
          seg->set_key(merged_key);
          seg->clear_candidates();
          AddEnglishCandidates(matched_word, matched_original_word, merged_key, seg);

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
          const std::string extra_key = std::string(
              Util::Utf8SubString(last_seg.key(), used_chars, extra_chars));
          merged_key += used_key;

          std::string extra_val = extra_key;
          uint16_t extra_lid = 0, extra_rid = 0;
          if (last_seg.candidates_size() > 0) {
            extra_lid = last_seg.candidate(0).lid;
            extra_rid = last_seg.candidate(0).rid;
            const size_t val_len = Util::CharsLen(last_seg.candidate(0).value);
            if (val_len >= extra_chars) {
              extra_val = std::string(Util::Utf8SubString(
                  last_seg.candidate(0).value, val_len - extra_chars,
                  extra_chars));
            }
          }

          const size_t raw_start_idx =
              segments->history_segments_size() + current_seg_idx;

          if (covered_segments == 1) {
            Segment* seg =
                segments->mutable_conversion_segment(current_seg_idx);
            seg->set_key(merged_key);
            seg->clear_candidates();
            AddEnglishCandidates(matched_word, matched_original_word, merged_key, seg);

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
            Segment* seg =
                segments->mutable_conversion_segment(current_seg_idx);
            seg->set_key(merged_key);
            seg->clear_candidates();
            AddEnglishCandidates(matched_word, matched_original_word, merged_key, seg);

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
        continue;
      }
    }

    current_preedit_offset += cur_seg_len;
    current_seg_idx++;
  }

  return modified;
}

}  // namespace mozc
