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
#include "absl/synchronization/mutex.h"
#include "base/file_util.h"
#include "base/japanese_util.h"
#include "base/util.h"
#include "composer/composer.h"
#include "config/gru_config.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/converter_util.h"
#include "converter/immutable_converter_interface.h"
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

void LoadBaseDictionary(dictionary::EnglishDictionary* base_dict) {
  base_dict->Clear();
  const std::string path = ResolveDictionaryPath();
  bool loaded = false;
  if (!path.empty()) {
    if (!base_dict->LoadFromFile(path)) {
      LOG(WARNING) << "Failed to load English dictionary from: " << path;
    } else {
      VLOG(1) << "Loaded " << base_dict->size()
              << " English words from: " << path;
      loaded = true;
    }
  }

  // Fallback to embedded dictionary data if external file is absent or failed to load.
  if (!loaded) {
    if (base_dict->LoadFromString(kEmbeddedEnglishDictionaryData)) {
      VLOG(1) << "Loaded " << base_dict->size()
              << " English words from embedded dictionary.";
    } else {
      LOG(ERROR) << "Failed to load embedded English dictionary data.";
    }
  }
}

struct DictionaryState {
  absl::Mutex mutex;
  bool base_initialized = false;
  dictionary::EnglishDictionary base_dict ABSL_GUARDED_BY(mutex);
  dictionary::EnglishDictionary user_dict ABSL_GUARDED_BY(mutex);
  std::vector<std::string> loaded_user_words ABSL_GUARDED_BY(mutex);
  int64_t last_user_dict_mtime ABSL_GUARDED_BY(mutex) = -1;
  std::string user_dict_file_path ABSL_GUARDED_BY(mutex);
};

DictionaryState* GetSharedDictionaryState() {
  static auto* kState = new DictionaryState();
  return kState;
}

void UpdateUserDictionaryLocked(
    const GruPenaltyConfig& config,
    dictionary::EnglishDictionary* user_dict,
    std::vector<std::string>* loaded_user_words,
    int64_t* last_mtime,
    std::string* file_path) {
  if (file_path->empty()) {
    *file_path = GruConfigManager::GetDefaultDataFilePath("user_english_words.txt");
  }

  int64_t file_mtime = -1;
  if (!file_path->empty() && std::filesystem::exists(*file_path)) {
    file_mtime = GruConfigManager::GetFileMtime(*file_path);
  }

  if (*loaded_user_words == config.user_english_words &&
      *last_mtime == file_mtime) {
    return;
  }

  dictionary::EnglishDictionary new_user_dict;
  for (const auto& word : config.user_english_words) {
    new_user_dict.AddWord(word);
  }

  if (file_mtime != -1 && !file_path->empty()) {
    new_user_dict.LoadFromFile(*file_path);
  }

  *user_dict = std::move(new_user_dict);
  *loaded_user_words = config.user_english_words;
  *last_mtime = file_mtime;
  VLOG(1) << "Loaded " << user_dict->size() << " user English words.";
}

void EnsureSharedDictionaryUpToDate(const GruPenaltyConfig& config) {
  DictionaryState* state = GetSharedDictionaryState();
  absl::MutexLock lock(state->mutex);
  if (!state->base_initialized) {
    LoadBaseDictionary(&state->base_dict);
    state->base_initialized = true;
  }
  UpdateUserDictionaryLocked(config, &state->user_dict,
                             &state->loaded_user_words,
                             &state->last_user_dict_mtime,
                             &state->user_dict_file_path);
}

size_t MatchLongestWordInDictionaries(
    const dictionary::EnglishDictionary& base_dict,
    const dictionary::EnglishDictionary& user_dict,
    absl::string_view text, size_t min_word_length,
    std::string* orig_word) {
  std::string user_orig;
  const size_t user_len =
      user_dict.LongestPrefixMatch(text, min_word_length, &user_orig);
  std::string base_orig;
  const size_t base_len =
      base_dict.LongestPrefixMatch(text, min_word_length, &base_orig);
  if (user_len >= base_len && user_len > 0) {
    *orig_word = std::move(user_orig);
    return user_len;
  }
  if (base_len > 0) {
    *orig_word = std::move(base_orig);
    return base_len;
  }
  orig_word->clear();
  return 0;
}

bool IsExcludedFallbackPunctuation(char32_t codepoint) {
  // Include halfwidth/ASCII ',' and '.' (and halfwidth '､'/'｡') because
  // Composer::GetQueryForConversion() normalizes fullwidth '，'/'．' via
  // FullWidthAsciiToHalfWidthAscii() before segmentation/conversion.
  return codepoint == 0x3001 ||  // '、' (IDEOGRAPHIC COMMA)
         codepoint == 0x3002 ||  // '。' (IDEOGRAPHIC FULL STOP)
         codepoint == 0xFF0C ||  // '，' (FULLWIDTH COMMA)
         codepoint == 0xFF0E ||  // '．' (FULLWIDTH FULL STOP)
         codepoint == 0x002C ||  // ','  (COMMA, normalized from '，')
         codepoint == 0x002E ||  // '.'  (FULL STOP, normalized from '．')
         codepoint == 0xFF64 ||  // '､'  (HALFWIDTH IDEOGRAPHIC COMMA)
         codepoint == 0xFF61;    // '｡'  (HALFWIDTH IDEOGRAPHIC FULL STOP)
}

// Classification of how a segment overlaps ASCII-alphabet-driven Romaji input.
struct AlphaInputSegmentKind {
  bool has_symbol_from_alpha = false;
  bool has_unresolved_romaji_alpha = false;
};

// Returns true if `span` is a Romaji chunk formed by an accidental "ww" +
// vowel boundary (e.g. "wwo" -> "っを" in "followwo"). Native Japanese never
// geminates 'w' before particles like "を", and Mozc assigns low Viterbi cost
// to "っ" + "を", so cost alone misses cross-boundary English words ending in 'w'.
bool IsDoubleWBoundaryChunk(const composer::ChunkSpan& span) {
  if (!span.is_romaji || span.raw_len != 3) {
    return false;
  }
  const std::string lower_raw = absl::AsciiStrToLower(span.raw);
  return lower_raw == "wwo" || lower_raw == "wwa" || lower_raw == "wwi" ||
         lower_raw == "wwu" || lower_raw == "wwe";
}

// Classifies whether `seg` covers:
// 1. A symbol/punctuation character (e.g. ";" from custom Romaji "b" -> ";")
//    excluding Japanese sentence punctuation, or
// 2. An unresolved Romaji Latin character (notably 'w', which OSS dictionaries
//    register with wcost=1 as slang) or a "ww"+vowel boundary chunk ("っを").
AlphaInputSegmentKind ClassifyAlphaInputSegment(
    const Segment& seg, size_t seg_preedit_start, size_t seg_preedit_end,
    const std::vector<composer::ChunkSpan>& spans) {
  AlphaInputSegmentKind kind;
  if (seg_preedit_end <= seg_preedit_start) {
    return kind;
  }
  const std::u32string key_u32 = Util::Utf8ToUtf32(seg.key());
  if (key_u32.size() != seg_preedit_end - seg_preedit_start) {
    return kind;
  }
  for (const auto& span : spans) {
    const size_t span_end = span.preedit_start + span.preedit_len;
    if (span_end <= seg_preedit_start) {
      continue;
    }
    if (span.preedit_start >= seg_preedit_end) {
      break;
    }
    bool has_alpha_raw = false;
    for (const char ch : span.raw) {
      if (absl::ascii_isalpha(static_cast<unsigned char>(ch))) {
        has_alpha_raw = true;
        break;
      }
    }
    if (!has_alpha_raw) {
      continue;
    }
    const size_t overlap_start =
        std::max(seg_preedit_start, span.preedit_start) - seg_preedit_start;
    const size_t overlap_end =
        std::min(seg_preedit_end, span_end) - seg_preedit_start;
    if (overlap_start < overlap_end && IsDoubleWBoundaryChunk(span)) {
      kind.has_unresolved_romaji_alpha = true;
    }
    for (size_t k = overlap_start; k < overlap_end; ++k) {
      const char32_t codepoint = key_u32[k];
      if (span.is_romaji &&
          Util::GetScriptType(codepoint) == Util::ALPHABET) {
        kind.has_unresolved_romaji_alpha = true;
      } else if (!IsExcludedFallbackPunctuation(codepoint) &&
                 Util::GetScriptType(codepoint) == Util::UNKNOWN_SCRIPT) {
        kind.has_symbol_from_alpha = true;
      }
    }
  }
  return kind;
}

constexpr int32_t kProtectedHealthyWCostThreshold = 4000;
constexpr int32_t kProtectedParticleStrictWCostThreshold = 500;
constexpr int32_t kLowPathCostParticleThreshold = 5000;
constexpr int32_t kMaxParticleWithSuffixPathCost = 8500;
constexpr int32_t kMinSymbolSpanTotalWCost = 10000;

bool IsHighWCostSegment(const Segment& seg, double cost_threshold) {
  if (seg.candidates_size() == 0) {
    return false;
  }
  if (seg.candidate(0).wcost >= cost_threshold) {
    return true;
  }
  // If EnglishFallbackRewriter already replaced a single high-wcost segment's
  // top candidate with an English fallback (e.g. "とぉ" -> "two" in "gitwo"),
  // preserve its high-wcost eligibility for cross-boundary matching so an
  // overlapping earlier word (e.g. "git" + "wo") can still trigger separation.
  constexpr uint32_t kFallbackMask =
      converter::Attribute::ENGLISH | converter::Attribute::DISABLE_RESCORING;
  if ((seg.candidate(0).attributes & kFallbackMask) == kFallbackMask) {
    for (size_t i = 1; i < seg.candidates_size(); ++i) {
      if (seg.candidate(i).wcost >= cost_threshold) {
        return true;
      }
    }
  }
  return false;
}

bool IsSegmentFallbackEligible(const Segment& seg, size_t seg_preedit_start,
                               size_t seg_preedit_end,
                               const std::vector<composer::ChunkSpan>& spans,
                               double cost_threshold) {
  if (IsHighWCostSegment(seg, cost_threshold)) {
    return true;
  }
  const AlphaInputSegmentKind kind = ClassifyAlphaInputSegment(
      seg, seg_preedit_start, seg_preedit_end, spans);
  return kind.has_unresolved_romaji_alpha || kind.has_symbol_from_alpha;
}

bool BoundarySplitsSingleRomajiChunk(
    size_t boundary_preedit_pos,
    const std::vector<composer::ChunkSpan>& spans) {
  for (const auto& span : spans) {
    if (!span.is_romaji || span.preedit_len <= 1) {
      continue;
    }
    const size_t span_end = span.preedit_start + span.preedit_len;
    if (span.preedit_start < boundary_preedit_pos &&
        boundary_preedit_pos < span_end) {
      return true;
    }
  }
  return false;
}

struct SegmentEligibilityMetrics {
  size_t preedit_start = 0;
  size_t preedit_len = 0;
  int32_t cost = 0;
  int32_t wcost = 0;
  bool is_free = false;
  bool is_high_wcost = false;
  bool is_unresolved_romaji_alpha = false;
  bool is_symbol_eligible = false;

  size_t preedit_end() const { return preedit_start + preedit_len; }
  bool is_eligible() const {
    return is_high_wcost || is_unresolved_romaji_alpha || is_symbol_eligible;
  }
  bool has_strong_anomaly() const {
    return is_high_wcost || is_unresolved_romaji_alpha;
  }
};

SegmentEligibilityMetrics BuildSegmentMetrics(
    const Segment& seg, size_t seg_preedit_start,
    const std::vector<composer::ChunkSpan>& spans, double cost_threshold) {
  const size_t len = Util::CharsLen(seg.key());
  SegmentEligibilityMetrics m;
  m.preedit_start = seg_preedit_start;
  m.preedit_len = len;
  if (seg.candidates_size() > 0) {
    m.cost = seg.candidate(0).cost;
    m.wcost = seg.candidate(0).wcost;
  }
  m.is_free = (seg.segment_type() == Segment::FREE);
  m.is_high_wcost = IsHighWCostSegment(seg, cost_threshold);
  const AlphaInputSegmentKind kind = ClassifyAlphaInputSegment(
      seg, seg_preedit_start, seg_preedit_start + len, spans);
  m.is_unresolved_romaji_alpha = kind.has_unresolved_romaji_alpha;
  m.is_symbol_eligible = kind.has_symbol_from_alpha;
  return m;
}

bool CanCoverSegmentSpan(const std::vector<SegmentEligibilityMetrics>& covered,
                         bool has_preceding_segment, size_t leading_chars,
                         size_t extra_chars,
                         const std::vector<composer::ChunkSpan>& spans) {
  if (covered.empty()) {
    return false;
  }
  bool any_eligible = false;
  bool any_strong_anomaly = false;
  int64_t total_wcost = 0;
  for (const auto& s : covered) {
    if (s.is_eligible()) {
      any_eligible = true;
    }
    if (s.has_strong_anomaly()) {
      any_strong_anomaly = true;
    }
    total_wcost += s.wcost;
  }
  if (!any_eligible) {
    return false;
  }
  // Never slice mid-segment into a healthy (non-eligible) starting segment.
  if (leading_chars > 0 && !covered.front().is_eligible()) {
    return false;
  }
  if (covered.size() == 1) {
    return true;
  }

  const SegmentEligibilityMetrics& last_seg = covered.back();
  const bool romaji_chunk_tail_split =
      (extra_chars > 0 &&
       BoundarySplitsSingleRomajiChunk(last_seg.preedit_start, spans));
  // When a multi-segment match truncates the trailing segment (extra_chars > 0),
  // require that the trailing segment itself is eligible or splits inside a
  // single Romaji CharChunk bridging into it (e.g. "be" -> "ｂう" + "に").
  if (extra_chars > 0 && !last_seg.is_eligible() && !romaji_chunk_tail_split) {
    return false;
  }

  // Multi-segment fallbacks triggered purely by low-cost custom Romaji symbol
  // mappings must still accumulate substantial total wcost (blocking ";あん",
  // "か;", "まけ;" while keeping "ぎてゅ;").
  if (!any_strong_anomaly && total_wcost < kMinSymbolSpanTotalWCost) {
    return false;
  }

  // Prevent a 2-segment merge from rewriting a smoothly connected low-wcost
  // single-kana Japanese particle/word following an earlier segment (e.g.,
  // "これ|と|ｐ", "私|の|ｗ", "これ|さ|ｗ") just because 1 trailing character is
  // eligible, while still allowing standalone "と|p" ("top") and multi-char
  // prefixes like "あん|d" ("and") or "あっ|pぇ" ("apple").
  const bool covered0_looks_like_particle =
      (covered[0].wcost < kProtectedHealthyWCostThreshold &&
       covered[0].cost < kLowPathCostParticleThreshold) ||
      (covered[0].wcost < kProtectedParticleStrictWCostThreshold &&
       covered[0].cost < kMaxParticleWithSuffixPathCost);
  if (has_preceding_segment && covered.size() == 2 &&
      !covered[0].is_eligible() && covered[0].preedit_len == 1 &&
      covered0_looks_like_particle && last_seg.is_eligible()) {
    const size_t used_last_chars =
        last_seg.preedit_len > extra_chars
            ? (last_seg.preedit_len - extra_chars)
            : 0;
    if (used_last_chars <= 1) {
      return false;
    }
  }

  return true;
}

struct CoveredSegmentsInfo {
  size_t covered_segments = 0;
  size_t accumulated_chars = 0;
  bool contains_high_cost = false;
  bool all_free = true;
};

CoveredSegmentsInfo MeasureCoveredSegments(
    const Segments& segments, size_t start_seg_idx,
    size_t start_preedit_offset, size_t leading_chars, size_t needed_chars,
    const std::vector<composer::ChunkSpan>& spans, double cost_threshold) {
  CoveredSegmentsInfo info;
  std::vector<SegmentEligibilityMetrics> covered;
  size_t seg_pos = start_preedit_offset;
  for (size_t j = start_seg_idx; j < segments.conversion_segments_size(); ++j) {
    const Segment& seg = segments.conversion_segment(j);
    SegmentEligibilityMetrics m =
        BuildSegmentMetrics(seg, seg_pos, spans, cost_threshold);
    info.accumulated_chars += m.preedit_len;
    info.covered_segments++;
    if (!m.is_free) {
      info.all_free = false;
    }
    seg_pos += m.preedit_len;
    covered.push_back(m);
    if (info.accumulated_chars >= needed_chars) {
      break;
    }
  }
  if (info.accumulated_chars >= needed_chars && !covered.empty()) {
    const size_t extra_chars = info.accumulated_chars - needed_chars;
    info.contains_high_cost =
        CanCoverSegmentSpan(covered, /*has_preceding_segment=*/start_seg_idx > 0,
                            leading_chars, extra_chars, spans);
  }
  return info;
}

bool ShouldTriggerFallbackCommon(
    const ConversionRequest& request, const Segments& segments,
    const GruPenaltyConfig& config, bool has_words,
    size_t base_preedit_offset,
    const std::vector<composer::ChunkSpan>& spans) {
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

  if (segments.conversion_segments_size() == 0 ||
      request.composer().GetLength() == 0 || !has_words) {
    return false;
  }

  size_t cur_pos = base_preedit_offset;
  for (size_t i = 0; i < segments.conversion_segments_size(); ++i) {
    const Segment& seg = segments.conversion_segment(i);
    const size_t seg_len = Util::CharsLen(seg.key());
    if (IsSegmentFallbackEligible(seg, cur_pos, cur_pos + seg_len, spans,
                                  config.english_fallback_cost_threshold)) {
      return true;
    }
    cur_pos += seg_len;
  }
  return false;
}

void AppendUniqueCandidate(Segment* seg, converter::Candidate cand) {
  for (size_t i = 0; i < seg->candidates_size(); ++i) {
    if (seg->candidate(i).value == cand.value) {
      return;
    }
  }
  *seg->add_candidate() = std::move(cand);
}

converter::Candidate MakePlainJapaneseCandidate(const std::string& key,
                                               const std::string& value,
                                               uint16_t lid, uint16_t rid,
                                               int32_t cost, int32_t wcost) {
  converter::Candidate cand;
  cand.key = key;
  cand.content_key = key;
  cand.value = value;
  cand.content_value = value;
  cand.cost = cost;
  cand.wcost = wcost;
  cand.structure_cost = std::max(0, cost - wcost);
  cand.lid = lid;
  cand.rid = rid;
  cand.converted_segment_count = 1;
  return cand;
}

bool TryPopulateCandidatesViaConverter(
    const ConversionRequest& request, const std::string& key,
    size_t key_preedit_offset,
    const ImmutableConverterInterface* immutable_converter, Segment* seg) {
  if (immutable_converter == nullptr) {
    return false;
  }
  Segments temp_segments;
  Segment* temp_seg = temp_segments.add_segment();
  temp_seg->set_key(key);
  temp_seg->set_segment_type(Segment::FIXED_BOUNDARY);
  temp_segments.set_atomic_romaji_spans(converter::ExtractAtomicRomajiSpans(
      request.composer(), key_preedit_offset, key));
  if (!immutable_converter->Convert(request.options(), &temp_segments) ||
      temp_segments.conversion_segments_size() == 0 ||
      temp_segments.conversion_segment(0).candidates_size() == 0) {
    return false;
  }
  const Segment& converted = temp_segments.conversion_segment(0);
  for (size_t k = 0; k < converted.candidates_size(); ++k) {
    AppendUniqueCandidate(seg, converted.candidate(k));
  }
  return true;
}

void PopulateExtraSegmentCandidates(
    const ConversionRequest& request, const std::string& extra_key,
    size_t extra_preedit_offset, uint16_t extra_lid, uint16_t extra_rid,
    const ImmutableConverterInterface* immutable_converter,
    Segment* extra_seg) {
  extra_seg->set_key(extra_key);
  extra_seg->clear_candidates();
  extra_seg->clear_meta_candidates();

  if (TryPopulateCandidatesViaConverter(request, extra_key, extra_preedit_offset,
                                        immutable_converter, extra_seg)) {
    return;
  }

  *extra_seg->add_candidate() = MakePlainJapaneseCandidate(
      extra_key, extra_key, extra_lid, extra_rid, /*cost=*/3000, /*wcost=*/2000);
}

void AppendJapaneseFallbackCandidates(
    const ConversionRequest& request, const std::string& merged_key,
    size_t merged_preedit_offset, const std::string& fallback_value,
    uint16_t fallback_lid, uint16_t fallback_rid,
    const ImmutableConverterInterface* immutable_converter, Segment* seg) {
  const bool converted = TryPopulateCandidatesViaConverter(
      request, merged_key, merged_preedit_offset, immutable_converter, seg);
  if (!fallback_value.empty()) {
    AppendUniqueCandidate(
        seg, MakePlainJapaneseCandidate(merged_key, fallback_value, fallback_lid,
                                        fallback_rid, /*cost=*/25000,
                                        /*wcost=*/16383));
  }
  if (!converted && merged_key != fallback_value) {
    AppendUniqueCandidate(
        seg, MakePlainJapaneseCandidate(merged_key, merged_key, fallback_lid,
                                        fallback_rid, /*cost=*/25100,
                                        /*wcost=*/16383));
  }
}

}  // namespace

void EnglishFallbackRewriter::InitBaseDictionary() {
  absl::MutexLock lock(mutex_);
  LoadBaseDictionary(&base_dictionary_);
}

void EnglishFallbackRewriter::MaybeUpdateUserDictionary(
    const GruPenaltyConfig& config) const {
  absl::MutexLock lock(mutex_);
  UpdateUserDictionaryLocked(config, &user_dictionary_, &loaded_user_words_,
                             &last_user_dict_file_mtime_,
                             &user_dict_file_path_);
}

bool EnglishFallbackRewriter::Reload() {
  if (!is_injected_dictionary_) {
    InitBaseDictionary();
    DictionaryState* state = GetSharedDictionaryState();
    absl::MutexLock lock(state->mutex);
    state->base_initialized = false;
    state->loaded_user_words.clear();
    state->last_user_dict_mtime = -1;
  }
  {
    absl::MutexLock lock(mutex_);
    loaded_user_words_.clear();
    last_user_dict_file_mtime_ = -1;
  }
  const auto config = GruConfigManager::LoadConfig();
  MaybeUpdateUserDictionary(config);
  if (!is_injected_dictionary_) {
    EnsureSharedDictionaryUpToDate(config);
  }
  return true;
}

EnglishFallbackRewriter::EnglishFallbackRewriter(
    const dictionary::PosMatcher& pos_matcher,
    const ImmutableConverterInterface* immutable_converter)
    : unknown_id_(pos_matcher.GetUnknownId()),
      immutable_converter_(immutable_converter) {
  InitBaseDictionary();
  MaybeUpdateUserDictionary(GruConfigManager::LoadConfig());
}

EnglishFallbackRewriter::EnglishFallbackRewriter(
    const dictionary::PosMatcher& pos_matcher,
    dictionary::EnglishDictionary dictionary,
    const ImmutableConverterInterface* immutable_converter)
    : unknown_id_(pos_matcher.GetUnknownId()),
      immutable_converter_(immutable_converter),
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
    cand->attributes = converter::Attribute::NO_VARIANTS_EXPANSION |
                       converter::Attribute::DISABLE_RESCORING |
                       converter::Attribute::ENGLISH;
    cost += 100;
  }
}

bool EnglishFallbackRewriter::ShouldTriggerFallback(
    const ConversionRequest& request, const Segments& segments,
    const GruPenaltyConfig& config) const {
  bool has_words = false;
  {
    absl::MutexLock lock(mutex_);
    has_words = !base_dictionary_.empty() || !user_dictionary_.empty();
  }
  return ShouldTriggerFallbackCommon(
      request, segments, config, has_words,
      request.options().base_preedit_offset,
      request.composer().GetChunkSpans());
}

bool EnglishFallbackRewriter::MatchWordAtOffset(
    const composer::ComposerData& composer, size_t offset,
    size_t total_composed_len, size_t min_word_length,
    std::string* matched_word, std::string* matched_original_word,
    size_t* matched_preedit_chars) const {
  if (offset >= total_composed_len) {
    return false;
  }

  const std::vector<composer::ChunkSpan> spans = composer.GetChunkSpans();
  size_t start_idx = spans.size();
  for (size_t i = 0; i < spans.size(); ++i) {
    if (spans[i].preedit_start == offset) {
      start_idx = i;
      break;
    }
    if (spans[i].preedit_start > offset) {
      return false;
    }
  }
  if (start_idx == spans.size()) {
    return false;
  }

  std::string remaining_raw;
  for (size_t i = start_idx; i < spans.size(); ++i) {
    if (spans[i].preedit_start + spans[i].preedit_len > total_composed_len) {
      break;
    }
    remaining_raw.append(spans[i].raw);
  }
  if (remaining_raw.empty()) {
    return false;
  }

  // Try longest dictionary match first, stepping down past longer words whose raw
  // boundary cuts strictly inside a Romaji chunk (e.g. "follows" inside "su" in
  // "followsuru") so the valid prefix ending on a chunk boundary ("follow") is
  // still found.
  absl::string_view search_raw = remaining_raw;
  while (search_raw.size() >= min_word_length) {
    size_t matched_raw_len = 0;
    std::string orig_word;
    {
      absl::MutexLock lock(mutex_);
      matched_raw_len = MatchLongestWordInDictionaries(
          base_dictionary_, user_dictionary_, search_raw, min_word_length,
          &orig_word);
    }
    if (matched_raw_len == 0) {
      return false;
    }

    const std::string word(search_raw.substr(0, matched_raw_len));
    const size_t target_raw_end = spans[start_idx].raw_start + matched_raw_len;

    bool split_inside_romaji_chunk = false;
    for (size_t j = start_idx; j < spans.size(); ++j) {
      const size_t chunk_raw_end = spans[j].raw_start + spans[j].raw_len;
      if (chunk_raw_end < target_raw_end) {
        continue;
      }
      size_t end_preedit_pos = 0;
      if (chunk_raw_end == target_raw_end) {
        end_preedit_pos = spans[j].preedit_start + spans[j].preedit_len;
      } else if (!spans[j].is_romaji &&
                 spans[j].preedit_len == spans[j].raw_len) {
        // `target_raw_end` falls strictly inside a 1:1 non-Romaji (e.g.
        // HALF_ASCII) chunk, which can be sliced cleanly on character units.
        end_preedit_pos =
            spans[j].preedit_start + (target_raw_end - spans[j].raw_start);
      } else {
        // Never split mid-Romaji chunk (handled via Composer chunk separation).
        split_inside_romaji_chunk = true;
        break;
      }
      *matched_word = word;
      if (matched_original_word != nullptr) {
        *matched_original_word = orig_word;
      }
      *matched_preedit_chars = end_preedit_pos - offset;
      return true;
    }
    if (!split_inside_romaji_chunk) {
      return false;
    }
    search_raw = search_raw.substr(0, matched_raw_len - 1);
  }
  return false;
}

bool EnglishFallbackRewriter::MatchBestWordAtOffset(
    const composer::ComposerData& composer, size_t offset,
    size_t total_composed_len, size_t min_word_length,
    std::string* matched_word, std::string* matched_original_word,
    size_t* matched_preedit_chars) const {
  if (!MatchWordAtOffset(composer, offset, total_composed_len, min_word_length,
                         matched_word, matched_original_word,
                         matched_preedit_chars)) {
    return false;
  }
  const size_t current_raw_len = matched_word->size();
  // Prevent a short spurious match (e.g., Japanese particle "と" + 'P' ->
  // "top") from stealing the leading character of a strictly longer English
  // word starting inside the same span (e.g., "Pencil").
  for (size_t q = offset + 1; q < offset + *matched_preedit_chars; ++q) {
    std::string next_word;
    std::string next_orig;
    size_t next_preedit_chars = 0;
    if (MatchWordAtOffset(composer, q, total_composed_len, min_word_length,
                          &next_word, &next_orig, &next_preedit_chars) &&
        next_word.size() > current_raw_len) {
      return false;
    }
  }
  return true;
}

std::vector<size_t> EnglishFallbackRewriter::FindCrossBoundarySplitOffsets(
    const composer::ComposerData& composer, const Segments& segments,
    const ConversionRequest& request, size_t base_preedit_offset) {
  const auto config = GruConfigManager::LoadConfig();
  EnsureSharedDictionaryUpToDate(config);

  DictionaryState* dict_state = GetSharedDictionaryState();
  bool has_words = false;
  {
    absl::MutexLock lock(dict_state->mutex);
    has_words = !dict_state->base_dict.empty() || !dict_state->user_dict.empty();
  }
  const std::vector<composer::ChunkSpan> spans = composer.GetChunkSpans();
  if (spans.empty() ||
      !ShouldTriggerFallbackCommon(request, segments, config, has_words,
                                    base_preedit_offset, spans)) {
    return {};
  }

  std::vector<SegmentEligibilityMetrics> seg_metrics;
  seg_metrics.reserve(segments.conversion_segments_size());
  size_t cur_pos = base_preedit_offset;
  for (size_t i = 0; i < segments.conversion_segments_size(); ++i) {
    seg_metrics.push_back(BuildSegmentMetrics(
        segments.conversion_segment(i), cur_pos, spans,
        config.english_fallback_cost_threshold));
    cur_pos += seg_metrics.back().preedit_len;
  }
  const size_t max_seg_preedit_end = cur_pos;

  std::string active_raw;
  size_t active_span_end = 0;
  for (size_t i = 0; i < spans.size(); ++i) {
    if (spans[i].preedit_start >= max_seg_preedit_end) {
      break;
    }
    active_raw.append(spans[i].raw);
    active_span_end = i + 1;
  }
  if (active_raw.empty()) {
    return {};
  }

  auto span_range_can_cover = [&](size_t start_p, size_t end_p) {
    std::vector<SegmentEligibilityMetrics> covered;
    size_t first_covered_idx = 0;
    for (size_t i = 0; i < seg_metrics.size(); ++i) {
      const auto& sm = seg_metrics[i];
      if (sm.preedit_end() <= start_p || sm.preedit_start >= end_p) {
        continue;
      }
      if (!sm.is_free) {
        return false;
      }
      if (covered.empty()) {
        first_covered_idx = i;
      }
      covered.push_back(sm);
    }
    if (covered.empty()) {
      return false;
    }
    const size_t leading_chars =
        start_p > covered.front().preedit_start
            ? (start_p - covered.front().preedit_start)
            : 0;
    const size_t extra_chars =
        covered.back().preedit_end() > end_p
            ? (covered.back().preedit_end() - end_p)
            : 0;
    return CanCoverSegmentSpan(covered,
                               /*has_preceding_segment=*/first_covered_idx > 0,
                               leading_chars, extra_chars, spans);
  };

  std::vector<size_t> split_offsets;
  size_t idx = 0;
  while (idx < active_span_end) {
    if (spans[idx].preedit_start < base_preedit_offset ||
        spans[idx].raw_len == 0) {
      ++idx;
      continue;
    }

    bool start_allowed = false;
    for (const auto& sm : seg_metrics) {
      if (sm.preedit_end() > spans[idx].preedit_start &&
          sm.preedit_start <= spans[idx].preedit_start) {
        const bool mid_segment = (spans[idx].preedit_start > sm.preedit_start);
        start_allowed = sm.is_free && (!mid_segment || sm.is_eligible());
        break;
      }
    }
    if (!start_allowed) {
      ++idx;
      continue;
    }

    const absl::string_view remaining_raw =
        absl::string_view(active_raw).substr(spans[idx].raw_start);
    std::string orig_word;
    size_t matched_raw_len = 0;
    {
      absl::MutexLock lock(dict_state->mutex);
      matched_raw_len = MatchLongestWordInDictionaries(
          dict_state->base_dict, dict_state->user_dict, remaining_raw,
          config.english_fallback_min_word_length, &orig_word);
    }
    if (matched_raw_len == 0) {
      ++idx;
      continue;
    }

    const size_t target_raw_end = spans[idx].raw_start + matched_raw_len;
    // Mirror MatchBestWordAtOffset lookahead: suppress a short spurious match
    // (e.g. "と"+"p..." -> "top") if a strictly longer word starts strictly
    // inside the matched span (e.g. "pencil").
    bool suppressed_by_longer_match = false;
    {
      absl::MutexLock lock(dict_state->mutex);
      auto has_longer_match_at = [&](size_t next_raw) {
        std::string next_orig;
        return MatchLongestWordInDictionaries(
                   dict_state->base_dict, dict_state->user_dict,
                   absl::string_view(active_raw).substr(next_raw),
                   config.english_fallback_min_word_length, &next_orig) >
               matched_raw_len;
      };
      for (size_t k = idx;
           k < active_span_end && spans[k].raw_start < target_raw_end; ++k) {
        if (k > idx && has_longer_match_at(spans[k].raw_start)) {
          suppressed_by_longer_match = true;
          break;
        }
        if (!spans[k].is_romaji && spans[k].preedit_len == spans[k].raw_len) {
          const size_t limit = std::min(
              spans[k].raw_start + spans[k].raw_len, target_raw_end);
          for (size_t pos = std::max(spans[k].raw_start,
                                     spans[idx].raw_start + 1);
               pos < limit; ++pos) {
            if ((static_cast<unsigned char>(active_raw[pos]) & 0xC0) == 0x80) {
              continue;
            }
            if (has_longer_match_at(pos)) {
              suppressed_by_longer_match = true;
              break;
            }
          }
          if (suppressed_by_longer_match) {
            break;
          }
        }
      }
    }
    if (suppressed_by_longer_match) {
      ++idx;
      continue;
    }

    size_t j = idx;
    while (j < active_span_end &&
           spans[j].raw_start + spans[j].raw_len < target_raw_end) {
      ++j;
    }
    if (j >= active_span_end) {
      ++idx;
      continue;
    }

    const size_t chunk_raw_end = spans[j].raw_start + spans[j].raw_len;
    if (spans[j].raw_start < target_raw_end &&
        target_raw_end < chunk_raw_end && spans[j].is_romaji) {
      if (span_range_can_cover(
              spans[idx].preedit_start,
              spans[j].preedit_start + spans[j].preedit_len)) {
        split_offsets.push_back(target_raw_end);
        idx = j + 1;
        continue;
      }
    }

    ++idx;
  }

  return split_offsets;
}

std::optional<RewriterInterface::ResizeSegmentsRequest>
EnglishFallbackRewriter::CheckResizeSegmentsRequest(
    const ConversionRequest& request, const Segments& segments) const {
  return CheckResizeSegmentsRequestInternal(request, segments,
                                            /*allow_when_resized=*/false);
}

std::optional<RewriterInterface::ResizeSegmentsRequest>
EnglishFallbackRewriter::CheckAdditionalResizeSegmentsRequest(
    const ConversionRequest& request, const Segments& segments) const {
  return CheckResizeSegmentsRequestInternal(request, segments,
                                            /*allow_when_resized=*/true);
}

std::optional<RewriterInterface::ResizeSegmentsRequest>
EnglishFallbackRewriter::CheckResizeSegmentsRequestInternal(
    const ConversionRequest& request, const Segments& segments,
    const bool allow_when_resized) const {
  if (!allow_when_resized && segments.resized()) {
    return std::nullopt;
  }

  const auto config = GruConfigManager::LoadConfig();
  MaybeUpdateUserDictionary(config);
  if (!ShouldTriggerFallback(request, segments, config)) {
    return std::nullopt;
  }

  const composer::ComposerData& composer = request.composer();
  const std::vector<composer::ChunkSpan> spans = composer.GetChunkSpans();
  const size_t base_preedit_offset = request.options().base_preedit_offset;

  size_t total_composed_len = base_preedit_offset;
  for (size_t i = 0; i < segments.conversion_segments_size(); ++i) {
    total_composed_len += Util::CharsLen(segments.conversion_segment(i).key());
  }

  size_t current_seg_idx = 0;
  size_t current_preedit_offset = base_preedit_offset;
  size_t skip_until_preedit_offset = base_preedit_offset;

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

    bool advanced_exact_match = false;
    const bool cur_seg_eligible = IsSegmentFallbackEligible(
        cur_seg, current_preedit_offset, current_preedit_offset + cur_seg_len,
        spans, config.english_fallback_cost_threshold);
    // Never slice mid-segment into a healthy (non-eligible) starting segment.
    const size_t p_end =
        cur_seg_eligible ? (current_preedit_offset + cur_seg_len)
                         : (current_preedit_offset + 1);
    for (size_t p = std::max(current_preedit_offset, skip_until_preedit_offset);
         p < p_end; ++p) {
      std::string matched_word;
      std::string matched_original_word;
      size_t matched_preedit_chars = 0;
      if (!MatchBestWordAtOffset(composer, p, total_composed_len,
                                 config.english_fallback_min_word_length,
                                 &matched_word, &matched_original_word,
                                 &matched_preedit_chars)) {
        continue;
      }

      const size_t leading_chars = p - current_preedit_offset;
      const size_t needed_chars = leading_chars + matched_preedit_chars;

      const CoveredSegmentsInfo covered = MeasureCoveredSegments(
          segments, current_seg_idx, current_preedit_offset, leading_chars,
          needed_chars, spans, config.english_fallback_cost_threshold);
      const size_t covered_segments = covered.covered_segments;
      const size_t accumulated_chars = covered.accumulated_chars;
      const bool contains_high_cost = covered.contains_high_cost;
      const bool all_free = covered.all_free;

      if (accumulated_chars >= needed_chars && covered_segments > 0) {
        const size_t extra_chars = accumulated_chars - needed_chars;
        if (!all_free) {
          skip_until_preedit_offset =
              std::max(skip_until_preedit_offset, p + matched_preedit_chars);
        } else if (contains_high_cost) {
          if (leading_chars > 0 &&
              leading_chars <= std::numeric_limits<uint8_t>::max() &&
              matched_preedit_chars <= std::numeric_limits<uint8_t>::max()) {
            ResizeSegmentsRequest resize_request;
            resize_request.segment_index = current_seg_idx;
            resize_request.segment_sizes[0] =
                static_cast<uint8_t>(leading_chars);
            resize_request.segment_sizes[1] =
                static_cast<uint8_t>(matched_preedit_chars);
            return resize_request;
          }
          if (leading_chars == 0 && (covered_segments > 1 || extra_chars > 0) &&
              matched_preedit_chars <= std::numeric_limits<uint8_t>::max()) {
            ResizeSegmentsRequest resize_request;
            resize_request.segment_index = current_seg_idx;
            resize_request.segment_sizes[0] =
                static_cast<uint8_t>(matched_preedit_chars);
            return resize_request;
          }
          if (leading_chars == 0 && covered_segments == 1 && extra_chars == 0) {
            current_preedit_offset += matched_preedit_chars;
            current_seg_idx++;
            advanced_exact_match = true;
            break;
          }
        }
      }
    }

    if (advanced_exact_match) {
      continue;
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
  const std::vector<composer::ChunkSpan> spans = composer.GetChunkSpans();
  const size_t base_preedit_offset = request.options().base_preedit_offset;

  // Total character length across all conversion segments in composer space.
  size_t total_composed_len = base_preedit_offset;
  for (size_t i = 0; i < segments->conversion_segments_size(); ++i) {
    total_composed_len += Util::CharsLen(segments->conversion_segment(i).key());
  }

  bool modified = false;
  size_t current_seg_idx = 0;
  size_t current_preedit_offset = base_preedit_offset;
  size_t skip_until_preedit_offset = base_preedit_offset;

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
    if (!MatchBestWordAtOffset(
            composer, current_preedit_offset, total_composed_len,
            config.english_fallback_min_word_length, &matched_word,
            &matched_original_word, &matched_preedit_chars)) {
      current_preedit_offset += cur_seg_len;
      current_seg_idx++;
      continue;
    }

    // Determine how many conversion segments are covered by matched_preedit_chars.
    const CoveredSegmentsInfo covered = MeasureCoveredSegments(
        *segments, current_seg_idx, current_preedit_offset, /*leading_chars=*/0,
        matched_preedit_chars, spans, config.english_fallback_cost_threshold);
    const size_t covered_segments = covered.covered_segments;
    const size_t accumulated_chars = covered.accumulated_chars;
    const bool contains_high_cost = covered.contains_high_cost;
    const bool all_free = covered.all_free;

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
        const Segment& first_seg = segments->conversion_segment(current_seg_idx);
        const Segment& last_seg_ref = segments->conversion_segment(last_seg_idx);
        const uint16_t fallback_lid =
            first_seg.candidates_size() > 0 ? first_seg.candidate(0).lid
                                            : unknown_id_;
        const uint16_t fallback_rid =
            last_seg_ref.candidates_size() > 0 ? last_seg_ref.candidate(0).rid
                                               : unknown_id_;

        std::string merged_key;
        for (size_t j = 0; j < covered_segments - 1; ++j) {
          merged_key += segments->conversion_segment(current_seg_idx + j).key();
        }

        if (extra_chars == 0) {
          merged_key += last_seg_ref.key();

          std::vector<converter::Candidate> saved_candidates;
          std::string concat_fallback_value;
          if (covered_segments == 1) {
            saved_candidates.reserve(first_seg.candidates_size());
            for (size_t k = 0; k < first_seg.candidates_size(); ++k) {
              saved_candidates.push_back(first_seg.candidate(k));
            }
          } else {
            for (size_t j = 0; j < covered_segments; ++j) {
              const Segment& covered_seg =
                  segments->conversion_segment(current_seg_idx + j);
              concat_fallback_value +=
                  covered_seg.candidates_size() > 0
                      ? covered_seg.candidate(0).value
                      : covered_seg.key();
            }
          }

          Segment* seg = segments->mutable_conversion_segment(current_seg_idx);
          seg->set_key(merged_key);
          seg->clear_candidates();
          if (covered_segments > 1) {
            seg->clear_meta_candidates();
          }
          AddEnglishCandidates(matched_word, matched_original_word, merged_key, seg);
          if (covered_segments == 1) {
            for (auto& orig_cand : saved_candidates) {
              AppendUniqueCandidate(seg, std::move(orig_cand));
            }
          } else {
            AppendJapaneseFallbackCandidates(
                request, merged_key, current_preedit_offset,
                concat_fallback_value, fallback_lid, fallback_rid,
                immutable_converter_, seg);
            const size_t raw_idx =
                segments->history_segments_size() + current_seg_idx;
            segments->erase_segments(raw_idx + 1, covered_segments - 1);
          }
        } else {
          // The last segment covers the tail of the English word plus trailing
          // characters (e.g. "ryを" -> "ry" + "を").
          const size_t last_seg_len = Util::CharsLen(last_seg_ref.key());
          const size_t used_chars = last_seg_len - extra_chars;

          const std::string used_key =
              std::string(Util::Utf8SubString(last_seg_ref.key(), 0, used_chars));
          const std::string extra_key = std::string(
              Util::Utf8SubString(last_seg_ref.key(), used_chars, extra_chars));
          merged_key += used_key;

          uint16_t extra_lid = 0, extra_rid = 0;
          if (last_seg_ref.candidates_size() > 0) {
            extra_lid = last_seg_ref.candidate(0).lid;
            extra_rid = last_seg_ref.candidate(0).rid;
          }

          const size_t raw_start_idx =
              segments->history_segments_size() + current_seg_idx;

          Segment* seg =
              segments->mutable_conversion_segment(current_seg_idx);
          seg->set_key(merged_key);
          seg->clear_candidates();
          seg->clear_meta_candidates();
          AddEnglishCandidates(matched_word, matched_original_word, merged_key,
                               seg);
          AppendJapaneseFallbackCandidates(
              request, merged_key, current_preedit_offset, merged_key,
              fallback_lid, fallback_rid, immutable_converter_, seg);

          Segment* extra_seg = nullptr;
          if (covered_segments == 1) {
            extra_seg = segments->insert_segment(raw_start_idx + 1);
          } else {
            if (covered_segments > 2) {
              segments->erase_segments(raw_start_idx + 1, covered_segments - 2);
            }
            extra_seg =
                segments->mutable_conversion_segment(current_seg_idx + 1);
          }
          PopulateExtraSegmentCandidates(
              request, extra_key, current_preedit_offset + matched_preedit_chars,
              extra_lid, extra_rid, immutable_converter_, extra_seg);
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
