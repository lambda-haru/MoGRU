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

#include "gru_scorer/gru_model.hh"
#include "gru_scorer/math_ops.hh"
#include "gru_scorer/charindex.hh"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace gruscorer
{
namespace
{

float *AllocateAlignedFloats(size_t num_elements) noexcept
{
    if (num_elements == 0)
    {
        return nullptr;
    }
    const size_t total_bytes = num_elements * sizeof(float);
    float *raw_buf = nullptr;
#if defined(_MSC_VER)
    raw_buf = static_cast<float *>(_aligned_malloc(total_bytes, 64));
#else
    if (posix_memalign(reinterpret_cast<void **>(&raw_buf), 64, total_bytes) != 0)
    {
        return nullptr;
    }
#endif
    return raw_buf;
}

uint64_t ComputeFnv1a64(const void *data, size_t num_bytes, uint64_t seed = 14695981039346656037ULL) noexcept
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < num_bytes; ++i)
    {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[maybe_unused]] void *AllocateAlignedBytes(size_t total_bytes) noexcept
{
    if (total_bytes == 0)
    {
        return nullptr;
    }
    void *raw_buf = nullptr;
#if defined(_MSC_VER)
    raw_buf = _aligned_malloc(total_bytes, 64);
#else
    if (posix_memalign(&raw_buf, 64, total_bytes) != 0)
    {
        return nullptr;
    }
#endif
    return raw_buf;
}

} // namespace

void GruModel::AlignedDeleter::operator()(float *ptr) const noexcept
{
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void GruModel::AlignedDeleter::operator()(uint16_t *ptr) const noexcept
{
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

GruModel::GruModel() = default;

GruModel::~GruModel() = default;

GruModel::GruModel(GruModel &&) noexcept = default;

GruModel &GruModel::operator=(GruModel &&) noexcept = default;

bool GruModel::LoadModel(const std::string &path)
{
    is_loaded_ = false;
    has_revert_snapshot_ = false;

    std::ifstream fin(path, std::ios::binary);
    if (!fin.is_open())
    {
        return false;
    }

    ModelHeader header;
    fin.read(reinterpret_cast<char *>(&header), sizeof(ModelHeader));
    if (fin.gcount() != sizeof(ModelHeader))
    {
        return false;
    }

    if (std::memcmp(header.magic, kMagic, 4) != 0)
    {
        return false;
    }

    if (header.version != kCurrentVersion || header.data_type != kDataTypeFp32)
    {
        return false;
    }

    vocab_size_ = header.vocab_size;
    hidden_size_ = header.hidden_size;

    const size_t H = hidden_size_;
    const size_t V = vocab_size_;

    const size_t size_emb = V * H;
    const size_t size_ih  = 3 * H * H;
    const size_t size_hh  = 3 * H * H;
    const size_t size_bih = 3 * H;
    const size_t size_bhh = 3 * H;
    const size_t size_out = V * H;
    const size_t size_bout = V;

    const size_t total_elements = size_emb + size_ih + size_hh + size_bih + size_bhh + size_out + size_bout;
    const size_t total_bytes = total_elements * sizeof(float);

    float *raw_buf = AllocateAlignedFloats(total_elements);
    float *base_buf = AllocateAlignedFloats(total_elements);
    float *revert_buf = AllocateAlignedFloats(total_elements);

    if (raw_buf == nullptr || base_buf == nullptr || revert_buf == nullptr)
    {
        AlignedDeleter deleter;
        if (raw_buf) deleter(raw_buf);
        if (base_buf) deleter(base_buf);
        if (revert_buf) deleter(revert_buf);
        return false;
    }

    weights_buffer_.reset(raw_buf);
    base_weights_buffer_.reset(base_buf);
    revert_weights_buffer_.reset(revert_buf);

    fin.read(reinterpret_cast<char *>(raw_buf), total_bytes);
    if (static_cast<size_t>(fin.gcount()) != total_bytes)
    {
        weights_buffer_.reset();
        base_weights_buffer_.reset();
        revert_weights_buffer_.reset();
        return false;
    }

    std::memcpy(base_buf, raw_buf, total_bytes);
    std::memcpy(revert_buf, raw_buf, total_bytes);
    total_elements_ = total_elements;
    base_model_hash_ = ComputeFnv1a64(raw_buf, total_bytes);

    // Map weight pointers
    float *ptr = raw_buf;
    w_emb_ = ptr; ptr += size_emb;
    w_ih_  = ptr; ptr += size_ih;
    w_hh_  = ptr; ptr += size_hh;
    b_ih_  = ptr; ptr += size_bih;
    b_hh_  = ptr; ptr += size_bhh;
    w_out_ = ptr; ptr += size_out;
    b_out_ = ptr; ptr += size_bout;

    is_loaded_ = true;
    InitPrecomputedInputGates();
    InitFp16Weights();
    return true;
}

bool GruModel::LoadUserWeights(const std::string &path)
{
    if (!is_loaded_ || !weights_buffer_ || total_elements_ == 0)
    {
        return false;
    }

    std::ifstream fin(path, std::ios::binary);
    if (!fin.is_open())
    {
        return false;
    }

    ModelHeader header;
    fin.read(reinterpret_cast<char *>(&header), sizeof(ModelHeader));
    if (fin.gcount() != sizeof(ModelHeader))
    {
        return false;
    }

    if (std::memcmp(header.magic, kMagic, 4) != 0 ||
        header.version != kCurrentVersion ||
        header.data_type != kDataTypeFp32 ||
        header.vocab_size != vocab_size_ ||
        header.hidden_size != hidden_size_)
    {
        return false;
    }

    uint64_t saved_base_hash = 0;
    std::memcpy(&saved_base_hash, header.reserved, sizeof(uint64_t));
    if (saved_base_hash != 0 && saved_base_hash != base_model_hash_)
    {
        return false;
    }

    const size_t total_bytes = total_elements_ * sizeof(float);
    std::vector<float> tmp_buf(total_elements_);
    fin.read(reinterpret_cast<char *>(tmp_buf.data()), total_bytes);
    if (static_cast<size_t>(fin.gcount()) != total_bytes)
    {
        return false;
    }

    std::memcpy(weights_buffer_.get(), tmp_buf.data(), total_bytes);
    has_revert_snapshot_ = false;
    InitPrecomputedInputGates();
    InitFp16Weights();
    return true;
}

bool GruModel::SwapUserWeights(const std::string &path)
{
    if (!is_loaded_ || !base_weights_buffer_ || !weights_buffer_)
    {
        return false;
    }
    ResetToBaseWeights();
    if (path.empty() || !std::filesystem::exists(path))
    {
        return true;
    }
    return LoadUserWeights(path);
}

bool GruModel::SaveModel(const std::string &path) const
{
    if (!is_loaded_ || !weights_buffer_ || total_elements_ == 0 || path.empty())
    {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path target_path(path);
    if (target_path.has_parent_path())
    {
        std::filesystem::create_directories(target_path.parent_path(), ec);
    }

    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream fout(tmp_path, std::ios::binary | std::ios::trunc);
        if (!fout.is_open())
        {
            return false;
        }

        ModelHeader header{};
        std::memcpy(header.magic, kMagic, 4);
        header.version = kCurrentVersion;
        header.vocab_size = vocab_size_;
        header.hidden_size = hidden_size_;
        header.data_type = kDataTypeFp32;
        std::memcpy(header.reserved, &base_model_hash_, sizeof(uint64_t));

        fout.write(reinterpret_cast<const char *>(&header), sizeof(ModelHeader));
        fout.write(reinterpret_cast<const char *>(weights_buffer_.get()),
                   total_elements_ * sizeof(float));
        fout.flush();
        if (!fout.good())
        {
            fout.close();
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    }

    std::filesystem::rename(tmp_path, path, ec);
    if (ec)
    {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

bool GruModel::RevertLastTrainStep()
{
    if (!is_loaded_ || !has_revert_snapshot_ || !revert_weights_buffer_ || !weights_buffer_)
    {
        return false;
    }
    std::memcpy(weights_buffer_.get(), revert_weights_buffer_.get(),
                total_elements_ * sizeof(float));
    has_revert_snapshot_ = false;
    InitPrecomputedInputGates();
    InitFp16Weights();
    return true;
}

void GruModel::ResetToBaseWeights()
{
    if (!is_loaded_ || !base_weights_buffer_ || !weights_buffer_)
    {
        return;
    }
    std::memcpy(weights_buffer_.get(), base_weights_buffer_.get(),
                total_elements_ * sizeof(float));
    has_revert_snapshot_ = false;
    InitPrecomputedInputGates();
    InitFp16Weights();
}

void GruModel::InitPrecomputedInputGates()
{
    if (!is_loaded_ || w_ih_ == nullptr || w_emb_ == nullptr || b_ih_ == nullptr)
    {
        precomputed_gi_.reset();
        return;
    }

    const size_t H = hidden_size_;
    const size_t H3 = 3 * H;
    const size_t V = vocab_size_;
    const size_t total_gi = V * H3;

    float *buf = AllocateAlignedFloats(total_gi);
    if (buf == nullptr)
    {
        precomputed_gi_.reset();
        return;
    }
    precomputed_gi_.reset(buf);

    for (size_t v = 0; v < V; ++v)
    {
        const float *e_v = &w_emb_[v * H];
        float *gi_v = &precomputed_gi_[v * H3];
        math::Gemv(w_ih_, e_v, b_ih_, gi_v, H3, H);
    }
}

void GruModel::InitFp16Weights()
{
    use_fp16_ = false;
    w_out_fp16_.reset();

#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC) || (defined(__clang__) || defined(__GNUC__))
    const char *disable_fp16_env = std::getenv("MOGRU_DISABLE_FP16");
    if (disable_fp16_env != nullptr && std::strcmp(disable_fp16_env, "1") == 0)
    {
        return;
    }

    if (!is_loaded_ || w_out_ == nullptr || vocab_size_ == 0 || hidden_size_ == 0)
    {
        return;
    }

    if (math::HasArmFp16())
    {
        const size_t out_elements = static_cast<size_t>(vocab_size_) * hidden_size_;
        void *raw_buf = AllocateAlignedBytes(out_elements * sizeof(uint16_t));
        if (raw_buf != nullptr)
        {
            w_out_fp16_.reset(static_cast<uint16_t *>(raw_buf));
            math::ConvertFp32ToFp16(w_out_, reinterpret_cast<__fp16 *>(raw_buf), out_elements);
            use_fp16_ = true;
        }
    }
#endif
#endif
}

void GruModel::ForwardGruCell(uint16_t token_id,
                              const float *h_prev,
                              float *h_next) const
{
    if (token_id >= vocab_size_) [[unlikely]]
    {
        token_id = kUnkTokenId;
    }

    const size_t H = hidden_size_;
    const size_t H3 = 3 * H;

    constexpr size_t kMaxStackH3 = 768;
    alignas(64) float gi_stack[kMaxStackH3];
    alignas(64) float gh_stack[kMaxStackH3];

    std::vector<float> gi_heap;
    std::vector<float> gh_heap;
    float *gi = gi_stack;
    float *gh = gh_stack;

    if (H3 > kMaxStackH3) [[unlikely]]
    {
        gi_heap.resize(H3);
        gh_heap.resize(H3);
        gi = gi_heap.data();
        gh = gh_heap.data();
    }

    // 1 & 2: gi = W_ih * e_t + b_ih
    if (precomputed_gi_ != nullptr)
    {
        gi = const_cast<float *>(&precomputed_gi_[static_cast<size_t>(token_id) * H3]);
    }
    else
    {
        const float *e_t = &w_emb_[static_cast<size_t>(token_id) * H];
        math::Gemv(w_ih_, e_t, b_ih_, gi, H3, H);
    }

    // 3. gh = W_hh * h_prev + b_hh
    if (h_prev != nullptr)
    {
        math::Gemv(w_hh_, h_prev, b_hh_, gh, H3, H);
    }
    else
    {
        std::memcpy(gh, b_hh_, H3 * sizeof(float));
    }

    // 4. Update GRU gates and compute h_next
    alignas(64) float zero_h[256] = {0.0f};
    const float *prev_h_vec = h_prev;
    if (prev_h_vec == nullptr)
    {
        if (H <= 256)
        {
            prev_h_vec = zero_h;
        }
        else
        {
            gi_heap.assign(H, 0.0f);
            prev_h_vec = gi_heap.data();
        }
    }

    math::GruCellUpdate(gi, gh, prev_h_vec, h_next, H);
}

void GruModel::Step(uint16_t token_id,
                    const float *h_prev,
                    float *h_next,
                    float *log_probs_out) const
{
    if (!is_loaded_)
    {
        return;
    }

    ForwardGruCell(token_id, h_prev, h_next);

    // Output layer: log_probs = LogSoftmax(W_out * h_next + b_out)
    if (log_probs_out != nullptr)
    {
#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC) || (defined(__clang__) || defined(__GNUC__))
        if (use_fp16_ && w_out_fp16_)
        {
            constexpr size_t kMaxStackH = 256;
            alignas(16) __fp16 h_fp16_stack[kMaxStackH];
            __fp16 *h_fp16 = h_fp16_stack;
            std::vector<__fp16> h_fp16_heap;
            if (hidden_size_ > kMaxStackH) [[unlikely]]
            {
                h_fp16_heap.resize(hidden_size_);
                h_fp16 = h_fp16_heap.data();
            }
            math::ConvertFp32ToFp16(h_next, h_fp16, hidden_size_);
            math::GemvNeonFp16(reinterpret_cast<const __fp16 *>(w_out_fp16_.get()),
                               h_fp16, b_out_, log_probs_out, vocab_size_, hidden_size_);
        }
        else
#endif
#endif
        {
            math::Gemv(w_out_, h_next, b_out_, log_probs_out, vocab_size_, hidden_size_);
        }
        math::LogSoftmax(log_probs_out, log_probs_out, vocab_size_);
    }
}

float GruModel::StepWithTargetLogProb(uint16_t token_id,
                                      const float *h_prev,
                                      float *h_next,
                                      uint16_t target_token) const
{
    if (!is_loaded_)
    {
        return 0.0f;
    }

    ForwardGruCell(token_id, h_prev, h_next);

    if (target_token >= vocab_size_) [[unlikely]]
    {
        target_token = kUnkTokenId;
    }

    constexpr size_t kMaxStackVocab = 4096;
    alignas(64) float logits_stack[kMaxStackVocab];
    std::vector<float> logits_heap;
    float *logits = logits_stack;

    if (vocab_size_ > kMaxStackVocab) [[unlikely]]
    {
        logits_heap.resize(vocab_size_);
        logits = logits_heap.data();
    }

    // Compute all logits for LogSumExp denominator
#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC) || (defined(__clang__) || defined(__GNUC__))
    if (use_fp16_ && w_out_fp16_)
    {
        constexpr size_t kMaxStackH = 256;
        alignas(16) __fp16 h_fp16_stack[kMaxStackH];
        __fp16 *h_fp16 = h_fp16_stack;
        std::vector<__fp16> h_fp16_heap;
        if (hidden_size_ > kMaxStackH) [[unlikely]]
        {
            h_fp16_heap.resize(hidden_size_);
            h_fp16 = h_fp16_heap.data();
        }
        math::ConvertFp32ToFp16(h_next, h_fp16, hidden_size_);
        math::GemvNeonFp16(reinterpret_cast<const __fp16 *>(w_out_fp16_.get()),
                           h_fp16, b_out_, logits, vocab_size_, hidden_size_);
    }
    else
#endif
#endif
    {
        math::Gemv(w_out_, h_next, b_out_, logits, vocab_size_, hidden_size_);
    }

    const float log_denom = math::ComputeLogSumExp(logits, vocab_size_);
    return logits[target_token] - log_denom;
}

GruContext GruModel::ComputeContext(const charindex::CharIndex &indexer,
                                    std::string_view prefix) const
{
    GruContext ctx;
    if (!is_loaded_ || prefix.empty())
    {
        return ctx;
    }

    std::vector<uint16_t> token_ids;
    token_ids.reserve(prefix.size());

    std::string_view remaining = prefix;
    while (!remaining.empty())
    {
        size_t bytes_consumed = 0;
        char32_t cp = indexer.DecodeUtf8Char(remaining, bytes_consumed);
        if (bytes_consumed == 0)
        {
            break;
        }
        token_ids.push_back(indexer.Char2Index(cp));
        remaining.remove_prefix(bytes_consumed);
    }

    if (token_ids.empty())
    {
        return ctx;
    }

    const size_t H = hidden_size_;
    std::vector<float> h_buf1(H, 0.0f);
    std::vector<float> h_buf2(H, 0.0f);
    float *h_curr = h_buf1.data();
    float *h_next = h_buf2.data();

    uint16_t prev_token = kBosTokenId;
    for (size_t t = 0; t < token_ids.size(); ++t)
    {
        ForwardGruCell(prev_token, (t == 0) ? nullptr : h_curr, h_next);

        uint16_t target_token = token_ids[t];
        if (target_token >= vocab_size_) [[unlikely]]
        {
            target_token = kUnkTokenId;
        }
        prev_token = target_token;
        std::swap(h_curr, h_next);
    }

    ctx.h.assign(h_curr, h_curr + H);
    ctx.last_token = prev_token;
    ctx.is_empty = false;
    return ctx;
}

float GruModel::ScoreStringWithContext(const charindex::CharIndex &indexer,
                                       std::string_view utf8_str,
                                       const GruContext &context,
                                       bool normalize) const
{
    if (!is_loaded_ || utf8_str.empty())
    {
        return 0.0f;
    }

    if (context.is_empty || context.h.empty())
    {
        return ScoreString(indexer, utf8_str, nullptr, normalize);
    }

    std::vector<uint16_t> token_ids;
    token_ids.reserve(utf8_str.size());

    std::string_view remaining = utf8_str;
    while (!remaining.empty())
    {
        size_t bytes_consumed = 0;
        char32_t cp = indexer.DecodeUtf8Char(remaining, bytes_consumed);
        if (bytes_consumed == 0)
        {
            break;
        }
        token_ids.push_back(indexer.Char2Index(cp));
        remaining.remove_prefix(bytes_consumed);
    }

    if (token_ids.empty())
    {
        return 0.0f;
    }

    const size_t H = hidden_size_;
    std::vector<float> h_buf1(context.h);
    std::vector<float> h_buf2(H, 0.0f);
    float *h_curr = h_buf1.data();
    float *h_next = h_buf2.data();

    float total_log_prob = 0.0f;
    uint16_t prev_token = context.last_token;

    for (size_t t = 0; t < token_ids.size(); ++t)
    {
        uint16_t target_token = token_ids[t];
        if (target_token >= vocab_size_) [[unlikely]]
        {
            target_token = kUnkTokenId;
        }

        // Fast zero-store step: directly computes log P(target_token | history)
        float token_log_prob = StepWithTargetLogProb(prev_token, h_curr, h_next, target_token);

        if (target_token == kUnkTokenId)
        {
            token_log_prob = std::min(token_log_prob, -6.0f) - 6.0f;
        }

        total_log_prob += token_log_prob;
        prev_token = target_token;
        std::swap(h_curr, h_next);
    }

    if (normalize)
    {
        return total_log_prob / static_cast<float>(token_ids.size());
    }

    return total_log_prob;
}

float GruModel::ScoreString(const charindex::CharIndex &indexer,
                            std::string_view utf8_str,
                            const float *initial_h,
                            bool normalize) const
{
    if (!is_loaded_ || utf8_str.empty())
    {
        return 0.0f;
    }

    // Parse UTF-8 string into token IDs
    std::vector<uint16_t> token_ids;
    token_ids.reserve(utf8_str.size());

    std::string_view remaining = utf8_str;
    while (!remaining.empty())
    {
        size_t bytes_consumed = 0;
        char32_t cp = indexer.DecodeUtf8Char(remaining, bytes_consumed);
        if (bytes_consumed == 0)
        {
            break;
        }
        token_ids.push_back(indexer.Char2Index(cp));
        remaining.remove_prefix(bytes_consumed);
    }

    if (token_ids.empty())
    {
        return 0.0f;
    }

    const size_t H = hidden_size_;

    // Ping-pong buffers for hidden states
    std::vector<float> h_buf1(H, 0.0f);
    std::vector<float> h_buf2(H, 0.0f);
    float *h_curr = h_buf1.data();
    float *h_next = h_buf2.data();

    if (initial_h != nullptr)
    {
        std::memcpy(h_curr, initial_h, H * sizeof(float));
    }

    float total_log_prob = 0.0f;
    uint16_t prev_token = kBosTokenId;

    for (size_t t = 0; t < token_ids.size(); ++t)
    {
        uint16_t target_token = token_ids[t];
        if (target_token >= vocab_size_) [[unlikely]]
        {
            target_token = kUnkTokenId;
        }

        // Fast zero-store step: directly computes log P(target_token | history)
        float token_log_prob = StepWithTargetLogProb(
            prev_token,
            (t == 0 && initial_h == nullptr) ? nullptr : h_curr,
            h_next,
            target_token);

        if (target_token == kUnkTokenId)
        {
            token_log_prob = std::min(token_log_prob, -6.0f) - 6.0f;
        }

        total_log_prob += token_log_prob;
        prev_token = target_token;
        std::swap(h_curr, h_next);
    }

    if (normalize)
    {
        return total_log_prob / static_cast<float>(token_ids.size());
    }

    return total_log_prob;
}

float GruModel::TrainOnStringWithContext(const charindex::CharIndex &indexer,
                                         std::string_view utf8_str,
                                         const GruContext &context,
                                         const TrainConfig &config)
{
    if (!is_loaded_ || utf8_str.empty())
    {
        return 0.0f;
    }

    std::vector<uint16_t> token_ids;
    token_ids.reserve(utf8_str.size());

    std::string_view remaining = utf8_str;
    while (!remaining.empty())
    {
        size_t bytes_consumed = 0;
        char32_t cp = indexer.DecodeUtf8Char(remaining, bytes_consumed);
        if (bytes_consumed == 0)
        {
            break;
        }
        token_ids.push_back(indexer.Char2Index(cp));
        remaining.remove_prefix(bytes_consumed);
    }

    if (token_ids.empty())
    {
        return 0.0f;
    }

    constexpr size_t kMaxTrainTokens = 256;
    if (token_ids.size() > kMaxTrainTokens)
    {
        token_ids.resize(kMaxTrainTokens);
    }

    const size_t T = token_ids.size();
    const size_t H = hidden_size_;
    const size_t V = vocab_size_;
    const size_t H3 = 3 * H;

    size_t valid_targets = 0;
    for (size_t t = 0; t < T; ++t)
    {
        if (token_ids[t] >= V)
        {
            token_ids[t] = kUnkTokenId;
        }
        if (token_ids[t] != kUnkTokenId)
        {
            ++valid_targets;
        }
    }

    // If learning is disabled or all targets are <unk>, just return normalized score.
    if (valid_targets == 0 || config.learning_rate <= 0.0f)
    {
        return ScoreStringWithContext(indexer, utf8_str, context, true);
    }

    // Snapshot current weights for RevertLastTrainStep()
    if (revert_weights_buffer_ && weights_buffer_)
    {
        std::memcpy(revert_weights_buffer_.get(), weights_buffer_.get(),
                    total_elements_ * sizeof(float));
        has_revert_snapshot_ = true;
    }

    // Instant mode strictly enforces output-only updates to protect recurrence dynamics
    const bool full_bptt = (!config.instant_mode && config.mode == TrainMode::kFullBptt);
    const float inv_valid = 1.0f / static_cast<float>(valid_targets);

    const size_t size_emb = V * H;
    const size_t size_ih  = 3 * H * H;
    const size_t size_hh  = 3 * H * H;
    const size_t size_bih = 3 * H;
    const size_t size_bhh = 3 * H;
    const size_t size_out = V * H;
    const size_t size_bout = V;

    // Allocate gradient buffers
    std::vector<float> grad_all;
    std::vector<float> grad_out_only;
    float *g_emb = nullptr;
    float *g_ih  = nullptr;
    float *g_hh  = nullptr;
    float *g_bih = nullptr;
    float *g_bhh = nullptr;
    float *g_out = nullptr;
    float *g_bout = nullptr;

    if (full_bptt)
    {
        grad_all.assign(total_elements_, 0.0f);
        float *gptr = grad_all.data();
        g_emb  = gptr; gptr += size_emb;
        g_ih   = gptr; gptr += size_ih;
        g_hh   = gptr; gptr += size_hh;
        g_bih  = gptr; gptr += size_bih;
        g_bhh  = gptr; gptr += size_bhh;
        g_out  = gptr; gptr += size_out;
        g_bout = gptr;
    }
    else
    {
        grad_out_only.assign(size_out + size_bout, 0.0f);
        g_out  = grad_out_only.data();
        g_bout = grad_out_only.data() + size_out;
    }

    // Forward trajectory buffers for BPTT
    std::vector<uint16_t> x_tokens(T, kBosTokenId);
    std::vector<float> h_prev_hist;
    std::vector<float> r_hist;
    std::vector<float> z_hist;
    std::vector<float> n_hist;
    std::vector<float> ghn_hist;
    std::vector<float> dh_out_hist;

    if (full_bptt)
    {
        h_prev_hist.assign(T * H, 0.0f);
        r_hist.assign(T * H, 0.0f);
        z_hist.assign(T * H, 0.0f);
        n_hist.assign(T * H, 0.0f);
        ghn_hist.assign(T * H, 0.0f);
        dh_out_hist.assign(T * H, 0.0f);
    }

    std::vector<float> h_curr(H, 0.0f);
    std::vector<float> h_next(H, 0.0f);
    uint16_t prev_token = kBosTokenId;
    bool has_initial_h = false;

    if (!context.is_empty && context.h.size() == H)
    {
        std::memcpy(h_curr.data(), context.h.data(), H * sizeof(float));
        prev_token = (context.last_token < V) ? context.last_token : kUnkTokenId;
        has_initial_h = true;
    }

    std::vector<float> gi(H3, 0.0f);
    std::vector<float> gh(H3, 0.0f);
    std::vector<float> logits(V, 0.0f);
    std::vector<float> dl(V, 0.0f);

    float total_log_prob = 0.0f;

    for (size_t t = 0; t < T; ++t)
    {
        const uint16_t x_t = (prev_token < V) ? prev_token : kUnkTokenId;
        const uint16_t y_t = token_ids[t];
        x_tokens[t] = x_t;

        const float *e_t = &w_emb_[static_cast<size_t>(x_t) * H];
        math::Gemv(w_ih_, e_t, b_ih_, gi.data(), H3, H);

        if (t > 0 || has_initial_h)
        {
            math::Gemv(w_hh_, h_curr.data(), b_hh_, gh.data(), H3, H);
        }
        else
        {
            std::memcpy(gh.data(), b_hh_, H3 * sizeof(float));
        }

        const float *gi_r = gi.data();
        const float *gi_z = gi.data() + H;
        const float *gi_n = gi.data() + 2 * H;

        const float *gh_r = gh.data();
        const float *gh_z = gh.data() + H;
        const float *gh_n = gh.data() + 2 * H;

        for (size_t j = 0; j < H; ++j)
        {
            const float r = math::Sigmoid(gi_r[j] + gh_r[j]);
            const float z = math::Sigmoid(gi_z[j] + gh_z[j]);
            const float n = std::tanh(gi_n[j] + r * gh_n[j]);
            h_next[j] = (1.0f - z) * n + z * h_curr[j];

            if (full_bptt)
            {
                h_prev_hist[t * H + j] = h_curr[j];
                r_hist[t * H + j] = r;
                z_hist[t * H + j] = z;
                n_hist[t * H + j] = n;
                ghn_hist[t * H + j] = gh_n[j];
            }
        }

        // Output layer forward
        math::Gemv(w_out_, h_next.data(), b_out_, logits.data(), V, H);
        const float log_denom = math::ComputeLogSumExp(logits.data(), V);
        float token_log_prob = logits[y_t] - log_denom;

        if (y_t == kUnkTokenId)
        {
            token_log_prob = std::min(token_log_prob, -6.0f) - 6.0f;
        }
        else
        {
            // Compute dlogits = (softmax(logits) - one_hot(y_t)) / valid_targets
            for (size_t v = 0; v < V; ++v)
            {
                const float p_v = std::exp(logits[v] - log_denom);
                const float dl_v = (p_v - ((v == y_t) ? 1.0f : 0.0f)) * inv_valid;
                dl[v] = dl_v;
                g_bout[v] += dl_v;
                math::Axpy(dl_v, h_next.data(), &g_out[v * H], H);
            }

            if (full_bptt)
            {
                math::GemvTransposedAdd(w_out_, dl.data(), &dh_out_hist[t * H], V, H);
            }
        }

        total_log_prob += token_log_prob;
        prev_token = y_t;
        std::swap(h_curr, h_next);
    }

    if (full_bptt)
    {
        std::vector<float> dh_next(H, 0.0f);
        std::vector<float> dh(H, 0.0f);
        std::vector<float> dgi(H3, 0.0f);
        std::vector<float> dgh(H3, 0.0f);

        for (size_t step = T; step > 0; --step)
        {
            const size_t t = step - 1;
            const uint16_t x_t = x_tokens[t];
            const float *e_t = &w_emb_[static_cast<size_t>(x_t) * H];
            const float *h_prev_t = &h_prev_hist[t * H];
            const float *r_t = &r_hist[t * H];
            const float *z_t = &z_hist[t * H];
            const float *n_t = &n_hist[t * H];
            const float *ghn_t = &ghn_hist[t * H];
            const float *dh_out_t = &dh_out_hist[t * H];

            float *dgi_r = dgi.data();
            float *dgi_z = dgi.data() + H;
            float *dgi_n = dgi.data() + 2 * H;

            float *dgh_r = dgh.data();
            float *dgh_z = dgh.data() + H;
            float *dgh_n = dgh.data() + 2 * H;

            for (size_t j = 0; j < H; ++j)
            {
                const float dh_j = dh_out_t[j] + dh_next[j];
                dh[j] = dh_j;

                const float r = r_t[j];
                const float z = z_t[j];
                const float n = n_t[j];

                const float dn = dh_j * (1.0f - z);
                const float dz = dh_j * (h_prev_t[j] - n);

                const float dgn = dn * (1.0f - n * n);
                const float dgz = dz * z * (1.0f - z);
                const float dr = dgn * ghn_t[j];
                const float dgr = dr * r * (1.0f - r);

                dgi_r[j] = dgr;
                dgi_z[j] = dgz;
                dgi_n[j] = dgn;

                dgh_r[j] = dgr;
                dgh_z[j] = dgz;
                dgh_n[j] = dgn * r;

                dh_next[j] = dh_j * z;
            }

            // Accumulate bias gradients
            math::Axpy(1.0f, dgi.data(), g_bih, H3);
            math::Axpy(1.0f, dgh.data(), g_bhh, H3);

            // Accumulate W_ih and W_hh gradients
            for (size_t k = 0; k < H3; ++k)
            {
                math::Axpy(dgi[k], e_t, &g_ih[k * H], H);
                math::Axpy(dgh[k], h_prev_t, &g_hh[k * H], H);
            }

            // Accumulate W_emb gradient for x_t
            math::GemvTransposedAdd(w_ih_, dgi.data(), &g_emb[static_cast<size_t>(x_t) * H], H3, H);

            // Propagate gradient to h_{t-1}
            math::GemvTransposedAdd(w_hh_, dgh.data(), dh_next.data(), H3, H);
        }

        // Global gradient norm clipping and L2 anchor SGD update over all weights
        const float grad_norm_sq = math::DotProduct(grad_all.data(), grad_all.data(), total_elements_);
        const float grad_norm = std::sqrt(grad_norm_sq);
        float grad_scale = 1.0f;
        if (config.max_grad_norm > 0.0f && grad_norm > config.max_grad_norm)
        {
            grad_scale = config.max_grad_norm / grad_norm;
        }

        const float eff_lr = config.learning_rate * grad_scale;
        const float anchor_decay = std::min(0.1f, config.learning_rate * config.weight_decay);
        float *w_ptr = weights_buffer_.get();
        const float *base_ptr = base_weights_buffer_.get();

        for (size_t i = 0; i < total_elements_; ++i)
        {
            float delta = eff_lr * grad_all[i] + anchor_decay * (w_ptr[i] - base_ptr[i]);
            if (config.max_weight_delta > 0.0f)
            {
                delta = std::clamp(delta, -config.max_weight_delta, config.max_weight_delta);
            }
            w_ptr[i] -= delta;
        }
    }
    else
    {
        // Output-only gradient norm clipping and L2 anchor SGD update
        const size_t out_elements = size_out + size_bout;
        const float grad_norm_sq = math::DotProduct(grad_out_only.data(), grad_out_only.data(), out_elements);
        const float grad_norm = std::sqrt(grad_norm_sq);
        float grad_scale = 1.0f;
        if (config.max_grad_norm > 0.0f && grad_norm > config.max_grad_norm)
        {
            grad_scale = config.max_grad_norm / grad_norm;
        }

        const float eff_lr = config.learning_rate * grad_scale;
        const float anchor_decay = std::min(0.1f, config.learning_rate * config.weight_decay);
        const size_t out_offset = size_emb + size_ih + size_hh + size_bih + size_bhh;
        float *w_out_ptr = weights_buffer_.get() + out_offset;
        const float *base_out_ptr = base_weights_buffer_.get() + out_offset;

        for (size_t i = 0; i < out_elements; ++i)
        {
            float delta = eff_lr * grad_out_only[i] + anchor_decay * (w_out_ptr[i] - base_out_ptr[i]);
            if (config.max_weight_delta > 0.0f)
            {
                delta = std::clamp(delta, -config.max_weight_delta, config.max_weight_delta);
            }
            w_out_ptr[i] -= delta;
        }
    }

    if (full_bptt)
    {
        InitPrecomputedInputGates();
    }
    InitFp16Weights();

    return total_log_prob / static_cast<float>(T);
}

float GruModel::TrainOnString(const charindex::CharIndex &indexer,
                              std::string_view utf8_str,
                              const TrainConfig &config)
{
    GruContext empty_ctx;
    return TrainOnStringWithContext(indexer, utf8_str, empty_ctx, config);
}

} // namespace gruscorer
