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

#ifndef MOZC_REWRITER_GRU_REWRITER_H_
#define MOZC_REWRITER_GRU_REWRITER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "absl/base/thread_annotations.h"
#include "absl/flags/declare.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "config/gru_config.h"
#include "converter/segments.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"

ABSL_DECLARE_FLAG(bool, enable_gru_rewriter);
ABSL_DECLARE_FLAG(bool, enable_gru_resegmentation);
ABSL_DECLARE_FLAG(bool, enable_gru_online_learning);
ABSL_DECLARE_FLAG(int32_t, gru_resegment_cost_threshold);
ABSL_DECLARE_FLAG(bool, show_gru_score);
ABSL_DECLARE_FLAG(double, gru_score_weight);
ABSL_DECLARE_FLAG(double, gru_katakana_multiplier);
ABSL_DECLARE_FLAG(double, gru_hiragana_multiplier);
ABSL_DECLARE_FLAG(double, gru_hiragana_length_power);
ABSL_DECLARE_FLAG(std::string, gru_model_path);
ABSL_DECLARE_FLAG(std::string, gru_user_model_path);
ABSL_DECLARE_FLAG(std::string, gru_chardata_path);
ABSL_DECLARE_FLAG(std::string, gru_config_path);

namespace gruscorer {
class GruModel;
}
namespace charindex {
class CharIndex;
}

namespace mozc {

class ImmutableConverterInterface;

class GruRewriter : public RewriterInterface {
 public:
  explicit GruRewriter(const ImmutableConverterInterface* immutable_converter = nullptr);
  ~GruRewriter() override;

  int capability(const ConversionRequest& request) const override {
    return CONVERSION | PREDICTION;
  }

  bool Rewrite(const ConversionRequest& request,
               Segments* segments) const override;

  bool Focus(Segments* segments, size_t segment_index,
             int candidate_index) const override;

  std::optional<ResizeSegmentsRequest> CheckResizeSegmentsRequest(
      const ConversionRequest& request,
      const Segments& segments) const override;

  void Finish(const ConversionRequest& request,
              const Segments& segments) override;

  void Revert(const Segments& segments) override;

  bool Sync() override;

  // Explicitly initialize or reload models and configurations.
  bool Reload() override;

  void Clear() override;

  std::string active_profile() const {
    absl::MutexLock l(mutex_);
    return active_profile_;
  }

  GruPenaltyConfig penalty_config() const {
    absl::MutexLock l(mutex_);
    return penalty_config_;
  }

 private:
  struct ResegmentResult {
    std::vector<uint8_t> new_sizes;
    double score_diff = 0.0;
  };

  bool EnsureInitialized() const;
  GruPenaltyConfig GetPenaltyConfig() const;
  void CheckAndApplyAppProfileLocked(const commands::Context* context) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  bool SwitchProfileLocked(
      const std::string& new_profile,
      const GruPenaltyConfig& root_config) const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  std::optional<ResegmentResult> EvaluateResegmentation(
      const Segments& segments,
      size_t start_seg_idx,
      const std::string& prefix_context,
      const GruPenaltyConfig& penalty_config,
      double alpha,
      size_t focused_seg_idx = static_cast<size_t>(-1),
      int focused_cand_idx = 0) const;

  bool RerankSegmentsWithBeamSearch(
      Segments* segments,
      size_t start_seg_idx,
      const std::string& prefix_context,
      const GruPenaltyConfig& penalty_config,
      double alpha,
      bool show_score) const;

  mutable absl::Mutex mutex_;
  mutable std::unique_ptr<gruscorer::GruModel> model_ ABSL_GUARDED_BY(mutex_);
  mutable std::unique_ptr<charindex::CharIndex> indexer_ ABSL_GUARDED_BY(mutex_);
  mutable std::string loaded_model_path_ ABSL_GUARDED_BY(mutex_);
  mutable std::string loaded_chardata_path_ ABSL_GUARDED_BY(mutex_);
  mutable std::string user_model_path_ ABSL_GUARDED_BY(mutex_);
  mutable std::string active_profile_ ABSL_GUARDED_BY(mutex_) = "default";
  mutable std::string config_file_path_ ABSL_GUARDED_BY(mutex_);
  mutable GruPenaltyConfig root_penalty_config_ ABSL_GUARDED_BY(mutex_);
  mutable GruPenaltyConfig penalty_config_ ABSL_GUARDED_BY(mutex_);
  mutable int64_t last_config_mtime_ ABSL_GUARDED_BY(mutex_) = 0;
  mutable absl::Time last_config_check_time_ ABSL_GUARDED_BY(mutex_) = absl::InfinitePast();
  mutable uint64_t last_revert_id_ ABSL_GUARDED_BY(mutex_) = 0;
  mutable int32_t unsaved_updates_count_ ABSL_GUARDED_BY(mutex_) = 0;
  mutable bool loaded_has_user_model_ ABSL_GUARDED_BY(mutex_) = false;
  mutable bool is_initialized_ ABSL_GUARDED_BY(mutex_) = false;

  const ImmutableConverterInterface* immutable_converter_ = nullptr;
};

}  // namespace mozc

#endif  // MOZC_REWRITER_GRU_REWRITER_H_
