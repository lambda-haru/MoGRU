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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "base/file_util.h"
#include "base/system_util.h"
#include "base/util.h"
#include "base/vlog.h"
#include "converter/attribute.h"
#include "converter/candidate.h"
#include "converter/immutable_converter_interface.h"
#include "gru_scorer/charindex.hh"
#include "gru_scorer/gru_model.hh"
#include "protocol/config.pb.h"

ABSL_FLAG(bool, enable_gru_rewriter, false, "Enable GRU neural LM rewriter.");
ABSL_FLAG(bool, enable_gru_resegmentation, true,
          "Enable GRU-based segment boundary resegmentation.");
ABSL_FLAG(bool, enable_gru_online_learning, true,
          "Enable in-process online learning from committed user history.");
ABSL_FLAG(int32_t, gru_resegment_cost_threshold, 600,
          "Minimum cost improvement required to trigger resegmentation.");
ABSL_FLAG(bool, show_gru_score, true,
          "Show GRU score in candidate description.");
ABSL_FLAG(double, gru_score_weight, 800.0,
          "Weight for GRU score combining with Viterbi cost.");
ABSL_FLAG(bool, enable_gru_auto_alpha, true,
          "Enable hybrid online + context-aware automatic alpha adjustment.");
ABSL_FLAG(double, gru_katakana_multiplier, 1.3,
          "Score multiplier for all-katakana candidates (>= 1.0 penalizes).");
ABSL_FLAG(double, gru_hiragana_multiplier, 1.1,
          "Score multiplier for all-hiragana candidates (>= 1.0 penalizes).");
ABSL_FLAG(double, gru_hiragana_length_power, 1.0,
          "Exponent for hiragana length penalty scaling.");
ABSL_FLAG(std::string, gru_model_path, "",
          "Path to GRU model file. If empty, uses default path.");
ABSL_FLAG(std::string, gru_user_model_path, "",
          "Path to user-adapted GRU model file. If empty, uses ~/.config/mogru/grumodel_user.bin");
ABSL_FLAG(std::string, gru_chardata_path, "",
          "Path to CharIndex data file. If empty, uses default path.");
ABSL_FLAG(std::string, gru_config_path, "",
          "Path to gru_config.ini. If empty, uses ~/.config/mogru/gru_config.ini");

namespace mozc {
namespace {

constexpr size_t kMaxCandidateEvaluation = 10;
constexpr size_t kBeamSize = 8;
constexpr size_t kMaxCandidatesPerSegmentForBeam = 5;
constexpr int32_t kMaxCostDiff = 4000;

enum class PenaltyType { NONE, KATAKANA, HIRAGANA, EMOJI };

struct PenaltyResult {
  double penalty = 0.0;
  PenaltyType type = PenaltyType::NONE;
};

bool ContainsEmoji(const charindex::CharIndex& indexer, const std::string& str) {
  std::string_view sv = str;
  while (!sv.empty()) {
    size_t consumed = 0;
    const char32_t cp = indexer.DecodeUtf8Char(sv, consumed);
    if (consumed == 0) break;
    if (charindex::IsEmojiCodePoint(cp)) return true;
    sv.remove_prefix(consumed);
  }
  return false;
}

bool IsCandidateEmoji(const charindex::CharIndex& indexer,
                      const Segment::Candidate& cand) {
  if (cand.category == converter::Candidate::SYMBOL) return true;
  if (absl::StrContains(cand.description, "絵文字")) return true;
  return ContainsEmoji(indexer, cand.value);
}

bool IsSymbolSegment(const Segment& seg) {
  return !Util::ContainsScriptType(seg.key(), Util::HIRAGANA);
}

bool IsAlphabetCandidate(const Segment::Candidate& cand) {
  return Util::ContainsScriptType(cand.value, Util::ALPHABET);
}

bool IsEnglishFallbackSegment(const Segment& seg) {
  return seg.candidates_size() > 0 &&
         (seg.candidate(0).attributes &
          converter::Attribute::DISABLE_RESCORING) != 0 &&
         IsAlphabetCandidate(seg.candidate(0));
}

bool ShouldBypassSegment(const Segment& seg) {
  return seg.segment_type() == Segment::FIXED_VALUE ||
         IsSymbolSegment(seg) || IsEnglishFallbackSegment(seg);
}

bool IsKatakanaCandidate(const Segment::Candidate& cand) {
  return !cand.value.empty() && Util::IsScriptType(cand.value, Util::KATAKANA);
}

bool IsHiraganaCandidate(const Segment::Candidate& cand) {
  return !cand.value.empty() && Util::IsScriptType(cand.value, Util::HIRAGANA);
}

PenaltyResult CalculatePenalty(const Segment::Candidate& cand,
                               bool is_emoji,
                               const GruPenaltyConfig& config) {
  if (is_emoji) {
    return {config.emoji_penalty, PenaltyType::EMOJI};
  }
  return {0.0, PenaltyType::NONE};
}

float ScoreCandidateWithContext(const gruscorer::GruModel& model,
                                const charindex::CharIndex& indexer,
                                const Segment::Candidate& cand,
                                const gruscorer::GruContext& context,
                                const GruPenaltyConfig& config) {
  float score =
      model.ScoreStringWithContext(indexer, cand.value, context, true);
  if (IsAlphabetCandidate(cand)) {
    score *= static_cast<float>(config.alphabet_multiplier);
  } else if (IsKatakanaCandidate(cand)) {
    score *= static_cast<float>(config.katakana_multiplier);
  } else if (IsHiraganaCandidate(cand)) {
    const size_t char_len = Util::CharsLen(cand.value);
    double mult = 1.0;
    if (char_len > 1) {
      const double excess = static_cast<double>(char_len - 1);
      const double scale = (config.hiragana_length_power == 1.0)
                               ? excess
                               : (config.hiragana_length_power == 0.0)
                                     ? 1.0
                                     : std::pow(excess, config.hiragana_length_power);
      mult = 1.0 + (config.hiragana_multiplier - 1.0) * scale;
    }
    score *= static_cast<float>(mult);
  }
  return score;
}

std::string GetHistoryPrefix(const ConversionRequest* request,
                             const Segments& segments) {
  if (segments.history_segments_size() > 0) {
    return segments.history_value();
  }
  if (request != nullptr && request->context().has_preceding_text()) {
    return request->GetSurroundingContext().first;
  }
  return std::string(segments.preceding_text());
}

constexpr size_t kMaxPrefixChars = 32;

gruscorer::GruContext ComputePrefixContext(
    const gruscorer::GruModel& model,
    const charindex::CharIndex& indexer,
    const std::string& hist_prefix,
    const std::string& current_prefix) {
  std::string full = hist_prefix + current_prefix;
  if (full.empty()) {
    return gruscorer::GruContext();
  }
  const size_t prefix_chars = Util::CharsLen(full);
  if (prefix_chars > kMaxPrefixChars) {
    full = std::string(Util::Utf8SubString(
        full, prefix_chars - kMaxPrefixChars, kMaxPrefixChars));
  }
  return model.ComputeContext(indexer, full);
}

struct CandidateScore {
  const Segment::Candidate* cand_ptr = nullptr;
  int32_t viterbi_cost = 0;
  double combined_cost = 0.0;
  float gru_score = 0.0f;
  PenaltyType penalty_type = PenaltyType::NONE;
  double penalty_val = 0.0;
  bool within_cost_diff = true;
  bool is_best = false;
};

size_t ComputeEffectivePrefixChars(const std::string& hist_prefix,
                                   const std::string& current_prefix) {
  return std::min(kMaxPrefixChars,
                  Util::CharsLen(hist_prefix) + Util::CharsLen(current_prefix));
}

double ComputeDynamicAlpha(double alpha_base,
                           size_t prefix_char_len,
                           const std::vector<CandidateScore>& candidates,
                           size_t max_stat_cands,
                           bool enable_auto_alpha) {
  if (!enable_auto_alpha || !std::isfinite(alpha_base) || alpha_base <= 0.0) {
    return alpha_base;
  }

  const size_t clipped_ctx = std::min(prefix_char_len, kMaxPrefixChars);
  const double ctx_ratio = std::min(1.0, static_cast<double>(clipped_ctx) / 16.0);
  const double delta_ctx = -0.15 + 0.30 * ctx_ratio;

  double min_mozc_cost = std::numeric_limits<double>::infinity();
  double second_mozc_cost = std::numeric_limits<double>::infinity();
  double max_gru_score = -std::numeric_limits<double>::infinity();
  double second_gru_score = -std::numeric_limits<double>::infinity();
  double max_non_emoji_gru = -std::numeric_limits<double>::infinity();
  double second_non_emoji_gru = -std::numeric_limits<double>::infinity();
  size_t valid_count = 0;
  size_t valid_non_emoji_count = 0;

  const size_t limit = std::min(candidates.size(), max_stat_cands);
  for (size_t i = 0; i < limit; ++i) {
    const CandidateScore& c = candidates[i];
    if (!c.within_cost_diff) continue;
    ++valid_count;

    const double mozc_cost = static_cast<double>(c.viterbi_cost) + c.penalty_val;
    if (mozc_cost < min_mozc_cost) {
      second_mozc_cost = min_mozc_cost;
      min_mozc_cost = mozc_cost;
    } else if (mozc_cost < second_mozc_cost) {
      second_mozc_cost = mozc_cost;
    }

    const double gs = static_cast<double>(c.gru_score);
    if (gs > max_gru_score) {
      second_gru_score = max_gru_score;
      max_gru_score = gs;
    } else if (gs > second_gru_score) {
      second_gru_score = gs;
    }

    if (c.penalty_type != PenaltyType::EMOJI) {
      ++valid_non_emoji_count;
      if (gs > max_non_emoji_gru) {
        second_non_emoji_gru = max_non_emoji_gru;
        max_non_emoji_gru = gs;
      } else if (gs > second_non_emoji_gru) {
        second_non_emoji_gru = gs;
      }
    }
  }

  double delta_vit = 0.0;
  double delta_gru = 0.0;
  if (valid_count >= 2 && std::isfinite(second_mozc_cost)) {
    const double gap_vit = std::max(0.0, second_mozc_cost - min_mozc_cost);
    if (gap_vit <= 1000.0) {
      delta_vit = 0.25 * (1.0 - gap_vit / 1000.0);
    } else {
      delta_vit = -0.25 * std::min(1.0, (gap_vit - 1000.0) / 1500.0);
    }
  }

  const double ref_max_gs =
      (valid_non_emoji_count >= 2) ? max_non_emoji_gru : max_gru_score;
  const double ref_second_gs =
      (valid_non_emoji_count >= 2) ? second_non_emoji_gru : second_gru_score;
  const size_t ref_count =
      (valid_non_emoji_count >= 2) ? valid_non_emoji_count : valid_count;

  if (ref_count >= 2 && std::isfinite(ref_max_gs) &&
      std::isfinite(ref_second_gs)) {
    const double spread = std::max(0.0, ref_max_gs - ref_second_gs);
    double spread_term = 0.0;
    if (spread < 0.15) {
      spread_term = -0.10 * (1.0 - spread / 0.15);
    } else if (spread > 0.40) {
      spread_term = 0.20 * std::min(1.0, (spread - 0.40) / 1.20);
    }
    double oov_damp = 0.0;
    if (ref_max_gs < -6.0) {
      oov_damp = -0.25 * std::min(1.0, (-6.0 - ref_max_gs) / 4.0);
    }
    delta_gru = std::clamp(spread_term + oov_damp, -0.30, 0.20);
  } else if (valid_count == 1 && std::isfinite(max_gru_score) &&
             max_gru_score < -6.0) {
    delta_gru = -0.25 * std::min(1.0, (-6.0 - max_gru_score) / 4.0);
  }

  const double mult = std::clamp(1.0 + delta_ctx + delta_vit + delta_gru, 0.5, 1.5);
  return alpha_base * mult;
}

std::vector<CandidateScore> EvaluateSegmentCandidates(
    const gruscorer::GruModel& model,
    const charindex::CharIndex& indexer,
    const Segment& seg,
    const gruscorer::GruContext& context,
    size_t prefix_char_len,
    size_t max_eval,
    size_t max_stat_cands,
    int32_t ref_min_cost,
    const GruPenaltyConfig& penalty_config,
    double alpha_base,
    bool enable_auto_alpha,
    double* out_alpha_eff) {
  const size_t num_eval = std::min(seg.candidates_size(), max_eval);
  std::vector<CandidateScore> scored;
  scored.reserve(num_eval);

  for (size_t i = 0; i < num_eval; ++i) {
    const auto& cand = seg.candidate(i);
    const bool is_emoji = IsCandidateEmoji(indexer, cand);
    const float gru_score =
        ScoreCandidateWithContext(model, indexer, cand, context, penalty_config);
    const PenaltyResult penalty_res =
        CalculatePenalty(cand, is_emoji, penalty_config);
    const bool within = (cand.cost <= ref_min_cost + kMaxCostDiff);

    CandidateScore cs;
    cs.cand_ptr = &cand;
    cs.viterbi_cost = cand.cost;
    cs.gru_score = gru_score;
    cs.penalty_type = penalty_res.type;
    cs.penalty_val = penalty_res.penalty;
    cs.within_cost_diff = within;
    scored.push_back(cs);
  }

  const double alpha_eff = ComputeDynamicAlpha(
      alpha_base, prefix_char_len, scored, max_stat_cands, enable_auto_alpha);
  if (out_alpha_eff != nullptr) {
    *out_alpha_eff = alpha_eff;
  }

  for (auto& cs : scored) {
    double eval = static_cast<double>(cs.viterbi_cost) -
                  (alpha_eff * cs.gru_score) + cs.penalty_val;
    if (!cs.within_cost_diff) {
      eval += 1e9;
    }
    cs.combined_cost = eval;
  }
  return scored;
}

void UpdateCandidateDescription(Segment::Candidate* cand,
                                float gru_score,
                                double combined_cost,
                                double effective_alpha,
                                PenaltyType penalty_type,
                                double penalty_val,
                                bool is_promoted) {
  std::string penalty_tag;
  if (penalty_type == PenaltyType::KATAKANA && penalty_val > 0.0) {
    penalty_tag = absl::StrFormat("[K+%.0f]", penalty_val);
  } else if (penalty_type == PenaltyType::HIRAGANA && penalty_val > 0.0) {
    penalty_tag = absl::StrFormat("[H+%.0f]", penalty_val);
  }

  std::string tag = (penalty_type == PenaltyType::EMOJI)
      ? absl::StrFormat("GRU:EMOJI[E:%.0f]", combined_cost)
      : absl::StrFormat("GRU:%.2f%s[E:%.0f][α:%.0f]", gru_score, penalty_tag,
                        combined_cost, effective_alpha);
  if (is_promoted) {
    tag += "*";
  }
  const size_t pos = cand->description.find("GRU:");
  if (pos != std::string::npos) {
    const size_t end_pos = cand->description.find(' ', pos);
    if (end_pos != std::string::npos) {
      cand->description.replace(pos, end_pos - pos, tag);
    } else {
      cand->description.replace(pos, std::string::npos, tag);
    }
  } else {
    if (cand->description.empty()) {
      cand->description = std::move(tag);
    } else {
      cand->description = absl::StrCat(cand->description, " ", tag);
    }
  }
}

// Sorts evaluated candidates of `seg` by combined_cost in ascending order.
// Ensures that the candidate marked with is_best (if any) is placed at index 0.
bool SortSegmentCandidates(Segment* seg,
                           std::vector<CandidateScore> scored,
                           double effective_alpha,
                           bool show_score) {
  if (scored.empty()) return false;

  const Segment::Candidate* original_top = &seg->candidate(0);

  // Sort by combined_cost ascending.
  // If is_best is true for one candidate, ensure it sorts first.
  std::stable_sort(scored.begin(), scored.end(),
                   [](const CandidateScore& a, const CandidateScore& b) {
                     if (a.is_best != b.is_best) {
                       return a.is_best;
                     }
                     return a.combined_cost < b.combined_cost;
                   });

  // Reorder candidates in seg using move_candidate
  bool modified = false;
  for (size_t target_idx = 0; target_idx < scored.size(); ++target_idx) {
    const Segment::Candidate* target_ptr = scored[target_idx].cand_ptr;
    for (size_t cur_idx = target_idx; cur_idx < seg->candidates_size(); ++cur_idx) {
      if (&seg->candidate(cur_idx) == target_ptr) {
        if (cur_idx != target_idx) {
          seg->move_candidate(cur_idx, target_idx);
          modified = true;
        }
        break;
      }
    }
  }

  // Update descriptions and attribute for the top candidate
  for (size_t i = 0; i < scored.size(); ++i) {
    auto* cand = seg->mutable_candidate(i);
    const bool is_promoted = (i == 0 && cand != original_top);
    if (show_score) {
      UpdateCandidateDescription(cand, scored[i].gru_score, scored[i].combined_cost,
                                 effective_alpha, scored[i].penalty_type,
                                 scored[i].penalty_val, is_promoted);
    }
  }

  if (&seg->candidate(0) != original_top) {
    seg->mutable_candidate(0)->attributes |= converter::Attribute::CONTEXT_SENSITIVE;
  }

  return modified;
}

}  // namespace

GruRewriter::GruRewriter(const ImmutableConverterInterface* immutable_converter)
    : immutable_converter_(immutable_converter) {}
GruRewriter::~GruRewriter() {
  Sync();
}

GruPenaltyConfig GruRewriter::GetPenaltyConfig() const {
  absl::MutexLock l(mutex_);
  const absl::Time now = absl::Now();
  if (now - last_config_check_time_ < absl::Seconds(1)) {
    return penalty_config_;
  }
  last_config_check_time_ = now;

  if (config_file_path_.empty()) {
    config_file_path_ = config::GruConfigManager::ResolveConfigFilePath(
        absl::GetFlag(FLAGS_gru_config_path), /*allow_in_test=*/false);
    config::GruConfigManager::EnsureConfigFileExists(config_file_path_);
  }

  const int64_t current_mtime =
      config::GruConfigManager::GetFileMtime(config_file_path_);

  if (current_mtime != last_config_mtime_ || last_config_mtime_ == 0) {
    last_config_mtime_ = current_mtime;
    GruPenaltyConfig base;
    base.enable_gru_rewriter = absl::GetFlag(FLAGS_enable_gru_rewriter);
    base.gru_model_path = absl::GetFlag(FLAGS_gru_model_path);
    base.gru_chardata_path = absl::GetFlag(FLAGS_gru_chardata_path);
    base.gru_user_model_path = absl::GetFlag(FLAGS_gru_user_model_path);
    base.gru_score_weight = absl::GetFlag(FLAGS_gru_score_weight);
    base.enable_auto_alpha = absl::GetFlag(FLAGS_enable_gru_auto_alpha);
    base.show_gru_score = absl::GetFlag(FLAGS_show_gru_score);
    base.katakana_multiplier = absl::GetFlag(FLAGS_gru_katakana_multiplier);
    base.hiragana_multiplier = absl::GetFlag(FLAGS_gru_hiragana_multiplier);
    base.hiragana_length_power = absl::GetFlag(FLAGS_gru_hiragana_length_power);
    base.enable_resegmentation = absl::GetFlag(FLAGS_enable_gru_resegmentation);
    base.resegment_threshold =
        static_cast<double>(absl::GetFlag(FLAGS_gru_resegment_cost_threshold));
    base.enable_online_learning = absl::GetFlag(FLAGS_enable_gru_online_learning);

    GruPenaltyConfig root_config;
    if (!config_file_path_.empty() && std::filesystem::exists(config_file_path_)) {
      root_config = config::GruConfigManager::ParseConfigFile(
          config_file_path_, base);
    } else {
      root_config = base;
    }
    config::GruConfigManager::ApplyEnvOverrides(&root_config);
    root_penalty_config_ = root_config;

    const std::string target_profile =
        root_config.current_profile.empty() ? "default" : root_config.current_profile;
    SwitchProfileLocked(target_profile, root_config);

    LOG(INFO) << "GruRewriter: Hot-reloaded configuration from "
              << config_file_path_ << ": profile=" << active_profile_
              << ", enable_gru_rewriter=" << penalty_config_.enable_gru_rewriter
              << ", enable_live_conversion=" << penalty_config_.enable_live_conversion
              << ", gru_score_weight=" << penalty_config_.gru_score_weight
              << ", enable_auto_alpha=" << penalty_config_.enable_auto_alpha
              << ", alpha_base=" << alpha_base_
              << ", show_gru_score=" << penalty_config_.show_gru_score
              << ", katakana_multiplier=" << penalty_config_.katakana_multiplier
              << ", hiragana_multiplier=" << penalty_config_.hiragana_multiplier
              << ", alphabet_multiplier=" << penalty_config_.alphabet_multiplier
              << ", emoji_penalty=" << penalty_config_.emoji_penalty
              << ", enable_instant_mode=" << penalty_config_.enable_instant_mode
              << ", instant_learning_rate=" << penalty_config_.instant_learning_rate;
  } else {
    CheckAlphaStateFileExternalChangeLocked();
  }

  return penalty_config_;
}

double GruRewriter::GetEffectiveBaseAlphaLocked(
    const GruPenaltyConfig& config) const {
  if (!absl::GetFlag(FLAGS_enable_gru_auto_alpha) ||
      !config.enable_auto_alpha ||
      config.gru_score_weight <= 0.0) {
    return config.gru_score_weight;
  }
  return alpha_base_;
}

void GruRewriter::SyncAlphaStateOnProfileLocked(
    bool profile_or_path_changed) const {
  const double anchor = penalty_config_.gru_score_weight;
  const std::string target_alpha_path =
      config::GruConfigManager::ResolveProfileAlphaStateFilePath(active_profile_);

  if (profile_or_path_changed || alpha_state_path_ != target_alpha_path) {
    alpha_state_path_ = target_alpha_path;
    has_alpha_revert_snapshot_ = false;
    if (anchor > 0.0 && !alpha_state_path_.empty()) {
      const config::GruAlphaState state =
          config::GruConfigManager::LoadAlphaState(
              active_profile_, anchor, alpha_state_path_);
      alpha_base_ = state.alpha_base;
      alpha_anchor_ = anchor;
      alpha_update_count_ = state.update_count;
      const int64_t disk_mtime =
          config::GruConfigManager::GetFileMtime(alpha_state_path_);
      if (disk_mtime != 0 && state.update_count == 0) {
        config::GruConfigManager::ResetAlphaState(active_profile_,
                                                  alpha_state_path_);
        last_alpha_mtime_ = 0;
        loaded_has_alpha_state_ = false;
      } else {
        last_alpha_mtime_ = disk_mtime;
        loaded_has_alpha_state_ = (disk_mtime != 0 && state.update_count > 0);
      }
    } else {
      alpha_base_ = anchor;
      alpha_anchor_ = anchor;
      alpha_update_count_ = 0;
      last_alpha_mtime_ = 0;
      loaded_has_alpha_state_ = false;
    }
    return;
  }

  if (std::abs(alpha_anchor_ - anchor) > 1e-4) {
    alpha_base_ = anchor;
    alpha_anchor_ = anchor;
    alpha_update_count_ = 0;
    has_alpha_revert_snapshot_ = false;
    loaded_has_alpha_state_ = false;
    last_alpha_mtime_ = 0;
    if (!alpha_state_path_.empty()) {
      config::GruConfigManager::ResetAlphaState(active_profile_,
                                                alpha_state_path_);
    }
  }
}

void GruRewriter::CheckAlphaStateFileExternalChangeLocked() const {
  if (alpha_state_path_.empty()) {
    return;
  }
  const int64_t cur_mtime =
      config::GruConfigManager::GetFileMtime(alpha_state_path_);
  if (loaded_has_alpha_state_ && cur_mtime == 0) {
    alpha_base_ = penalty_config_.gru_score_weight;
    alpha_anchor_ = penalty_config_.gru_score_weight;
    alpha_update_count_ = 0;
    has_alpha_revert_snapshot_ = false;
    loaded_has_alpha_state_ = false;
    last_alpha_mtime_ = 0;
  } else if (cur_mtime != 0 && cur_mtime != last_alpha_mtime_) {
    const config::GruAlphaState state =
        config::GruConfigManager::LoadAlphaState(
            active_profile_, penalty_config_.gru_score_weight, alpha_state_path_);
    alpha_base_ = state.alpha_base;
    alpha_anchor_ = state.anchor_alpha;
    alpha_update_count_ = state.update_count;
    has_alpha_revert_snapshot_ = false;
    loaded_has_alpha_state_ = (state.update_count > 0);
    last_alpha_mtime_ = cur_mtime;
  }
}

bool GruRewriter::PrepareRuntimeTuning(const commands::Context* context,
                                       GruPenaltyConfig* out_config,
                                       double* out_alpha_base,
                                       bool* out_enable_auto_alpha) const {
  GruPenaltyConfig cfg = GetPenaltyConfig();
  if (!cfg.enable_gru_rewriter) {
    return false;
  }
  if (!EnsureInitialized()) {
    return false;
  }
  absl::MutexLock l(mutex_);
  CheckAndApplyAppProfileLocked(context);
  if (out_config != nullptr) {
    *out_config = penalty_config_;
  }
  if (out_alpha_base != nullptr) {
    *out_alpha_base = GetEffectiveBaseAlphaLocked(penalty_config_);
  }
  if (out_enable_auto_alpha != nullptr) {
    *out_enable_auto_alpha = absl::GetFlag(FLAGS_enable_gru_auto_alpha) &&
                             penalty_config_.enable_auto_alpha &&
                             penalty_config_.gru_score_weight > 0.0;
  }
  return true;
}

void GruRewriter::CheckAndApplyAppProfileLocked(
    const commands::Context* context) const {
  if (!root_penalty_config_.enable_app_profile_switching) {
    return;
  }
  const std::string app_name =
      config::GruConfigManager::GetCurrentApplicationName(context);
  if (app_name.empty()) {
    return;
  }
  const std::string target_profile =
      config::GruConfigManager::DetectProfileFromApplication(
          app_name, root_penalty_config_);
  const std::string effective_profile =
      target_profile.empty() ? root_penalty_config_.current_profile : target_profile;
  const std::string final_profile =
      effective_profile.empty() ? "default" : effective_profile;
  if (final_profile != active_profile_) {
    SwitchProfileLocked(final_profile, root_penalty_config_);
  }
}

bool GruRewriter::SwitchProfileLocked(
    const std::string& new_profile,
    const GruPenaltyConfig& root_config) const {
  const std::string effective_profile =
      new_profile.empty() ? "default" : new_profile;

  penalty_config_ = config::GruConfigManager::GetEffectiveProfileConfig(
      root_config, effective_profile);

  const std::string target_user_model_path =
      !penalty_config_.gru_user_model_path.empty()
          ? penalty_config_.gru_user_model_path
          : config::GruConfigManager::ResolveProfileUserModelFilePath(
                effective_profile, root_config.gru_user_model_path);

  bool profile_or_path_changed = false;
  if (active_profile_ != effective_profile || user_model_path_ != target_user_model_path) {
    if (model_ && unsaved_updates_count_ > 0 && !user_model_path_.empty()) {
      model_->SaveModel(user_model_path_);
      unsaved_updates_count_ = 0;
    }

    active_profile_ = effective_profile;
    user_model_path_ = target_user_model_path;
    profile_or_path_changed = true;

    if (model_ && model_->is_loaded()) {
      loaded_has_user_model_ = model_->SwapUserWeights(user_model_path_);
      last_revert_id_ = 0;
      LOG(INFO) << "GruRewriter: Swapped active profile to '" << active_profile_
                << "' with user model " << user_model_path_;
    }
  }
  SyncAlphaStateOnProfileLocked(profile_or_path_changed);
  return true;
}

bool GruRewriter::EnsureInitialized() const {
  absl::MutexLock l(mutex_);

  const std::string configured_chardata =
      !penalty_config_.gru_chardata_path.empty()
          ? penalty_config_.gru_chardata_path
          : absl::GetFlag(FLAGS_gru_chardata_path);
  const std::string configured_model =
      !penalty_config_.gru_model_path.empty()
          ? penalty_config_.gru_model_path
          : absl::GetFlag(FLAGS_gru_model_path);
  const std::string configured_user_model =
      !penalty_config_.gru_user_model_path.empty()
          ? penalty_config_.gru_user_model_path
          : absl::GetFlag(FLAGS_gru_user_model_path);

  const std::string chardata_path =
      config::GruConfigManager::ResolveDataFilePath(
          configured_chardata, "chardata.bin");
  const std::string model_path =
      config::GruConfigManager::ResolveDataFilePath(
          configured_model, "grumodel.bin");
  const std::string user_model_path =
      !penalty_config_.gru_user_model_path.empty()
          ? penalty_config_.gru_user_model_path
          : config::GruConfigManager::ResolveProfileUserModelFilePath(
                active_profile_, configured_user_model);

  if (is_initialized_ && model_ && model_->is_loaded() && indexer_ &&
      loaded_chardata_path_ == chardata_path &&
      loaded_model_path_ == model_path) {
    if (user_model_path_ != user_model_path) {
      if (unsaved_updates_count_ > 0 && !user_model_path_.empty()) {
        model_->SaveModel(user_model_path_);
        unsaved_updates_count_ = 0;
      }
      user_model_path_ = user_model_path;
      loaded_has_user_model_ = model_->SwapUserWeights(user_model_path_);
      last_revert_id_ = 0;
      LOG(INFO) << "GruRewriter: Swapped user model to " << user_model_path;
    } else if (loaded_has_user_model_ && unsaved_updates_count_ == 0 &&
               !user_model_path_.empty() &&
               !std::filesystem::exists(user_model_path_)) {
      model_->ResetToBaseWeights();
      loaded_has_user_model_ = false;
      last_revert_id_ = 0;
      LOG(INFO) << "GruRewriter: Reset weights to base model after user model removal.";
    }
    return true;
  }

  is_initialized_ = true;
  loaded_chardata_path_ = chardata_path;
  loaded_model_path_ = model_path;
  user_model_path_ = user_model_path;
  unsaved_updates_count_ = 0;
  last_revert_id_ = 0;
  loaded_has_user_model_ = false;
  model_.reset();
  indexer_.reset();

  if (chardata_path.empty()) {
    MOZC_VLOG(1) << "GruRewriter: chardata.bin not found.";
    return false;
  }

  auto indexer = std::make_unique<charindex::CharIndex>();
  if (!indexer->LoadFile(chardata_path)) {
    LOG(WARNING) << "GruRewriter: Failed to load chardata: " << chardata_path;
    return false;
  }

  if (model_path.empty()) {
    MOZC_VLOG(1) << "GruRewriter: grumodel.bin not found. GruRewriter disabled.";
    return false;
  }

  auto model = std::make_unique<gruscorer::GruModel>();
  if (!model->LoadModel(model_path)) {
    LOG(WARNING) << "GruRewriter: Failed to load model: " << model_path;
    return false;
  }

  if (!user_model_path_.empty() && std::filesystem::exists(user_model_path_)) {
    if (model->LoadUserWeights(user_model_path_)) {
      loaded_has_user_model_ = true;
      LOG(INFO) << "GruRewriter: Loaded user-adapted GRU weights from " << user_model_path_;
    } else {
      LOG(WARNING) << "GruRewriter: Ignored incompatible or corrupted user model at "
                   << user_model_path_;
    }
  }

  model_ = std::move(model);
  indexer_ = std::move(indexer);
  LOG(INFO) << "GruRewriter: Successfully loaded GRU model from " << model_path;
  return true;
}

bool GruRewriter::Reload() {
  Sync();
  {
    absl::MutexLock l(mutex_);
    is_initialized_ = false;
    loaded_chardata_path_.clear();
    loaded_model_path_.clear();
    user_model_path_.clear();
    config_file_path_.clear();
    alpha_state_path_.clear();
    last_config_mtime_ = 0;
    last_alpha_mtime_ = 0;
    last_config_check_time_ = absl::InfinitePast();
    last_revert_id_ = 0;
    unsaved_updates_count_ = 0;
    loaded_has_user_model_ = false;
    loaded_has_alpha_state_ = false;
    has_alpha_revert_snapshot_ = false;
    model_.reset();
    indexer_.reset();
  }
  (void)GetPenaltyConfig();
  return EnsureInitialized();
}

bool GruRewriter::UpdateAlphaOnFinishLocked(
    const Segments& segments,
    const std::string& hist_prefix,
    const GruPenaltyConfig& penalty_config) const {
  if (!model_ || !model_->is_loaded() || !indexer_) {
    return false;
  }
  const double anchor_alpha = penalty_config.gru_score_weight;
  if (!std::isfinite(anchor_alpha) || anchor_alpha <= 0.0) {
    return false;
  }

  prev_alpha_base_ = alpha_base_;
  prev_alpha_update_count_ = alpha_update_count_;
  has_alpha_revert_snapshot_ = false;

  const size_t start_seg_idx = segments.history_segments_size();
  const size_t num_total = segments.segments_size();
  const size_t num_target =
      (num_total > start_seg_idx) ? (num_total - start_seg_idx) : 0;
  const size_t stat_limit =
      (num_target > 1) ? kMaxCandidatesPerSegmentForBeam : kMaxCandidateEvaluation;

  std::string accum_text;
  double total_delta = 0.0;
  bool any_signal = false;

  for (size_t i = start_seg_idx; i < num_total; ++i) {
    const Segment& seg = segments.segment(i);
    if (seg.candidates_size() == 0 || seg.segment_type() != Segment::FIXED_VALUE) {
      continue;
    }
    const Segment::Candidate& chosen = seg.candidate(0);
    const std::string chosen_val = chosen.value;

    const bool skip_seg =
        IsSymbolSegment(seg) || IsEnglishFallbackSegment(seg) ||
        seg.candidates_size() <= 1 ||
        ((chosen.attributes & (converter::Attribute::NO_HISTORY_LEARNING |
                               converter::Attribute::COMMAND_CANDIDATE)) != 0);
    if (skip_seg) {
      accum_text.append(chosen_val);
      continue;
    }

    const size_t num_eval = std::min(seg.candidates_size(), kMaxCandidateEvaluation);
    int32_t min_cost = chosen.cost;
    for (size_t j = 1; j < num_eval; ++j) {
      min_cost = std::min(min_cost, seg.candidate(j).cost);
    }
    if (chosen.cost > min_cost + kMaxCostDiff) {
      accum_text.append(chosen_val);
      continue;
    }

    const gruscorer::GruContext seg_ctx =
        ComputePrefixContext(*model_, *indexer_, hist_prefix, accum_text);
    const size_t prefix_len = ComputeEffectivePrefixChars(hist_prefix, accum_text);

    double alpha_eff = alpha_base_;
    std::vector<CandidateScore> scored = EvaluateSegmentCandidates(
        *model_, *indexer_, seg, seg_ctx, prefix_len, num_eval, stat_limit,
        min_cost, penalty_config, alpha_base_, /*enable_auto_alpha=*/true,
        &alpha_eff);

    size_t best_rival_idx = 0;
    bool found_rival = false;
    for (size_t j = 1; j < scored.size(); ++j) {
      if (!scored[j].within_cost_diff) {
        continue;
      }
      if (!found_rival || scored[j].combined_cost < scored[best_rival_idx].combined_cost) {
        best_rival_idx = j;
        found_rival = true;
      }
    }

    if (!found_rival) {
      accum_text.append(chosen_val);
      continue;
    }

    const CandidateScore& c_star = scored[0];
    const CandidateScore& c_rival = scored[best_rival_idx];
    const double m = alpha_eff / alpha_base_;
    const double margin_cur = c_rival.combined_cost - c_star.combined_cost;
    const double delta_s =
        static_cast<double>(c_star.gru_score) - static_cast<double>(c_rival.gru_score);

    const bool was_reranked =
        (chosen.attributes & converter::Attribute::RERANKED) != 0;
    constexpr double kErrTargetMargin = 200.0;
    constexpr double kPosTargetMargin = 200.0;
    constexpr double kErrStepRate = 0.50;
    constexpr double kPosStepRate = 0.20;
    constexpr double kPaReg = 0.25;

    if (was_reranked || margin_cur < 0.0) {
      const double loss = std::max(0.0, kErrTargetMargin - margin_cur);
      if (loss > 0.0 && std::abs(delta_s) > 1e-4) {
        const double eff_ds = m * delta_s;
        total_delta += kErrStepRate * (loss * eff_ds) / (eff_ds * eff_ds + kPaReg);
        any_signal = true;
      }
    } else {
      const double mozc_cost_star =
          static_cast<double>(c_star.viterbi_cost) + c_star.penalty_val;
      const double mozc_cost_rival =
          static_cast<double>(c_rival.viterbi_cost) + c_rival.penalty_val;
      const bool gru_overruled_viterbi =
          (mozc_cost_star > mozc_cost_rival) && (delta_s > 1e-4);
      if (gru_overruled_viterbi && margin_cur < kPosTargetMargin) {
        const double loss = kPosTargetMargin - margin_cur;
        const double eff_ds = m * delta_s;
        total_delta += kPosStepRate * (loss * eff_ds) / (eff_ds * eff_ds + kPaReg);
        any_signal = true;
      }
    }

    accum_text.append(chosen_val);
  }

  if (!any_signal) {
    return false;
  }

  constexpr double kAnchorPullRate = 0.005;
  total_delta -= kAnchorPullRate * (alpha_base_ - anchor_alpha);

  const double max_step = 0.04 * anchor_alpha;
  total_delta = std::clamp(total_delta, -max_step, max_step);

  alpha_base_ = config::GruConfigManager::ClampAutoAlphaBase(
      alpha_base_ + total_delta, anchor_alpha);
  alpha_anchor_ = anchor_alpha;
  ++alpha_update_count_;
  has_alpha_revert_snapshot_ = true;

  if (!alpha_state_path_.empty()) {
    config::GruAlphaState state;
    state.alpha_base = alpha_base_;
    state.anchor_alpha = alpha_anchor_;
    state.update_count = alpha_update_count_;
    if (config::GruConfigManager::SaveAlphaState(
            active_profile_, state, alpha_state_path_)) {
      loaded_has_alpha_state_ = true;
      last_alpha_mtime_ =
          config::GruConfigManager::GetFileMtime(alpha_state_path_);
    }
  }
  return true;
}

void GruRewriter::Finish(const ConversionRequest& request,
                         const Segments& segments) {
  if (!absl::GetFlag(FLAGS_enable_gru_rewriter)) {
    return;
  }

  const GruPenaltyConfig penalty_config = GetPenaltyConfig();
  if (!penalty_config.enable_gru_rewriter) {
    return;
  }

  const bool is_live_conversion =
      (request.options().composer_key_selection ==
       ConversionRequest::PREDICTION_KEY);

  // Check for MoGRU inline command candidate selection (only for normal explicit conversions)
  if (!is_live_conversion) {
    for (size_t i = 0; i < segments.segments_size(); ++i) {
      const Segment& seg = segments.segment(i);
      if (seg.candidates_size() > 0 && seg.segment_type() == Segment::FIXED_VALUE) {
        const std::string& val = seg.candidate(0).value;
        constexpr absl::string_view kProfPrefix = "MoGRU: プロファイル切替 [";
        if (absl::StartsWith(val, kProfPrefix) && absl::EndsWith(val, "]")) {
          const std::string new_profile =
              val.substr(kProfPrefix.size(), val.size() - kProfPrefix.size() - 1);
          absl::MutexLock l(mutex_);
          if (config_file_path_.empty()) {
            config_file_path_ = config::GruConfigManager::ResolveConfigFilePath(
                "", /*allow_in_test=*/true);
          }
          GruPenaltyConfig root_config =
              config::GruConfigManager::LoadConfig(config_file_path_);
          root_config.current_profile = new_profile;
          config::GruConfigManager::SaveConfig(root_config, config_file_path_);
          config::GruConfigManager::InvalidateConfigCache();
          root_penalty_config_ = root_config;
          last_config_mtime_ =
              config::GruConfigManager::GetFileMtime(config_file_path_);
          SwitchProfileLocked(new_profile, root_config);
          return;
        }
        constexpr absl::string_view kAppBindPrefix = "MoGRU: 現在のアプリ (";
        if (absl::StartsWith(val, kAppBindPrefix)) {
          constexpr absl::string_view kMiddle = ") を [";
          constexpr absl::string_view kSuffix = "] に登録";
          size_t app_start = kAppBindPrefix.size();
          size_t app_end = val.find(kMiddle, app_start);
          if (app_end != std::string::npos) {
            std::string app_name = val.substr(app_start, app_end - app_start);
            size_t prof_start = app_end + kMiddle.size();
            size_t prof_end = val.find(kSuffix, prof_start);
            if (prof_end != std::string::npos) {
              std::string target_profile = val.substr(prof_start, prof_end - prof_start);
              absl::MutexLock l(mutex_);
              if (config_file_path_.empty()) {
                config_file_path_ = config::GruConfigManager::ResolveConfigFilePath(
                    "", /*allow_in_test=*/true);
              }
              GruPenaltyConfig root_config =
                  config::GruConfigManager::LoadConfig(config_file_path_);
              config::GruConfigManager::AddOrUpdateAppProfileMapping(&root_config, app_name, target_profile);
              root_config.enable_app_profile_switching = true;
              config::GruConfigManager::SaveConfig(root_config, config_file_path_);
              config::GruConfigManager::InvalidateConfigCache();
              root_penalty_config_ = root_config;
              last_config_mtime_ =
                  config::GruConfigManager::GetFileMtime(config_file_path_);
              SwitchProfileLocked(target_profile, root_config);
              return;
            }
          }
        }
        if (val == "MoGRU: インスタント速習モード ON" ||
            val == "MoGRU: インスタント速習モード OFF") {
          const bool enable = (val == "MoGRU: インスタント速習モード ON");
          absl::MutexLock l(mutex_);
          if (config_file_path_.empty()) {
            config_file_path_ = config::GruConfigManager::ResolveConfigFilePath(
                "", /*allow_in_test=*/true);
          }
          GruPenaltyConfig root_config =
              config::GruConfigManager::LoadConfig(config_file_path_);
          const std::string active_prof =
              config::GruConfigManager::ResolveActiveProfileForContext(
                  root_config, &request.context());
          config::GruConfigManager::SetInstantModeEnabled(
              &root_config, active_prof, enable);
          config::GruConfigManager::SaveConfig(root_config, config_file_path_);
          config::GruConfigManager::InvalidateConfigCache();
          root_penalty_config_ = root_config;
          last_config_mtime_ =
              config::GruConfigManager::GetFileMtime(config_file_path_);
          SwitchProfileLocked(active_prof, root_config);
          return;
        }
      }
    }
  }

  if (request.incognito_mode()) {
    return;
  }
  if (request.config().history_learning_level() != config::Config::DEFAULT_HISTORY) {
    return;
  }

  const bool run_weight_learning =
      absl::GetFlag(FLAGS_enable_gru_online_learning) &&
      penalty_config.enable_online_learning &&
      penalty_config.online_learning_rate > 0.0;
  const bool run_auto_alpha =
      absl::GetFlag(FLAGS_enable_gru_auto_alpha) &&
      penalty_config.enable_auto_alpha &&
      !penalty_config.enable_instant_mode &&
      penalty_config.gru_score_weight > 0.0;

  if (!run_weight_learning && !run_auto_alpha) {
    return;
  }
  if (!EnsureInitialized()) {
    return;
  }

  std::string committed_text;
  const size_t start_seg_idx = segments.history_segments_size();
  for (size_t i = start_seg_idx; i < segments.segments_size(); ++i) {
    const Segment& seg = segments.segment(i);
    if (seg.candidates_size() <= 0 ||
        seg.segment_type() != Segment::FIXED_VALUE) {
      continue;
    }
    const Segment::Candidate& cand = seg.candidate(0);
    if (cand.attributes & (converter::Attribute::NO_HISTORY_LEARNING |
                           converter::Attribute::COMMAND_CANDIDATE)) {
      return;
    }
    committed_text.append(cand.value);
  }

  if (committed_text.empty()) {
    return;
  }

  if (!Util::ContainsScriptType(committed_text, Util::HIRAGANA) &&
      !Util::ContainsScriptType(committed_text, Util::KATAKANA) &&
      !Util::ContainsScriptType(committed_text, Util::KANJI)) {
    return;
  }

  const std::string hist_prefix = GetHistoryPrefix(&request, segments);

  absl::MutexLock l(mutex_);
  CheckAndApplyAppProfileLocked(&request.context());
  const GruPenaltyConfig effective_config = penalty_config_;
  if (!model_ || !model_->is_loaded() || !indexer_) {
    return;
  }

  const bool eff_run_auto_alpha =
      absl::GetFlag(FLAGS_enable_gru_auto_alpha) &&
      effective_config.enable_auto_alpha &&
      !effective_config.enable_instant_mode &&
      effective_config.gru_score_weight > 0.0;
  const bool eff_run_weight_learning =
      absl::GetFlag(FLAGS_enable_gru_online_learning) &&
      effective_config.enable_online_learning &&
      effective_config.online_learning_rate > 0.0;

  // Evaluate candidate scores with pre-update model weights before online training.
  bool alpha_updated = false;
  if (eff_run_auto_alpha) {
    alpha_updated = UpdateAlphaOnFinishLocked(segments, hist_prefix, effective_config);
  }

  if (eff_run_weight_learning) {
    const gruscorer::GruContext init_ctx =
        ComputePrefixContext(*model_, *indexer_, hist_prefix, "");

    gruscorer::TrainConfig train_cfg;
    if (effective_config.enable_instant_mode) {
      train_cfg.instant_mode = true;
      train_cfg.mode = gruscorer::TrainMode::kOutputOnly;
      train_cfg.learning_rate = static_cast<float>(effective_config.instant_learning_rate);
      train_cfg.max_weight_delta = static_cast<float>(effective_config.instant_max_weight_delta);
      train_cfg.max_grad_norm = std::max(1.0f, train_cfg.learning_rate * 2.0f);
      train_cfg.weight_decay = static_cast<float>(effective_config.online_learning_weight_decay);
    } else {
      train_cfg.instant_mode = false;
      if (effective_config.online_learning_mode == "full" ||
          effective_config.online_learning_mode == "full_bptt") {
        train_cfg.mode = gruscorer::TrainMode::kFullBptt;
      } else {
        train_cfg.mode = gruscorer::TrainMode::kOutputOnly;
      }
      train_cfg.learning_rate =
          static_cast<float>(effective_config.online_learning_rate);
      train_cfg.weight_decay =
          static_cast<float>(effective_config.online_learning_weight_decay);
      train_cfg.max_grad_norm = 1.0f;
      train_cfg.max_weight_delta = 0.0f;
    }

    model_->TrainOnStringWithContext(*indexer_, committed_text, init_ctx, train_cfg);
    ++unsaved_updates_count_;

    const bool should_save_immediately = effective_config.enable_instant_mode;
    if ((should_save_immediately ||
         (effective_config.online_learning_save_interval > 0 &&
          unsaved_updates_count_ >= effective_config.online_learning_save_interval)) &&
        !user_model_path_.empty()) {
      if (model_->SaveModel(user_model_path_)) {
        unsaved_updates_count_ = 0;
        loaded_has_user_model_ = true;
      }
    }
  }

  if (alpha_updated || eff_run_weight_learning) {
    last_revert_id_ = segments.revert_id();
  }
}

void GruRewriter::Revert(const Segments& segments) {
  absl::MutexLock l(mutex_);
  if (segments.revert_id() != 0 && segments.revert_id() != last_revert_id_) {
    return;
  }

  bool any_reverted = false;
  if (model_ && model_->is_loaded() && model_->has_revert_snapshot()) {
    if (model_->RevertLastTrainStep()) {
      any_reverted = true;
      if (unsaved_updates_count_ > 0) {
        --unsaved_updates_count_;
      } else if (!user_model_path_.empty() &&
                 std::filesystem::exists(user_model_path_)) {
        model_->SaveModel(user_model_path_);
      }
    }
  }

  if (has_alpha_revert_snapshot_) {
    alpha_base_ = prev_alpha_base_;
    alpha_update_count_ = prev_alpha_update_count_;
    has_alpha_revert_snapshot_ = false;
    any_reverted = true;
    if (!alpha_state_path_.empty()) {
      if (alpha_update_count_ == 0) {
        config::GruConfigManager::ResetAlphaState(active_profile_,
                                                  alpha_state_path_);
        loaded_has_alpha_state_ = false;
        last_alpha_mtime_ = 0;
      } else {
        config::GruAlphaState state;
        state.alpha_base = alpha_base_;
        state.anchor_alpha = alpha_anchor_;
        state.update_count = alpha_update_count_;
        if (config::GruConfigManager::SaveAlphaState(
                active_profile_, state, alpha_state_path_)) {
          last_alpha_mtime_ =
              config::GruConfigManager::GetFileMtime(alpha_state_path_);
        }
      }
    }
  }

  if (any_reverted) {
    last_revert_id_ = 0;
  }
}

bool GruRewriter::Sync() {
  absl::MutexLock l(mutex_);
  if (unsaved_updates_count_ > 0 && model_ && model_->is_loaded() &&
      !user_model_path_.empty()) {
    if (model_->SaveModel(user_model_path_)) {
      unsaved_updates_count_ = 0;
      loaded_has_user_model_ = true;
    } else {
      return false;
    }
  }
  return true;
}

void GruRewriter::Clear() {
  absl::MutexLock l(mutex_);
  if (model_ && model_->is_loaded()) {
    model_->ResetToBaseWeights();
  }
  unsaved_updates_count_ = 0;
  last_revert_id_ = 0;
  loaded_has_user_model_ = false;

  alpha_base_ = penalty_config_.gru_score_weight;
  alpha_anchor_ = penalty_config_.gru_score_weight;
  alpha_update_count_ = 0;
  has_alpha_revert_snapshot_ = false;
  loaded_has_alpha_state_ = false;
  last_alpha_mtime_ = 0;

  const std::string target_path =
      user_model_path_.empty()
          ? config::GruConfigManager::ResolveUserModelFilePath(
                absl::GetFlag(FLAGS_gru_user_model_path))
          : user_model_path_;
  if (!target_path.empty() && std::filesystem::exists(target_path)) {
    std::error_code ec;
    std::filesystem::remove(target_path, ec);
  }
  if (!alpha_state_path_.empty()) {
    config::GruConfigManager::ResetAlphaState(active_profile_, alpha_state_path_);
  }
}

bool GruRewriter::Rewrite(const ConversionRequest& request,
                          Segments* segments) const {
  if (!absl::GetFlag(FLAGS_enable_gru_rewriter)) {
    return false;
  }

  if (request.options().skip_slow_rewriters) {
    return false;
  }

  if (segments == nullptr || segments->conversion_segments_size() == 0) {
    return false;
  }

  GruPenaltyConfig penalty_config;
  double alpha_base = 800.0;
  bool enable_auto_alpha = true;
  if (!PrepareRuntimeTuning(&request.context(), &penalty_config, &alpha_base,
                            &enable_auto_alpha)) {
    return false;
  }

  const std::string hist_prefix = GetHistoryPrefix(&request, *segments);
  const bool show_score = penalty_config.show_gru_score;

  const size_t start_seg_idx = segments->history_segments_size();
  const bool modified = RerankSegmentsWithBeamSearch(
      segments, start_seg_idx, hist_prefix, penalty_config, alpha_base,
      enable_auto_alpha, show_score);

  // Inline MoGRU command candidate suggestion:
  // Must NEVER be suggested during Live Conversion (to prevent auto-commit/conversion),
  // and must be added AFTER RerankSegmentsWithBeamSearch so that GRU does not score
  // or sort them to Top-1.
  const bool is_live_conversion =
      (request.options().composer_key_selection ==
       ConversionRequest::PREDICTION_KEY);
  if (!is_live_conversion && segments->conversion_segments_size() == 1) {
    Segment* seg = segments->mutable_conversion_segment(0);
    const absl::string_view key = seg->key();
    bool match_profile =
        (key == "ぷろふぁいる" || key == "プロファイル" || key == "profile" ||
         key == "pろふぃぇ" || key == "ｐろふぃぇ");
    bool match_instant =
        (key == "いんすたんと" || key == "インスタント" || key == "instant" ||
         key == "いんsたんt" || key == "いんｓたんｔ");

    if (!match_profile && !match_instant &&
        request.composer().GetLength() > 0) {
      const std::string raw_input = request.composer().GetRawString();
      if (raw_input == "profile") {
        match_profile = true;
      } else if (raw_input == "instant") {
        match_instant = true;
      }
    }

    if (!match_profile && !match_instant) {
      for (size_t ci = 0; ci < seg->candidates_size(); ++ci) {
        const absl::string_view val = seg->candidate(ci).value;
        if (val == "プロファイル" || val == "ぷろふぁいる" ||
            val == "profile") {
          match_profile = true;
        } else if (val == "インスタント" || val == "いんすたんと" ||
                   val == "instant") {
          match_instant = true;
        }
      }
    }

    if (match_profile || match_instant) {
      auto add_cmd = [&](const std::string& val, const std::string& desc) {
        Segment::Candidate* cand = seg->add_candidate();
        cand->value = val;
        cand->content_value = val;
        cand->content_key = std::string(key);
        cand->key = std::string(key);
        cand->description = desc;
        cand->cost = 100000;
        cand->attributes |= (converter::Attribute::NO_LEARNING |
                             converter::Attribute::COMMAND_CANDIDATE |
                             converter::Attribute::NO_VARIANTS_EXPANSION);
      };

      if (match_profile) {
        const std::string current_app =
            config::GruConfigManager::GetCurrentApplicationName(&request.context());
        const std::string prof = active_profile();
        if (!current_app.empty()) {
          add_cmd(absl::StrFormat("MoGRU: 現在のアプリ (%s) を [%s] に登録",
                                  current_app, prof),
                  absl::StrFormat("アプリ「%s」使用時にプロファイル [%s] を自動適用",
                                  current_app, prof));
        }
        std::vector<std::string> avail_profiles;
        {
          absl::MutexLock l(mutex_);
          avail_profiles =
              config::GruConfigManager::GetAvailableProfiles(root_penalty_config_);
        }
        auto profile_desc = [](const std::string& p) -> std::string {
          if (p == "coding") return "プログラミング・技術向けに切替";
          if (p == "business") return "公用文・ビジネス向けに切替";
          if (p == "casual") return "日常会話・SNS向けに切替";
          if (p == "creative") return "小説・創作向けに切替";
          if (p == "instant") return "インスタント速習プロファイルに切替";
          if (p == "default") return "標準プロファイルに切替";
          return absl::StrFormat("カスタムプロファイル [%s] に切替", p);
        };
        for (const auto& p : avail_profiles) {
          if (p == "default") continue;
          add_cmd(absl::StrFormat("MoGRU: プロファイル切替 [%s]", p),
                  profile_desc(p));
        }
        add_cmd("MoGRU: プロファイル切替 [default]", profile_desc("default"));
      }
      if (match_instant) {
        if (!penalty_config.enable_instant_mode) {
          add_cmd("MoGRU: インスタント速習モード ON", "1回確定で限界学習するモードを有効化");
        } else {
          add_cmd("MoGRU: インスタント速習モード OFF", "インスタント速習モードを解除");
        }
      }
      return true;
    }
  }

  return modified;
}

bool GruRewriter::RerankSegmentsWithBeamSearch(
    Segments* segments,
    size_t start_seg_idx,
    const std::string& prefix_context,
    const GruPenaltyConfig& penalty_config,
    double alpha_base,
    bool enable_auto_alpha,
    bool show_score) const {
  if (segments == nullptr || start_seg_idx >= segments->segments_size()) {
    return false;
  }
  const size_t num_total = segments->segments_size();
  const size_t num_target = num_total - start_seg_idx;
  if (num_target == 0) {
    return false;
  }

  // Single-segment case
  if (num_target == 1) {
    Segment* seg = segments->mutable_segment(start_seg_idx);
    if (ShouldBypassSegment(*seg)) {
      return false;
    }
    const gruscorer::GruContext init_context =
        ComputePrefixContext(*model_, *indexer_, prefix_context, "");
    const size_t prefix_len = ComputeEffectivePrefixChars(prefix_context, "");

    if (seg->candidates_size() <= 1) {
      if (seg->candidates_size() == 1 && show_score) {
        auto* cand = seg->mutable_candidate(0);
        if (IsAlphabetCandidate(*cand)) {
          return false;
        }
        double alpha_eff = alpha_base;
        std::vector<CandidateScore> scored = EvaluateSegmentCandidates(
            *model_, *indexer_, *seg, init_context, prefix_len, 1, 1,
            cand->cost, penalty_config, alpha_base, enable_auto_alpha,
            &alpha_eff);
        if (!scored.empty()) {
          UpdateCandidateDescription(cand, scored[0].gru_score,
                                     scored[0].combined_cost, alpha_eff,
                                     scored[0].penalty_type,
                                     scored[0].penalty_val, false);
        }
      }
      return false;
    }

    const int32_t top_cost = seg->candidate(0).cost;
    double alpha_eff = alpha_base;
    std::vector<CandidateScore> scored = EvaluateSegmentCandidates(
        *model_, *indexer_, *seg, init_context, prefix_len,
        kMaxCandidateEvaluation, kMaxCandidateEvaluation, top_cost,
        penalty_config, alpha_base, enable_auto_alpha, &alpha_eff);

    auto best_it = std::min_element(
        scored.begin(), scored.end(),
        [](const CandidateScore& a, const CandidateScore& b) {
          return a.combined_cost < b.combined_cost;
        });
    if (best_it != scored.end()) {
      best_it->is_best = true;
    }

    return SortSegmentCandidates(seg, std::move(scored), alpha_eff, show_score);
  }

  // Multi-segment beam search optimization
  struct BeamPath {
    std::vector<size_t> indices;
    std::string text;
    int32_t total_viterbi_cost = 0;
    float gru_score = 0.0f;
    double eval_score = 0.0;
    bool has_emoji = false;
  };

  std::vector<BeamPath> beam;

  // 1. Initialize beam with first target segment candidates
  {
    const Segment& seg0 = segments->segment(start_seg_idx);
    if (seg0.candidates_size() == 0) {
      return false;
    }
    const int32_t top_cost = seg0.candidate(0).cost;
    const bool bypass_seg0 = ShouldBypassSegment(seg0);
    const size_t num_eval =
        bypass_seg0
            ? std::min<size_t>(seg0.candidates_size(), 1)
            : std::min(seg0.candidates_size(), kMaxCandidatesPerSegmentForBeam);
    const gruscorer::GruContext init_context =
        ComputePrefixContext(*model_, *indexer_, prefix_context, "");
    const size_t prefix_len = ComputeEffectivePrefixChars(prefix_context, "");

    if (bypass_seg0) {
      for (size_t j = 0; j < num_eval; ++j) {
        const auto& cand = seg0.candidate(j);
        if (cand.cost > top_cost + kMaxCostDiff) continue;
        const PenaltyResult penalty_res =
            CalculatePenalty(cand, /*is_emoji=*/false, penalty_config);
        const double eval =
            static_cast<double>(cand.cost) + penalty_res.penalty;
        beam.push_back({{j}, cand.value, cand.cost, 0.0f, eval, false});
      }
    } else {
      double alpha_eff = alpha_base;
      std::vector<CandidateScore> scored = EvaluateSegmentCandidates(
          *model_, *indexer_, seg0, init_context, prefix_len, num_eval,
          kMaxCandidatesPerSegmentForBeam, top_cost, penalty_config, alpha_base,
          enable_auto_alpha, &alpha_eff);
      for (size_t j = 0; j < scored.size(); ++j) {
        if (!scored[j].within_cost_diff) continue;
        const auto& cand = seg0.candidate(j);
        const bool is_emoji = (scored[j].penalty_type == PenaltyType::EMOJI);
        beam.push_back({{j}, cand.value, cand.cost, scored[j].gru_score,
                        scored[j].combined_cost, is_emoji});
      }
    }

    std::sort(beam.begin(), beam.end(),
              [](const BeamPath& a, const BeamPath& b) { return a.eval_score < b.eval_score; });
    if (beam.size() > kBeamSize) beam.resize(kBeamSize);
  }

  if (beam.empty()) return false;

  // 2. Expand beam through subsequent segments
  for (size_t rel_idx = 1; rel_idx < num_target; ++rel_idx) {
    const size_t seg_idx = start_seg_idx + rel_idx;
    const Segment& seg = segments->segment(seg_idx);
    const int32_t top_cost = seg.candidate(0).cost;
    const bool bypass_seg = ShouldBypassSegment(seg);
    const size_t num_eval =
        bypass_seg
            ? std::min<size_t>(seg.candidates_size(), 1)
            : std::min(seg.candidates_size(), kMaxCandidatesPerSegmentForBeam);

    std::vector<BeamPath> next_beam;
    next_beam.reserve(beam.size() * num_eval);

    for (const auto& path : beam) {
      if (bypass_seg) {
        for (size_t j = 0; j < num_eval; ++j) {
          const auto& cand = seg.candidate(j);
          if (cand.cost > top_cost + kMaxCostDiff) continue;
          const PenaltyResult penalty_res =
              CalculatePenalty(cand, path.has_emoji, penalty_config);
          const double cand_eval =
              static_cast<double>(cand.cost) + penalty_res.penalty;
          std::vector<size_t> new_indices = path.indices;
          new_indices.push_back(j);
          next_beam.push_back({std::move(new_indices), path.text + cand.value,
                               path.total_viterbi_cost + cand.cost, 0.0f,
                               path.eval_score + cand_eval, path.has_emoji});
        }
        continue;
      }

      const gruscorer::GruContext path_ctx =
          ComputePrefixContext(*model_, *indexer_, prefix_context, path.text);
      const size_t prefix_len =
          ComputeEffectivePrefixChars(prefix_context, path.text);
      double alpha_eff = alpha_base;
      std::vector<CandidateScore> scored = EvaluateSegmentCandidates(
          *model_, *indexer_, seg, path_ctx, prefix_len, num_eval,
          kMaxCandidatesPerSegmentForBeam, top_cost, penalty_config, alpha_base,
          enable_auto_alpha, &alpha_eff);

      for (size_t j = 0; j < scored.size(); ++j) {
        if (!scored[j].within_cost_diff) continue;
        const auto& cand = seg.candidate(j);
        const bool is_emoji =
            path.has_emoji || (scored[j].penalty_type == PenaltyType::EMOJI);
        double cand_eval = scored[j].combined_cost;
        if (path.has_emoji && scored[j].penalty_type != PenaltyType::EMOJI) {
          cand_eval += penalty_config.emoji_penalty;
        }
        std::vector<size_t> new_indices = path.indices;
        new_indices.push_back(j);
        next_beam.push_back({std::move(new_indices), path.text + cand.value,
                             path.total_viterbi_cost + cand.cost,
                             scored[j].gru_score, path.eval_score + cand_eval,
                             is_emoji});
      }
    }

    if (next_beam.empty()) break;

    std::sort(next_beam.begin(), next_beam.end(),
              [](const BeamPath& a, const BeamPath& b) { return a.eval_score < b.eval_score; });
    if (next_beam.size() > kBeamSize) next_beam.resize(kBeamSize);
    beam = std::move(next_beam);
  }

  if (beam.empty()) return false;

  const BeamPath& best_path = beam[0];

  // 3. Reorder candidates in each segment based on best_path selection
  bool any_modified = false;
  std::string accum_text;

  for (size_t rel_idx = 0; rel_idx < num_target; ++rel_idx) {
    const size_t seg_idx = start_seg_idx + rel_idx;
    Segment* seg = segments->mutable_segment(seg_idx);
    if (ShouldBypassSegment(*seg) || seg->candidates_size() <= 1) {
      if (seg->candidates_size() > 0) {
        accum_text += seg->candidate(0).value;
      }
      continue;
    }

    const size_t best_cand_idx = (rel_idx < best_path.indices.size()) ? best_path.indices[rel_idx] : 0;
    const gruscorer::GruContext seg_ctx =
        ComputePrefixContext(*model_, *indexer_, prefix_context, accum_text);
    const size_t prefix_len =
        ComputeEffectivePrefixChars(prefix_context, accum_text);

    const int32_t top_cost = seg->candidate(0).cost;
    double alpha_eff = alpha_base;
    std::vector<CandidateScore> scored = EvaluateSegmentCandidates(
        *model_, *indexer_, *seg, seg_ctx, prefix_len, kMaxCandidateEvaluation,
        kMaxCandidatesPerSegmentForBeam, top_cost, penalty_config, alpha_base,
        enable_auto_alpha, &alpha_eff);
    if (best_cand_idx < scored.size()) {
      scored[best_cand_idx].is_best = true;
    }

    if (SortSegmentCandidates(seg, std::move(scored), alpha_eff, show_score)) {
      any_modified = true;
    }

    accum_text += seg->candidate(0).value;
  }

  return any_modified;
}

bool GruRewriter::Focus(Segments* segments, size_t segment_index,
                        int candidate_index) const {
  if (segments == nullptr) {
    return false;
  }
  if (!absl::GetFlag(FLAGS_enable_gru_rewriter)) {
    return false;
  }
  if (segment_index >= segments->segments_size()) {
    return false;
  }
  const size_t next_segment_index = segment_index + 1;
  if (next_segment_index >= segments->segments_size()) {
    return false;
  }

  Segment* focused_seg = segments->mutable_segment(segment_index);
  if (candidate_index < 0 || candidate_index >= focused_seg->candidates_size()) {
    return false;
  }

  GruPenaltyConfig penalty_config;
  double alpha_base = 800.0;
  bool enable_auto_alpha = true;
  if (!PrepareRuntimeTuning(nullptr, &penalty_config, &alpha_base,
                            &enable_auto_alpha)) {
    return false;
  }

  const bool show_score = penalty_config.show_gru_score;

  // Build prefix text up to segment_index, incorporating the chosen candidate.
  std::string prefix_text;
  if (segments->history_segments_size() == 0 &&
      !segments->preceding_text().empty()) {
    prefix_text.append(segments->preceding_text());
  }
  for (size_t i = 0; i < segment_index; ++i) {
    const Segment& seg = segments->segment(i);
    if (seg.candidates_size() > 0) {
      prefix_text.append(seg.candidate(0).value);
    }
  }
  prefix_text.append(focused_seg->candidate(candidate_index).value);

  bool modified = false;

  // 1. Resegment subsequent segments if enabled and supported.
  if (penalty_config.enable_resegmentation && immutable_converter_ != nullptr) {
    auto reseg_result = EvaluateResegmentation(
        *segments, next_segment_index, prefix_text, penalty_config, alpha_base,
        enable_auto_alpha, segment_index, candidate_index);
    if (reseg_result.has_value()) {
      MOZC_VLOG(1) << "GruRewriter::Focus triggered resegmentation: diff="
                   << reseg_result->score_diff;
      Segment saved_focused_seg = *focused_seg;

      if (segments->Resize(next_segment_index, reseg_result->new_sizes)) {
        for (size_t i = 0; i <= segment_index; ++i) {
          segments->mutable_segment(i)->set_segment_type(Segment::FIXED_VALUE);
        }
        ConversionOptions conv_options;
        conv_options.request_type = RequestType::CONVERSION;
        immutable_converter_->Convert(conv_options, segments);

        // Restore original focused segment state
        *segments->mutable_segment(segment_index) = std::move(saved_focused_seg);
        modified = true;
      }
    }
  }

  // 2. Rerank subsequent segments based on the chosen candidate prefix.
  modified |= RerankSegmentsWithBeamSearch(
      segments, next_segment_index, prefix_text, penalty_config, alpha_base,
      enable_auto_alpha, show_score);
  return modified;
}

namespace {

std::optional<RewriterInterface::ResizeSegmentsRequest> BuildResizeSegmentsRequest(
    size_t segment_index, const std::vector<uint8_t>& new_sizes) {
  RewriterInterface::ResizeSegmentsRequest resize_req;
  if (new_sizes.size() > resize_req.segment_sizes.size()) {
    return std::nullopt;
  }
  resize_req.segment_index = segment_index;
  for (size_t i = 0; i < new_sizes.size(); ++i) {
    resize_req.segment_sizes[i] = new_sizes[i];
  }
  return resize_req;
}

std::string SegmentTopText(const Segment& seg) {
  if (seg.candidates_size() > 0) {
    return seg.candidate(0).value;
  }
  return std::string(seg.key());
}

}  // namespace

bool GruRewriter::CanCheckConversionResegmentation(
    const ConversionRequest& request, const Segments& segments,
    const bool allow_when_resized) const {
  if (!absl::GetFlag(FLAGS_enable_gru_rewriter)) {
    return false;
  }
  if (request.options().skip_slow_rewriters) {
    return false;
  }
  if (!allow_when_resized && segments.resized()) {
    return false;
  }
  if (segments.conversion_segments_size() == 0) {
    return false;
  }
  if (request.request_type() != ConversionRequest::CONVERSION) {
    return false;
  }
  return true;
}

std::optional<GruPenaltyConfig> GruRewriter::PrepareResegmentationConfig(
    const commands::Context* context,
    double* out_alpha_base,
    bool* out_enable_auto_alpha) const {
  if (immutable_converter_ == nullptr) {
    return std::nullopt;
  }
  GruPenaltyConfig penalty_config;
  if (!PrepareRuntimeTuning(context, &penalty_config, out_alpha_base,
                            out_enable_auto_alpha) ||
      !penalty_config.enable_resegmentation) {
    return std::nullopt;
  }
  return penalty_config;
}

std::optional<RewriterInterface::ResizeSegmentsRequest>
GruRewriter::CheckResizeSegmentsRequest(const ConversionRequest& request,
                                        const Segments& segments) const {
  if (!CanCheckConversionResegmentation(request, segments,
                                        /*allow_when_resized=*/false)) {
    return std::nullopt;
  }

  // Merge segments when typing inline command keywords ("profile", "instant")
  // in Japanese Romaji mode if they were split by the converter.
  const bool is_live_conversion =
      (request.options().composer_key_selection ==
       ConversionRequest::PREDICTION_KEY);
  if (!is_live_conversion && segments.conversion_segments_size() > 1) {
    const std::string raw_input = request.composer().GetLength() > 0
                                      ? request.composer().GetRawString()
                                      : std::string();
    std::string concat_key;
    size_t total_char_len = 0;
    for (size_t i = 0; i < segments.conversion_segments_size(); ++i) {
      const absl::string_view k = segments.conversion_segment(i).key();
      concat_key.append(k.data(), k.size());
      total_char_len += Util::CharsLen(k);
    }
    if ((raw_input == "profile" || raw_input == "instant" ||
         concat_key == "pろふぃぇ" || concat_key == "ｐろふぃぇ" ||
         concat_key == "いんsたんt" || concat_key == "いんｓたんｔ" ||
         concat_key == "ぷろふぁいる" || concat_key == "いんすたんと") &&
        total_char_len > 0 && total_char_len <= 255) {
      ResizeSegmentsRequest resize_req;
      resize_req.segment_index = 0;
      resize_req.segment_sizes[0] = static_cast<uint8_t>(total_char_len);
      return resize_req;
    }
  }

  double alpha_base = 800.0;
  bool enable_auto_alpha = true;
  const std::optional<GruPenaltyConfig> penalty_config =
      PrepareResegmentationConfig(&request.context(), &alpha_base,
                                  &enable_auto_alpha);
  if (!penalty_config.has_value()) {
    return std::nullopt;
  }

  const std::string hist_prefix = GetHistoryPrefix(&request, segments);
  const size_t start_seg_idx = segments.history_segments_size();

  auto result = EvaluateResegmentation(
      segments, start_seg_idx, hist_prefix, *penalty_config, alpha_base,
      enable_auto_alpha);
  if (!result.has_value()) {
    return std::nullopt;
  }

  return BuildResizeSegmentsRequest(0, result->new_sizes);
}

std::optional<RewriterInterface::ResizeSegmentsRequest>
GruRewriter::CheckAdditionalResizeSegmentsRequest(
    const ConversionRequest& request, const Segments& segments) const {
  if (!CanCheckConversionResegmentation(request, segments,
                                        /*allow_when_resized=*/true)) {
    return std::nullopt;
  }

  double alpha_base = 800.0;
  bool enable_auto_alpha = true;
  const std::optional<GruPenaltyConfig> penalty_config =
      PrepareResegmentationConfig(&request.context(), &alpha_base,
                                  &enable_auto_alpha);
  if (!penalty_config.has_value()) {
    return std::nullopt;
  }

  std::string running_prefix = GetHistoryPrefix(&request, segments);
  const size_t history_size = segments.history_segments_size();
  const size_t total_size = segments.segments_size();

  auto is_resegmentable_free = [](const Segment& seg) {
    return seg.segment_type() == Segment::FREE && !ShouldBypassSegment(seg);
  };

  for (size_t idx = history_size; idx < total_size;) {
    const Segment& seg = segments.segment(idx);
    if (!is_resegmentable_free(seg)) {
      running_prefix.append(SegmentTopText(seg));
      ++idx;
      continue;
    }

    const size_t span_start = idx;
    while (idx < total_size && is_resegmentable_free(segments.segment(idx))) {
      ++idx;
    }
    const size_t span_end = idx;

    auto result = EvaluateResegmentation(
        segments, span_start, running_prefix, *penalty_config, alpha_base,
        enable_auto_alpha, static_cast<size_t>(-1), 0, span_end);
    if (result.has_value()) {
      return BuildResizeSegmentsRequest(span_start - history_size,
                                        result->new_sizes);
    }

    for (size_t j = span_start; j < span_end; ++j) {
      running_prefix.append(SegmentTopText(segments.segment(j)));
    }
  }

  return std::nullopt;
}

std::optional<GruRewriter::ResegmentResult> GruRewriter::EvaluateResegmentation(
    const Segments& segments,
    size_t start_seg_idx,
    const std::string& prefix_context,
    const GruPenaltyConfig& penalty_config,
    double alpha,
    bool enable_auto_alpha,
    size_t focused_seg_idx,
    int focused_cand_idx,
    size_t end_seg_idx) const {
  if (immutable_converter_ == nullptr) {
    return std::nullopt;
  }

  const size_t num_total = std::min(end_seg_idx, segments.segments_size());
  if (start_seg_idx >= num_total) {
    return std::nullopt;
  }

  std::string target_key;
  std::vector<uint8_t> default_sizes;
  for (size_t i = start_seg_idx; i < num_total; ++i) {
    const Segment& seg = segments.segment(i);
    if (ShouldBypassSegment(seg)) {
      return std::nullopt;
    }
    target_key.append(seg.key());
    const size_t seg_char_len = Util::CharsLen(seg.key());
    if (seg_char_len == 0 || seg_char_len > 255) {
      return std::nullopt;
    }
    default_sizes.push_back(static_cast<uint8_t>(seg_char_len));
  }

  const size_t total_char_len = Util::CharsLen(target_key);
  if (total_char_len < 3 || total_char_len > 60) {
    return std::nullopt;
  }

  // Build simulation Segments
  Segments tmp_segments;
  for (size_t i = 0; i < start_seg_idx; ++i) {
    Segment* copy_seg = tmp_segments.add_segment();
    *copy_seg = segments.segment(i);
    copy_seg->set_segment_type(Segment::FIXED_VALUE);
    if (i == focused_seg_idx && focused_cand_idx > 0 &&
        focused_cand_idx < copy_seg->candidates_size()) {
      copy_seg->move_candidate(focused_cand_idx, 0);
    }
  }
  Segment* target_seg = tmp_segments.add_segment();
  target_seg->set_key(target_key);
  target_seg->set_segment_type(Segment::FREE);

  ConversionOptions options;
  options.request_type = RequestType::PREDICTION;
  options.create_partial_candidates = false;
  constexpr size_t kMaxCandidatesForResegment = 8;
  options.max_conversion_candidates_size = kMaxCandidatesForResegment;

  if (!immutable_converter_->Convert(options, &tmp_segments)) {
    return std::nullopt;
  }

  if (tmp_segments.segments_size() <= start_seg_idx) {
    return std::nullopt;
  }
  const Segment& sim_seg = tmp_segments.segment(start_seg_idx);
  if (sim_seg.candidates_size() == 0) {
    return std::nullopt;
  }

  const gruscorer::GruContext init_context =
      ComputePrefixContext(*model_, *indexer_, prefix_context, "");

  // Evaluate default path cost.
  std::string default_top_text;
  int32_t default_total_cost = 0;
  for (size_t i = start_seg_idx; i < num_total; ++i) {
    const Segment& seg = segments.segment(i);
    if (seg.candidates_size() > 0) {
      default_top_text.append(seg.candidate(0).value);
      default_total_cost += seg.candidate(0).cost;
    }
  }

  double default_eval_score = 1e9;
  if (!default_top_text.empty()) {
    Segment::Candidate default_cand;
    default_cand.value = default_top_text;
    default_cand.cost = default_total_cost;
    const bool is_emoji = IsCandidateEmoji(*indexer_, default_cand);
    const float score = ScoreCandidateWithContext(
        *model_, *indexer_, default_cand, init_context,
        penalty_config);
    const PenaltyResult penalty_res =
        CalculatePenalty(default_cand, is_emoji, penalty_config);
    default_eval_score = static_cast<double>(default_cand.cost) -
                         (alpha * score) + penalty_res.penalty;
  }

  double best_eval_score = default_eval_score;
  std::vector<uint8_t> best_sizes = default_sizes;

  for (int i = 0; i < sim_seg.candidates_size(); ++i) {
    const Segment::Candidate& cand = sim_seg.candidate(i);
    std::vector<uint8_t> cand_sizes;
    size_t cand_sizes_sum = 0;
    for (const auto& inner : cand.inner_segments()) {
      const size_t len = Util::CharsLen(inner.GetKey());
      if (len == 0 || len > 255) {
        cand_sizes.clear();
        break;
      }
      cand_sizes.push_back(static_cast<uint8_t>(len));
      cand_sizes_sum += len;
    }
    if (cand_sizes.empty() || cand_sizes_sum != total_char_len) {
      continue;
    }

    const bool is_emoji = IsCandidateEmoji(*indexer_, cand);
    const float score = ScoreCandidateWithContext(
        *model_, *indexer_, cand, init_context,
        penalty_config);
    const PenaltyResult penalty_res =
        CalculatePenalty(cand, is_emoji, penalty_config);
    const double eval_score =
        static_cast<double>(cand.cost) - (alpha * score) + penalty_res.penalty;

    if (cand_sizes == default_sizes) {
      if (eval_score < default_eval_score) {
        default_eval_score = eval_score;
      }
    }

    if (eval_score < best_eval_score) {
      best_eval_score = eval_score;
      best_sizes = std::move(cand_sizes);
    }
  }

  double effective_threshold = penalty_config.resegment_threshold;
  if (enable_auto_alpha && penalty_config.gru_score_weight > 0.0 && alpha > 0.0) {
    effective_threshold *= (alpha / penalty_config.gru_score_weight);
  }

  if (best_sizes != default_sizes &&
      (default_eval_score - best_eval_score) >= effective_threshold) {
    return ResegmentResult{std::move(best_sizes), default_eval_score - best_eval_score};
  }

  return std::nullopt;
}

}  // namespace mozc
