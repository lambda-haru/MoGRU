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

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace charindex
{
    class CharIndex;
}

namespace gruscorer
{

#pragma pack(push, 1)
struct ModelHeader
{
    char magic[4];        // "GRUM"
    uint32_t version;     // 1
    uint32_t vocab_size;  // e.g. 4096
    uint32_t hidden_size; // e.g. 64, 128, 256
    uint32_t data_type;   // 0: FP32
    uint8_t reserved[44]; // Padding to make header exactly 64 bytes
};
#pragma pack(pop)

static_assert(sizeof(ModelHeader) == 64, "ModelHeader must be exactly 64 bytes");

constexpr char kMagic[4] = {'G', 'R', 'U', 'M'};
constexpr uint32_t kCurrentVersion = 1;
constexpr uint32_t kDataTypeFp32 = 0;
constexpr uint16_t kUnkTokenId = 0;
constexpr uint16_t kBosTokenId = 1;

/**
 * @brief Context state (hidden state and last token) for prefix conditioning.
 */
struct GruContext
{
    std::vector<float> h;
    uint16_t last_token = kBosTokenId;
    bool is_empty = true;
};

/**
 * @brief Parameter scope for in-process online learning.
 */
enum class TrainMode
{
    kOutputOnly = 0, // Update only output projection (w_out_ and b_out_)
    kFullBptt = 1    // Update all parameters (Embedding + GRU + Output) via BPTT
};

/**
 * @brief Hyperparameters for online learning step.
 */
struct TrainConfig
{
    TrainMode mode = TrainMode::kOutputOnly;
    float learning_rate = 0.05f;
    float weight_decay = 0.001f; // L2 anchor regularization toward base model weights
    float max_grad_norm = 1.0f;  // Global gradient norm clipping threshold
    float max_weight_delta = 0.0f; // Clamps element-wise update: |delta_w| <= max_weight_delta (0 = disabled)
    bool instant_mode = false;   // If true, applies instant adaptation safeguards (Output-Only, delta clamping)
};

class GruModel
{
public:
    GruModel();
    ~GruModel();

    // Disable copy, allow move
    GruModel(const GruModel &) = delete;
    GruModel &operator=(const GruModel &) = delete;
    GruModel(GruModel &&) noexcept;
    GruModel &operator=(GruModel &&) noexcept;

    /**
     * @brief Load base weights binary from file and initialize base weight anchor.
     * @return true on success, false otherwise.
     */
    bool LoadModel(const std::string &path);

    /**
     * @brief Load user-adapted weights binary into active weights (keeping base anchor intact).
     * Requires base model to be loaded first and header dimensions to match.
     * @return true on success, false otherwise.
     */
    bool LoadUserWeights(const std::string &path);

    /**
     * @brief Swap active weights to a new profile user weights binary.
     * Resets active weights to base model anchor first. If path is empty or does not exist,
     * leaves active weights at base model (success). If path exists, loads the user model.
     * @return true on success, false on load error.
     */
    bool SwapUserWeights(const std::string &path);

    /**
     * @brief Save current active weights binary to file atomically (via temporary file).
     * @return true on success, false otherwise.
     */
    bool SaveModel(const std::string &path) const;

    /**
     * @brief Perform one online learning step on a committed UTF-8 string given a preceding context.
     *
     * @param indexer CharIndex instance to map characters to IDs
     * @param utf8_str Committed UTF-8 target string
     * @param context Preceding GRU context (hidden state and last token)
     * @param config Online learning hyperparameters
     * @return Pre-update average log-probability per character of utf8_str.
     */
    float TrainOnStringWithContext(const charindex::CharIndex &indexer,
                                   std::string_view utf8_str,
                                   const GruContext &context,
                                   const TrainConfig &config);

    /**
     * @brief Perform one online learning step starting from empty (<bos>) context.
     */
    float TrainOnString(const charindex::CharIndex &indexer,
                        std::string_view utf8_str,
                        const TrainConfig &config);

    /**
     * @brief Revert weights to the state immediately prior to the last TrainOnString* call.
     * @return true if a revert snapshot was available and restored, false otherwise.
     */
    bool RevertLastTrainStep();

    /**
     * @brief Reset active weights back to the initial base model weights loaded by LoadModel().
     */
    void ResetToBaseWeights();

    bool has_revert_snapshot() const noexcept { return has_revert_snapshot_; }

    /**
     * @brief Execute single step of GRU inference.
     *
     * Given token_id and h_prev, computes the next hidden state h_next,
     * and optionally the log-probabilities over all vocab_size tokens.
     *
     * @param token_id Input token index (0 to vocab_size - 1)
     * @param h_prev Previous hidden state (array of size hidden_size, or nullptr for zeros)
     * @param h_next Output next hidden state (array of size hidden_size, must not be nullptr)
     * @param log_probs_out Output log probabilities (array of size vocab_size, can be nullptr if not needed)
     */
    void Step(uint16_t token_id,
              const float *h_prev,
              float *h_next,
              float *log_probs_out) const;

    /**
     * @brief Executes GRU step and computes only the log-probability of target_token: log P(target_token | history).
     *
     * Zero-Store optimization: avoids writing back the full vocab_size LogSoftmax vector to memory.
     *
     * @param token_id Input token index (0 to vocab_size - 1)
     * @param h_prev Previous hidden state (array of size hidden_size, or nullptr for zeros)
     * @param h_next Output next hidden state (array of size hidden_size, must not be nullptr)
     * @param target_token Target token whose log probability is computed
     * @return Log-probability of target_token
     */
    float StepWithTargetLogProb(uint16_t token_id,
                                const float *h_prev,
                                float *h_next,
                                uint16_t target_token) const;

    /**
     * @brief Compute the final hidden state and last token ID for a prefix string.
     *
     * Iterates characters from <bos> without computing output log probabilities.
     * Fast forward pass to obtain context for subsequent scoring.
     */
    GruContext ComputeContext(const charindex::CharIndex &indexer,
                              std::string_view prefix) const;

    /**
     * @brief Score a UTF-8 string given a preceding GruContext.
     *
     * If context is empty, starts from <bos> and zeros hidden state (same as ScoreString).
     * Otherwise, continues from context.h and context.last_token.
     *
     * @param indexer CharIndex instance to map characters to IDs
     * @param utf8_str Input UTF-8 candidate string
     * @param context Preceding GRU context (hidden state and last token)
     * @param normalize If true, returns average log probability per character (total / token count).
     * @return Sum of log probabilities (or average if normalize is true).
     */
    float ScoreStringWithContext(const charindex::CharIndex &indexer,
                                 std::string_view utf8_str,
                                 const GruContext &context,
                                 bool normalize = false) const;

    /**
     * @brief Score a UTF-8 string by accumulating log P(c_t | c_{<t}).
     *
     * Starts with <bos> (token 1), iteratively decodes characters with CharIndex,
     * and accumulates the log probability of each next character.
     *
     * @param indexer CharIndex instance to map characters to IDs
     * @param utf8_str Input UTF-8 candidate string
     * @param initial_h Optional initial hidden state (size hidden_size). If nullptr, starts with zeros.
     * @param normalize If true, returns average log probability per character (total / token count).
     * @return Sum of log probabilities (or average if normalize is true).
     */
    float ScoreString(const charindex::CharIndex &indexer,
                      std::string_view utf8_str,
                      const float *initial_h = nullptr,
                      bool normalize = false) const;

    bool is_loaded() const noexcept { return is_loaded_; }
    uint32_t vocab_size() const noexcept { return vocab_size_; }
    uint32_t hidden_size() const noexcept { return hidden_size_; }
    bool is_fp16_active() const noexcept { return use_fp16_; }
    bool has_precomputed_input_gates() const noexcept { return precomputed_gi_ != nullptr; }

private:
    struct AlignedDeleter
    {
        void operator()(float *ptr) const noexcept;
        void operator()(uint16_t *ptr) const noexcept;
    };

    /**
     * @brief Internal helper to forward GRU cell state: computes h_next from token_id and h_prev.
     */
    void ForwardGruCell(uint16_t token_id,
                        const float *h_prev,
                        float *h_next) const;

    /**
     * @brief Precomputes W_ih * e_v + b_ih for all tokens v in [0, vocab_size - 1].
     * Eliminates ~200,000 FLOPs (Gemv for input gates) per GRU inference step.
     */
    void InitPrecomputedInputGates();

    /**
     * @brief Converts w_out_ to FP16 representation on ARM64 platforms supporting native FP16.
     */
    void InitFp16Weights();

    bool is_loaded_ = false;
    bool has_revert_snapshot_ = false;
    bool use_fp16_ = false;
    uint32_t vocab_size_ = 0;
    uint32_t hidden_size_ = 0;
    size_t total_elements_ = 0;
    uint64_t base_model_hash_ = 0;

    std::unique_ptr<float[], AlignedDeleter> weights_buffer_;
    std::unique_ptr<float[], AlignedDeleter> base_weights_buffer_;
    std::unique_ptr<float[], AlignedDeleter> revert_weights_buffer_;

    // Precomputed input gates: gi(v) = W_ih * e_v + b_ih for each v in [0, vocab_size - 1]
    std::unique_ptr<float[], AlignedDeleter> precomputed_gi_;

    // FP16 weights for output projection: w_out_ [vocab_size * hidden_size]
    std::unique_ptr<uint16_t[], AlignedDeleter> w_out_fp16_;

    // Pointers into weights_buffer_
    float *w_emb_ = nullptr; // [vocab_size, hidden_size]
    float *w_ih_ = nullptr;  // [3 * hidden_size, hidden_size]
    float *w_hh_ = nullptr;  // [3 * hidden_size, hidden_size]
    float *b_ih_ = nullptr;  // [3 * hidden_size]
    float *b_hh_ = nullptr;  // [3 * hidden_size]
    float *w_out_ = nullptr; // [vocab_size, hidden_size]
    float *b_out_ = nullptr; // [vocab_size]
};

} // namespace gruscorer
