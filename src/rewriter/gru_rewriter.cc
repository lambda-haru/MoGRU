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
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
  std::string prefix;
  if (segments.history_segments_size() > 0) {
    prefix = segments.history_value();
  } else if (request != nullptr) {
    prefix = request->GetSurroundingContext().first;
  }
  return prefix;
}

gruscorer::GruContext ComputePrefixContext(
    const gruscorer::GruModel& model,
    const charindex::CharIndex& indexer,
    const std::string& hist_prefix,
    const std::string& current_prefix) {
  std::string full = hist_prefix + current_prefix;
  if (full.empty()) {
    return gruscorer::GruContext();
  }
  constexpr size_t kMaxPrefixChars = 32;
  const size_t prefix_chars = Util::CharsLen(full);
  if (prefix_chars > kMaxPrefixChars) {
    full = std::string(Util::Utf8SubString(
        full, prefix_chars - kMaxPrefixChars, kMaxPrefixChars));
  }
  return model.ComputeContext(indexer, full);
}

struct CandidateScore {
  const Segment::Candidate* cand_ptr = nullptr;
  double combined_cost = 0.0;
  float gru_score = 0.0f;
  PenaltyType penalty_type = PenaltyType::NONE;
  double penalty_val = 0.0;
  bool is_best = false;
};

void UpdateCandidateDescription(Segment::Candidate* cand,
                                float gru_score,
                                double combined_cost,
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
      : absl::StrFormat("GRU:%.2f%s[E:%.0f]", gru_score, penalty_tag, combined_cost);
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
                                 scored[i].penalty_type, scored[i].penalty_val, is_promoted);
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

    const std::string target_profile =
        root_config.current_profile.empty() ? "default" : root_config.current_profile;
    SwitchProfileLocked(target_profile, root_config);

    LOG(INFO) << "GruRewriter: Hot-reloaded configuration from "
              << config_file_path_ << ": profile=" << active_profile_
              << ", enable_gru_rewriter=" << penalty_config_.enable_gru_rewriter
              << ", enable_live_conversion=" << penalty_config_.enable_live_conversion
              << ", gru_score_weight=" << penalty_config_.gru_score_weight
              << ", show_gru_score=" << penalty_config_.show_gru_score
              << ", katakana_multiplier=" << penalty_config_.katakana_multiplier
              << ", hiragana_multiplier=" << penalty_config_.hiragana_multiplier
              << ", alphabet_multiplier=" << penalty_config_.alphabet_multiplier
              << ", emoji_penalty=" << penalty_config_.emoji_penalty
              << ", enable_instant_mode=" << penalty_config_.enable_instant_mode
              << ", instant_learning_rate=" << penalty_config_.instant_learning_rate;
  }

  return penalty_config_;
}

bool GruRewriter::SwitchProfileLocked(
    const std::string& new_profile,
    const GruPenaltyConfig& root_config) const {
  const std::string effective_profile =
      new_profile.empty() ? "default" : new_profile;

  penalty_config_ = config::GruConfigManager::GetEffectiveProfileConfig(
      root_config, effective_profile);

  const std::string configured_user_model =
      !penalty_config_.gru_user_model_path.empty()
          ? penalty_config_.gru_user_model_path
          : absl::GetFlag(FLAGS_gru_user_model_path);
  const std::string target_user_model_path =
      config::GruConfigManager::ResolveProfileUserModelFilePath(
          effective_profile, configured_user_model);

  if (active_profile_ != effective_profile || user_model_path_ != target_user_model_path) {
    if (model_ && unsaved_updates_count_ > 0 && !user_model_path_.empty()) {
      model_->SaveModel(user_model_path_);
      unsaved_updates_count_ = 0;
    }

    active_profile_ = effective_profile;
    user_model_path_ = target_user_model_path;

    if (model_ && model_->is_loaded()) {
      loaded_has_user_model_ = model_->SwapUserWeights(user_model_path_);
      last_revert_id_ = 0;
      LOG(INFO) << "GruRewriter: Swapped active profile to '" << active_profile_
                << "' with user model " << user_model_path_;
    }
  }
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
      config::GruConfigManager::ResolveProfileUserModelFilePath(
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
    last_config_mtime_ = 0;
    last_config_check_time_ = absl::InfinitePast();
    last_revert_id_ = 0;
    unsaved_updates_count_ = 0;
    loaded_has_user_model_ = false;
    model_.reset();
    indexer_.reset();
  }
  (void)GetPenaltyConfig();
  return EnsureInitialized();
}

void GruRewriter::Finish(const ConversionRequest& request,
                         const Segments& segments) {
  if (!absl::GetFlag(FLAGS_enable_gru_rewriter)) {
    return;
  }
  if (!absl::GetFlag(FLAGS_enable_gru_online_learning)) {
    return;
  }
  if (request.incognito_mode()) {
    return;
  }
  if (request.config().history_learning_level() != config::Config::DEFAULT_HISTORY) {
    return;
  }

  const GruPenaltyConfig penalty_config = GetPenaltyConfig();
  if (!penalty_config.enable_gru_rewriter ||
      !penalty_config.enable_online_learning ||
      penalty_config.online_learning_rate <= 0.0) {
    return;
  }
  if (!EnsureInitialized()) {
    return;
  }

  // Check for MoGRU inline command candidate selection
  for (size_t i = 0; i < segments.segments_size(); ++i) {
    const Segment& seg = segments.segment(i);
    if (seg.candidates_size() > 0 && seg.segment_type() == Segment::FIXED_VALUE) {
      const std::string& val = seg.candidate(0).value;
      constexpr absl::string_view kProfPrefix = "MoGRU: プロファイル切替 [";
      if (absl::StartsWith(val, kProfPrefix) && absl::EndsWith(val, "]")) {
        const std::string new_profile =
            val.substr(kProfPrefix.size(), val.size() - kProfPrefix.size() - 1);
        absl::MutexLock l(mutex_);
        penalty_config_.current_profile = new_profile;
        config::GruConfigManager::SaveConfig(penalty_config_, config_file_path_);
        SwitchProfileLocked(new_profile, penalty_config_);
        return;
      }
      if (val == "MoGRU: インスタント速習モード ON") {
        absl::MutexLock l(mutex_);
        penalty_config_.enable_instant_mode = true;
        config::GruConfigManager::SaveConfig(penalty_config_, config_file_path_);
        return;
      }
      if (val == "MoGRU: インスタント速習モード OFF") {
        absl::MutexLock l(mutex_);
        penalty_config_.enable_instant_mode = false;
        config::GruConfigManager::SaveConfig(penalty_config_, config_file_path_);
        return;
      }
    }
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
    if (cand.attributes & converter::Attribute::NO_HISTORY_LEARNING) {
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
  if (!model_ || !model_->is_loaded() || !indexer_) {
    return;
  }

  const gruscorer::GruContext init_ctx =
      ComputePrefixContext(*model_, *indexer_, hist_prefix, "");

  gruscorer::TrainConfig train_cfg;
  if (penalty_config.enable_instant_mode) {
    train_cfg.instant_mode = true;
    train_cfg.mode = gruscorer::TrainMode::kOutputOnly;
    train_cfg.learning_rate = static_cast<float>(penalty_config.instant_learning_rate);
    train_cfg.max_weight_delta = static_cast<float>(penalty_config.instant_max_weight_delta);
    train_cfg.max_grad_norm = std::max(1.0f, train_cfg.learning_rate * 2.0f);
    train_cfg.weight_decay = static_cast<float>(penalty_config.online_learning_weight_decay);
  } else {
    train_cfg.instant_mode = false;
    if (penalty_config.online_learning_mode == "full" ||
        penalty_config.online_learning_mode == "full_bptt") {
      train_cfg.mode = gruscorer::TrainMode::kFullBptt;
    } else {
      train_cfg.mode = gruscorer::TrainMode::kOutputOnly;
    }
    train_cfg.learning_rate =
        static_cast<float>(penalty_config.online_learning_rate);
    train_cfg.weight_decay =
        static_cast<float>(penalty_config.online_learning_weight_decay);
    train_cfg.max_grad_norm = 1.0f;
    train_cfg.max_weight_delta = 0.0f;
  }

  model_->TrainOnStringWithContext(*indexer_, committed_text, init_ctx, train_cfg);
  last_revert_id_ = segments.revert_id();
  ++unsaved_updates_count_;

  const bool should_save_immediately = penalty_config.enable_instant_mode;
  if ((should_save_immediately ||
       (penalty_config.online_learning_save_interval > 0 &&
        unsaved_updates_count_ >= penalty_config.online_learning_save_interval)) &&
      !user_model_path_.empty()) {
    if (model_->SaveModel(user_model_path_)) {
      unsaved_updates_count_ = 0;
      loaded_has_user_model_ = true;
    }
  }
}

void GruRewriter::Revert(const Segments& segments) {
  absl::MutexLock l(mutex_);
  if (!model_ || !model_->is_loaded() || !model_->has_revert_snapshot()) {
    return;
  }
  if (segments.revert_id() != 0 && segments.revert_id() != last_revert_id_) {
    return;
  }
  if (model_->RevertLastTrainStep()) {
    last_revert_id_ = 0;
    if (unsaved_updates_count_ > 0) {
      --unsaved_updates_count_;
    } else if (!user_model_path_.empty() &&
               std::filesystem::exists(user_model_path_)) {
      model_->SaveModel(user_model_path_);
    }
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

  const std::string target_path =
      user_model_path_.empty()
          ? config::GruConfigManager::ResolveUserModelFilePath(
                absl::GetFlag(FLAGS_gru_user_model_path))
          : user_model_path_;
  if (!target_path.empty() && std::filesystem::exists(target_path)) {
    std::error_code ec;
    std::filesystem::remove(target_path, ec);
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

  const GruPenaltyConfig penalty_config = GetPenaltyConfig();
  if (!penalty_config.enable_gru_rewriter) {
    return false;
  }

  if (!EnsureInitialized()) {
    return false;
  }

  const std::string hist_prefix = GetHistoryPrefix(&request, *segments);
  const double alpha = penalty_config.gru_score_weight;
  const bool show_score = penalty_config.show_gru_score;

  // Inline MoGRU command candidate suggestion
  if (segments->conversion_segments_size() == 1) {
    Segment* seg = segments->mutable_conversion_segment(0);
    const absl::string_view key = seg->key();
    if (key == "ぷろふぁいる" || key == "もぐら" || key == "いんすたんと") {
      auto add_cmd = [&](const std::string& val, const std::string& desc) {
        Segment::Candidate* cand = seg->add_candidate();
        cand->value = val;
        cand->content_value = val;
        cand->description = desc;
        cand->prefix = "【";
        cand->suffix = "】";
        cand->attributes |= converter::Attribute::NO_LEARNING;
        cand->attributes |= converter::Attribute::COMMAND_CANDIDATE;
      };

      if (key == "ぷろふぁいる" || key == "もぐら") {
        add_cmd("MoGRU: プロファイル切替 [coding]", "プログラミング・技術向けに切替");
        add_cmd("MoGRU: プロファイル切替 [business]", "公用文・ビジネス向けに切替");
        add_cmd("MoGRU: プロファイル切替 [casual]", "日常会話・SNS向けに切替");
        add_cmd("MoGRU: プロファイル切替 [creative]", "小説・創作向けに切替");
        add_cmd("MoGRU: プロファイル切替 [instant]", "インスタント速習プロファイルに切替");
        add_cmd("MoGRU: プロファイル切替 [default]", "標準プロファイルに切替");
      }
      if (key == "いんすたんと" || key == "もぐら") {
        if (!penalty_config.enable_instant_mode) {
          add_cmd("MoGRU: インスタント速習モード ON", "1回確定で限界学習するモードを有効化");
        } else {
          add_cmd("MoGRU: インスタント速習モード OFF", "インスタント速習モードを解除");
        }
      }
    }
  }

  const size_t start_seg_idx = segments->history_segments_size();
  return RerankSegmentsWithBeamSearch(
      segments, start_seg_idx, hist_prefix, penalty_config, alpha, show_score);
}

bool GruRewriter::RerankSegmentsWithBeamSearch(
    Segments* segments,
    size_t start_seg_idx,
    const std::string& prefix_context,
    const GruPenaltyConfig& penalty_config,
    double alpha,
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
    if (IsSymbolSegment(*seg)) {
      return false;
    }
    const gruscorer::GruContext init_context =
        ComputePrefixContext(*model_, *indexer_, prefix_context, "");

    if (seg->candidates_size() <= 1) {
      if (seg->candidates_size() == 1 && show_score) {
        auto* cand = seg->mutable_candidate(0);
        if (IsAlphabetCandidate(*cand)) {
          return false;
        }
        const bool is_emoji = IsCandidateEmoji(*indexer_, *cand);
        const float s =
            ScoreCandidateWithContext(*model_, *indexer_, *cand, init_context,
                                      penalty_config);
        const PenaltyResult penalty_res =
            CalculatePenalty(*cand, is_emoji, penalty_config);
        double eval = static_cast<double>(cand->cost) - (alpha * s) + penalty_res.penalty;
        UpdateCandidateDescription(cand, s, eval, penalty_res.type, penalty_res.penalty, false);
      }
      return false;
    }

    const int32_t top_cost = seg->candidate(0).cost;
    const size_t num_eval = std::min(seg->candidates_size(), kMaxCandidateEvaluation);

    std::vector<CandidateScore> scored;
    scored.reserve(num_eval);

    for (size_t i = 0; i < num_eval; ++i) {
      const auto& cand = seg->candidate(i);
      const bool is_emoji = IsCandidateEmoji(*indexer_, cand);
      const float gru_score =
          ScoreCandidateWithContext(*model_, *indexer_, cand, init_context,
                                    penalty_config);
      const PenaltyResult penalty_res =
          CalculatePenalty(cand, is_emoji, penalty_config);

      double combined_cost = static_cast<double>(cand.cost) - (alpha * gru_score) + penalty_res.penalty;
      if (cand.cost > top_cost + kMaxCostDiff) {
        combined_cost += 1e9;
      }

      scored.push_back({&cand, combined_cost, gru_score, penalty_res.type, penalty_res.penalty, false});
    }

    auto best_it = std::min_element(
        scored.begin(), scored.end(),
        [](const CandidateScore& a, const CandidateScore& b) {
          return a.combined_cost < b.combined_cost;
        });
    if (best_it != scored.end()) {
      best_it->is_best = true;
    }

    return SortSegmentCandidates(seg, std::move(scored), show_score);
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
    const size_t num_eval =
        IsSymbolSegment(seg0)
            ? std::min<size_t>(seg0.candidates_size(), 1)
            : std::min(seg0.candidates_size(), kMaxCandidatesPerSegmentForBeam);
    const gruscorer::GruContext init_context =
        ComputePrefixContext(*model_, *indexer_, prefix_context, "");

    for (size_t j = 0; j < num_eval; ++j) {
      const auto& cand = seg0.candidate(j);
      if (cand.cost > top_cost + kMaxCostDiff) continue;

      const bool is_emoji =
          !IsSymbolSegment(seg0) && IsCandidateEmoji(*indexer_, cand);
      const float score =
          ScoreCandidateWithContext(*model_, *indexer_, cand, init_context,
                                    penalty_config);
      const PenaltyResult penalty_res =
          CalculatePenalty(cand, is_emoji, penalty_config);

      double eval = static_cast<double>(cand.cost) - (alpha * score) + penalty_res.penalty;

      beam.push_back({{j}, cand.value, cand.cost, score, eval, is_emoji});
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
    const bool is_sym_seg = IsSymbolSegment(seg);
    const size_t num_eval =
        is_sym_seg
            ? std::min<size_t>(seg.candidates_size(), 1)
            : std::min(seg.candidates_size(), kMaxCandidatesPerSegmentForBeam);

    std::vector<BeamPath> next_beam;
    next_beam.reserve(beam.size() * num_eval);

    for (const auto& path : beam) {
      const gruscorer::GruContext path_ctx =
          ComputePrefixContext(*model_, *indexer_, prefix_context, path.text);
      for (size_t j = 0; j < num_eval; ++j) {
        const auto& cand = seg.candidate(j);
        if (cand.cost > top_cost + kMaxCostDiff) continue;

        const bool is_emoji =
            path.has_emoji || (!is_sym_seg && IsCandidateEmoji(*indexer_, cand));
        const float cand_score =
            ScoreCandidateWithContext(*model_, *indexer_, cand, path_ctx,
                                      penalty_config);
        const int32_t new_viterbi = path.total_viterbi_cost + cand.cost;

        const PenaltyResult penalty_res =
            CalculatePenalty(cand, is_emoji, penalty_config);
        double cand_eval = static_cast<double>(cand.cost) - (alpha * cand_score) + penalty_res.penalty;
        double total_eval = path.eval_score + cand_eval;

        std::vector<size_t> new_indices = path.indices;
        new_indices.push_back(j);

        next_beam.push_back({std::move(new_indices), path.text + cand.value,
                             new_viterbi, cand_score, total_eval, is_emoji});
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
    if (IsSymbolSegment(*seg) || seg->candidates_size() <= 1) {
      if (seg->candidates_size() > 0) {
        accum_text += seg->candidate(0).value;
      }
      continue;
    }

    const size_t best_cand_idx = (rel_idx < best_path.indices.size()) ? best_path.indices[rel_idx] : 0;
    const gruscorer::GruContext seg_ctx =
        ComputePrefixContext(*model_, *indexer_, prefix_context, accum_text);

    const int32_t top_cost = seg->candidate(0).cost;
    const size_t num_eval = std::min(seg->candidates_size(), kMaxCandidateEvaluation);

    std::vector<CandidateScore> scored;
    scored.reserve(num_eval);

    for (size_t j = 0; j < num_eval; ++j) {
      const auto& cand = seg->candidate(j);
      const bool is_emoji = IsCandidateEmoji(*indexer_, cand);
      const float score =
          ScoreCandidateWithContext(*model_, *indexer_, cand, seg_ctx,
                                    penalty_config);
      const PenaltyResult penalty_res =
          CalculatePenalty(cand, is_emoji, penalty_config);

      double eval = static_cast<double>(cand.cost) - (alpha * score) + penalty_res.penalty;
      if (cand.cost > top_cost + kMaxCostDiff) {
        eval += 1e9;
      }

      const bool is_best = (j == best_cand_idx);
      scored.push_back({&cand, eval, score, penalty_res.type, penalty_res.penalty, is_best});
    }

    if (SortSegmentCandidates(seg, std::move(scored), show_score)) {
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

  const GruPenaltyConfig penalty_config = GetPenaltyConfig();
  if (!penalty_config.enable_gru_rewriter) {
    return false;
  }

  if (!EnsureInitialized()) {
    return false;
  }

  const double alpha = penalty_config.gru_score_weight;
  const bool show_score = penalty_config.show_gru_score;

  // Build prefix text up to segment_index, incorporating the chosen candidate.
  std::string prefix_text;
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
        *segments, next_segment_index, prefix_text, penalty_config, alpha,
        segment_index, candidate_index);
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
      segments, next_segment_index, prefix_text, penalty_config, alpha, show_score);
  return modified;
}

std::optional<RewriterInterface::ResizeSegmentsRequest> GruRewriter::CheckResizeSegmentsRequest(
    const ConversionRequest& request,
    const Segments& segments) const {
  if (!absl::GetFlag(FLAGS_enable_gru_rewriter)) {
    return std::nullopt;
  }

  if (request.options().skip_slow_rewriters) {
    return std::nullopt;
  }

  if (segments.resized()) {
    return std::nullopt;
  }

  if (segments.conversion_segments_size() == 0) {
    return std::nullopt;
  }

  if (request.request_type() != ConversionRequest::CONVERSION) {
    return std::nullopt;
  }

  if (immutable_converter_ == nullptr) {
    return std::nullopt;
  }

  const GruPenaltyConfig penalty_config = GetPenaltyConfig();
  if (!penalty_config.enable_gru_rewriter ||
      !penalty_config.enable_resegmentation) {
    return std::nullopt;
  }

  if (!EnsureInitialized()) {
    return std::nullopt;
  }

  const std::string hist_prefix = GetHistoryPrefix(&request, segments);
  const double alpha = penalty_config.gru_score_weight;
  const size_t start_seg_idx = segments.history_segments_size();

  auto result = EvaluateResegmentation(
      segments, start_seg_idx, hist_prefix, penalty_config, alpha);
  if (!result.has_value()) {
    return std::nullopt;
  }

  ResizeSegmentsRequest resize_req;
  resize_req.segment_index = 0;
  if (result->new_sizes.size() > resize_req.segment_sizes.size()) {
    return std::nullopt;
  }
  for (size_t i = 0; i < result->new_sizes.size(); ++i) {
    resize_req.segment_sizes[i] = result->new_sizes[i];
  }
  return resize_req;
}

std::optional<GruRewriter::ResegmentResult> GruRewriter::EvaluateResegmentation(
    const Segments& segments,
    size_t start_seg_idx,
    const std::string& prefix_context,
    const GruPenaltyConfig& penalty_config,
    double alpha,
    size_t focused_seg_idx,
    int focused_cand_idx) const {
  if (immutable_converter_ == nullptr) {
    return std::nullopt;
  }

  const size_t num_total = segments.segments_size();
  if (start_seg_idx >= num_total) {
    return std::nullopt;
  }

  std::string target_key;
  std::vector<uint8_t> default_sizes;
  for (size_t i = start_seg_idx; i < num_total; ++i) {
    const Segment& seg = segments.segment(i);
    if (IsSymbolSegment(seg)) {
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
    for (const auto& inner : cand.inner_segments()) {
      const size_t len = Util::CharsLen(inner.GetKey());
      if (len == 0 || len > 255) {
        cand_sizes.clear();
        break;
      }
      cand_sizes.push_back(static_cast<uint8_t>(len));
    }
    if (cand_sizes.empty()) {
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

  if (best_sizes != default_sizes &&
      (default_eval_score - best_eval_score) >= penalty_config.resegment_threshold) {
    return ResegmentResult{std::move(best_sizes), default_eval_score - best_eval_score};
  }

  return std::nullopt;
}

}  // namespace mozc
