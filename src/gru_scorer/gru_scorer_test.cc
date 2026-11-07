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

#include <cassert>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

#define ENSURE(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            std::cerr << "Assertion failed: " #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while (0)

void AssertNear(float actual, float expected, float tolerance = 1e-4f, const std::string &msg = "")
{
    float diff = std::fabs(actual - expected);
    if (diff > tolerance)
    {
        std::cerr << "Assertion failed: " << msg
                  << " actual=" << actual << " expected=" << expected
                  << " diff=" << diff << " (tolerance=" << tolerance << ")" << std::endl;
        std::abort();
    }
}

void TestMathOps()
{
    std::cout << "[Test] Running TestMathOps..." << std::endl;

    // 1. DotProduct
    const float a[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    const float b[] = {2.0f, 0.5f, -1.0f, 1.5f, 2.0f};
    // 1*2 + 2*0.5 + 3*(-1) + 4*1.5 + 5*2 = 2 + 1 - 3 + 6 + 10 = 16
    float dot = gruscorer::math::DotProduct(a, b, 5);
    AssertNear(dot, 16.0f, 1e-5f, "DotProduct");

    // 2. Gemv: y = W * x + bias
    // W = [[1, 2], [3, 4], [5, 6]] (3x2)
    // x = [0.5, -1.0]
    // b = [10, 20, 30]
    // y0 = 1*0.5 + 2*(-1.0) + 10 = -1.5 + 10 = 8.5
    // y1 = 3*0.5 + 4*(-1.0) + 20 = -2.5 + 20 = 17.5
    // y2 = 5*0.5 + 6*(-1.0) + 30 = -3.5 + 30 = 26.5
    const float W[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const float x[] = {0.5f, -1.0f};
    const float bias[] = {10.0f, 20.0f, 30.0f};
    float y[3] = {0.0f};
    gruscorer::math::Gemv(W, x, bias, y, 3, 2);
    AssertNear(y[0], 8.5f, 1e-5f, "Gemv y[0]");
    AssertNear(y[1], 17.5f, 1e-5f, "Gemv y[1]");
    AssertNear(y[2], 26.5f, 1e-5f, "Gemv y[2]");

    // 3. Sigmoid
    AssertNear(gruscorer::math::Sigmoid(0.0f), 0.5f, 1e-5f, "Sigmoid(0)");

    // 4. LogSoftmax
    const float logits[] = {1.0f, 2.0f, 3.0f};
    float log_probs[3] = {0.0f};
    gruscorer::math::LogSoftmax(logits, log_probs, 3);
    // exp(1)+exp(2)+exp(3) = 2.71828 + 7.389056 + 20.085537 = 30.19287
    // log(30.19287) = 3.4076
    // log_probs = [1 - 3.4076 = -2.4076, 2 - 3.4076 = -1.4076, 3 - 3.4076 = -0.4076]
    AssertNear(log_probs[0], -2.4076f, 1e-3f, "LogSoftmax[0]");
    AssertNear(log_probs[1], -1.4076f, 1e-3f, "LogSoftmax[1]");
    AssertNear(log_probs[2], -0.4076f, 1e-3f, "LogSoftmax[2]");

    // Sum of exp(log_probs) must be 1.0
    float sum_p = std::exp(log_probs[0]) + std::exp(log_probs[1]) + std::exp(log_probs[2]);
    AssertNear(sum_p, 1.0f, 1e-5f, "LogSoftmax sum exp");

    // 5. Numerical consistency check: Scalar vs AVX2+FMA
#if defined(__x86_64__) || defined(_M_X64)
    if (gruscorer::math::HasAvx2Fma())
    {
        constexpr size_t kRows = 257;
        constexpr size_t kCols = 135;
        std::vector<float> big_W(kRows * kCols);
        std::vector<float> big_x(kCols);
        std::vector<float> big_b(kRows);
        std::vector<float> y_scalar(kRows);
        std::vector<float> y_avx2(kRows);

        for (size_t i = 0; i < big_W.size(); ++i)
        {
            big_W[i] = std::sin(static_cast<float>(i + 1) * 0.01f);
        }
        for (size_t i = 0; i < big_x.size(); ++i)
        {
            big_x[i] = std::cos(static_cast<float>(i + 1) * 0.02f);
        }
        for (size_t i = 0; i < big_b.size(); ++i)
        {
            big_b[i] = 0.1f * static_cast<float>(i % 10);
        }

        gruscorer::math::GemvScalar(big_W.data(), big_x.data(), big_b.data(), y_scalar.data(), kRows, kCols);
        gruscorer::math::GemvAvx2Fma(big_W.data(), big_x.data(), big_b.data(), y_avx2.data(), kRows, kCols);

        for (size_t r = 0; r < kRows; ++r)
        {
            AssertNear(y_avx2[r], y_scalar[r], 1e-4f, "Gemv AVX2 vs Scalar row " + std::to_string(r));
        }

        float lse_scalar = gruscorer::math::ComputeLogSumExpScalar(y_scalar.data(), kRows);
        float lse_avx2 = gruscorer::math::ComputeLogSumExpAvx2(y_scalar.data(), kRows);
        AssertNear(lse_avx2, lse_scalar, 1e-4f, "LogSumExp AVX2 vs Scalar");
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    {
        constexpr size_t kRows = 257;
        constexpr size_t kCols = 135;
        std::vector<float> big_W(kRows * kCols);
        std::vector<float> big_x(kCols);
        std::vector<float> big_b(kRows);
        std::vector<float> y_scalar(kRows);
        std::vector<float> y_neon(kRows);

        for (size_t i = 0; i < big_W.size(); ++i)
        {
            big_W[i] = std::sin(static_cast<float>(i + 1) * 0.01f);
        }
        for (size_t i = 0; i < big_x.size(); ++i)
        {
            big_x[i] = std::cos(static_cast<float>(i + 1) * 0.02f);
        }
        for (size_t i = 0; i < big_b.size(); ++i)
        {
            big_b[i] = 0.1f * static_cast<float>(i % 10);
        }

        gruscorer::math::GemvScalar(big_W.data(), big_x.data(), big_b.data(), y_scalar.data(), kRows, kCols);
        gruscorer::math::GemvNeon(big_W.data(), big_x.data(), big_b.data(), y_neon.data(), kRows, kCols);

        for (size_t r = 0; r < kRows; ++r)
        {
            AssertNear(y_neon[r], y_scalar[r], 1e-4f, "Gemv NEON vs Scalar row " + std::to_string(r));
        }

        float lse_scalar = gruscorer::math::ComputeLogSumExpScalar(y_scalar.data(), kRows);
        float lse_neon = gruscorer::math::ComputeLogSumExpNeon(y_scalar.data(), kRows);
        AssertNear(lse_neon, lse_scalar, 1e-3f, "LogSumExp NEON vs Scalar");
    }
#endif

    std::cout << "[Test] TestMathOps passed." << std::endl;
}

#include <filesystem>

std::string FindDataFile(const std::string &filename)
{
    const std::vector<std::string> candidates = {
        filename,
        "gru_scorer/" + filename,
        "_main/gru_scorer/" + filename,
        "../gru_scorer/" + filename,
    };
    for (const auto &p : candidates)
    {
        if (std::filesystem::exists(p)) return p;
    }
    const char *test_srcdir = std::getenv("TEST_SRCDIR");
    if (test_srcdir)
    {
        std::string p = std::string(test_srcdir) + "/_main/gru_scorer/" + filename;
        if (std::filesystem::exists(p)) return p;
        p = std::string(test_srcdir) + "/gru_scorer/" + filename;
        if (std::filesystem::exists(p)) return p;
    }
    return filename;
}

void TestGruModelInference()
{
    std::cout << "[Test] Running TestGruModelInference..." << std::endl;

    gruscorer::GruModel model;
    std::string path = FindDataFile("dummy_model.bin");
    const bool loaded = model.LoadModel(path);
    (void)loaded;
    ENSURE(loaded && "Failed to load dummy_model.bin");
    ENSURE(model.is_loaded());
    ENSURE(model.vocab_size() == 4096);
    ENSURE(model.has_precomputed_input_gates() && "Precomputed input gates must be initialized on model load");
    ENSURE(model.hidden_size() == 64);

    const size_t H = model.hidden_size();
    const size_t V = model.vocab_size();

    std::vector<float> h1(H, 0.0f);
    std::vector<float> log_probs1(V, 0.0f);

    // Step 1: token 1 (<bos>), h_prev = nullptr
    model.Step(1, nullptr, h1.data(), log_probs1.data());

    // Verify against Python NumPy reference values
    AssertNear(h1[0], -0.001625417f, 1e-5f, "Step1 h1[0]");
    AssertNear(h1[1], -0.010878378f, 1e-5f, "Step1 h1[1]");
    AssertNear(h1[2], -0.014900248f, 1e-5f, "Step1 h1[2]");
    AssertNear(h1[3], -0.008568134f, 1e-5f, "Step1 h1[3]");
    AssertNear(h1[4],  0.017188603f, 1e-5f, "Step1 h1[4]");

    AssertNear(log_probs1[0], -8.313534f, 1e-4f, "Step1 log_probs[0]");
    AssertNear(log_probs1[1], -8.329125f, 1e-4f, "Step1 log_probs[1]");
    AssertNear(log_probs1[2], -8.335196f, 1e-4f, "Step1 log_probs[2]");
    AssertNear(log_probs1[100], -8.157008f, 1e-4f, "Step1 log_probs[100]");

    // Verify StepWithTargetLogProb matches Step log_probs output exactly
    std::vector<float> h1_fast(H, 0.0f);
    for (uint16_t test_tok : {0, 1, 2, 50, 100, 500, 4000})
    {
        float target_lp = model.StepWithTargetLogProb(1, nullptr, h1_fast.data(), test_tok);
        AssertNear(target_lp, log_probs1[test_tok], 1e-4f, "StepWithTargetLogProb token " + std::to_string(test_tok));
        for (size_t j = 0; j < H; ++j)
        {
            AssertNear(h1_fast[j], h1[j], 1e-5f, "StepWithTargetLogProb h state");
        }
    }

    // Step 2: token 100, h_prev = h1
    std::vector<float> h2(H, 0.0f);
    std::vector<float> log_probs2(V, 0.0f);
    model.Step(100, h1.data(), h2.data(), log_probs2.data());

    AssertNear(h2[0], -0.014443655f, 1e-5f, "Step2 h2[0]");
    AssertNear(h2[1], -0.024245083f, 1e-5f, "Step2 h2[1]");
    AssertNear(h2[2],  0.000322970f, 1e-5f, "Step2 h2[2]");
    AssertNear(h2[3], -0.007451564f, 1e-5f, "Step2 h2[3]");
    AssertNear(h2[4],  0.045681491f, 1e-5f, "Step2 h2[4]");

    AssertNear(log_probs2[200], -8.254036f, 1e-4f, "Step2 log_probs[200]");

    std::cout << "[Test] TestGruModelInference passed! Precision strictly matches NumPy." << std::endl;
}

void TestCharIndexIntegration()
{
    std::cout << "[Test] Running TestCharIndexIntegration..." << std::endl;

    charindex::CharIndex indexer;
    std::string chardata_path = FindDataFile("chardata.bin");
    const bool loaded_char = indexer.LoadFile(chardata_path);
    (void)loaded_char;
    ENSURE(loaded_char && "Failed to load chardata.bin");

    gruscorer::GruModel model;
    std::string model_path = FindDataFile("dummy_model.bin");
    const bool loaded_model = model.LoadModel(model_path);
    (void)loaded_model;
    ENSURE(loaded_model && "Failed to load dummy_model.bin");

    std::string text = "今日は良い天気";
    float score = model.ScoreString(indexer, text);

    std::cout << "  - Text: \"" << text << "\", Log-likelihood Score: " << score << std::endl;
    ENSURE(!std::isnan(score) && !std::isinf(score));
    ENSURE(score < 0.0f);

    ENSURE(model.ScoreString(indexer, "") == 0.0f);

    // Test Japanese punctuation normalization
    ENSURE(indexer.Char2Index(0xFF0C) == indexer.Char2Index(0x3001));  // '，' == '、'
    ENSURE(indexer.Char2Index(0xFF0E) == indexer.Char2Index(0x3002));  // '．' == '。'
    ENSURE(indexer.Char2Index(0xFF64) == indexer.Char2Index(0x3001));  // '､' == '、'
    ENSURE(indexer.Char2Index(0xFF61) == indexer.Char2Index(0x3002));  // '｡' == '。'
    ENSURE(indexer.Char2Index(0x002C) == indexer.Char2Index(0x3001));  // ',' == '、'
    ENSURE(indexer.Char2Index(0x002E) == indexer.Char2Index(0x3002));  // '.' == '。'

    float score_touten = model.ScoreString(indexer, "今日は、良い天気ですね。");
    float score_comma = model.ScoreString(indexer, "今日は，良い天気ですね．");
    float score_ascii = model.ScoreString(indexer, "今日は,良い天気ですね.");
    AssertNear(score_comma, score_touten, 1e-5f, "Score with '，．' must match '、。'");
    AssertNear(score_ascii, score_touten, 1e-5f, "Score with ',.' must match '、。'");

    std::cout << "[Test] TestCharIndexIntegration passed!" << std::endl;
}

void TestGruContextCaching()
{
    std::cout << "[Test] Running TestGruContextCaching..." << std::endl;

    charindex::CharIndex indexer;
    std::string chardata_path = FindDataFile("chardata.bin");
    ENSURE(indexer.LoadFile(chardata_path));

    gruscorer::GruModel model;
    std::string model_path = FindDataFile("dummy_model.bin");
    ENSURE(model.LoadModel(model_path));

    const std::string full_text = "今日は良い天気";
    float score_full = model.ScoreString(indexer, full_text, nullptr, false);

    // 1. Empty context test
    gruscorer::GruContext empty_ctx;
    float score_empty_ctx = model.ScoreStringWithContext(indexer, full_text, empty_ctx, false);
    AssertNear(score_empty_ctx, score_full, 1e-5f, "Empty context scoring == full ScoreString");

    // 2. Empty string with context
    auto ctx_partial = model.ComputeContext(indexer, "今日は");
    AssertNear(model.ScoreStringWithContext(indexer, "", ctx_partial, false), 0.0f, 1e-5f, "Empty string with context");

    // 3. Mathematical consistency of chain rule:
    // log P(AB) = log P(A) + log P(B | A)
    const std::string prefix = "今日は";
    const std::string suffix = "良い天気";
    float score_prefix = model.ScoreString(indexer, prefix, nullptr, false);
    auto ctx = model.ComputeContext(indexer, prefix);
    ENSURE(!ctx.is_empty);
    ENSURE(ctx.h.size() == model.hidden_size());

    float score_suffix = model.ScoreStringWithContext(indexer, suffix, ctx, false);
    AssertNear(score_full, score_prefix + score_suffix, 1e-4f, "Chain rule log P(AB) == log P(A) + log P(B|A)");

    // 4. Punctuation normalization context consistency
    auto ctx_touten = model.ComputeContext(indexer, "今日は、");
    auto ctx_comma = model.ComputeContext(indexer, "今日は，");
    auto ctx_ascii = model.ComputeContext(indexer, "今日は,");
    ENSURE(ctx_comma.last_token == ctx_touten.last_token);
    ENSURE(ctx_ascii.last_token == ctx_touten.last_token);
    for (size_t i = 0; i < model.hidden_size(); ++i) {
        AssertNear(ctx_comma.h[i], ctx_touten.h[i], 1e-5f, "Context h with '，' must match '、'");
        AssertNear(ctx_ascii.h[i], ctx_touten.h[i], 1e-5f, "Context h with ',' must match '、'");
    }

    std::cout << "[Test] TestGruContextCaching passed! Chain rule strictly holds." << std::endl;
}

void TestOnlineTrainingAndPersistence()
{
    std::cout << "[Test] Running TestOnlineTrainingAndPersistence..." << std::endl;

    charindex::CharIndex indexer;
    std::string chardata_path = FindDataFile("chardata.bin");
    ENSURE(indexer.LoadFile(chardata_path));

    gruscorer::GruModel model;
    std::string model_path = FindDataFile("dummy_model.bin");
    ENSURE(model.LoadModel(model_path));

    const std::string prefix = "今日は";
    const std::string target = "良い天気ですね";

    // 1. Test Output-Only Online Learning
    auto ctx0 = model.ComputeContext(indexer, prefix);
    const float base_score = model.ScoreStringWithContext(indexer, target, ctx0, true);

    gruscorer::TrainConfig out_cfg;
    out_cfg.mode = gruscorer::TrainMode::kOutputOnly;
    out_cfg.learning_rate = 0.05f;
    out_cfg.weight_decay = 0.001f;
    out_cfg.max_grad_norm = 5.0f;

    float pre_step1 = model.TrainOnStringWithContext(indexer, target, ctx0, out_cfg);
    AssertNear(pre_step1, base_score, 1e-4f, "Pre-update score returned by TrainOnStringWithContext");

    const float after_step1 = model.ScoreStringWithContext(indexer, target, ctx0, true);
    ENSURE(after_step1 > base_score && "Output-only training must improve target log-probability");

    // Verify that prefix hidden context is unchanged in OutputOnly mode
    auto ctx_after_out = model.ComputeContext(indexer, prefix);
    for (size_t i = 0; i < model.hidden_size(); ++i)
    {
        AssertNear(ctx_after_out.h[i], ctx0.h[i], 1e-6f, "OutputOnly must not alter hidden state weights");
    }

    // 2. Test RevertLastTrainStep
    ENSURE(model.has_revert_snapshot());
    ENSURE(model.RevertLastTrainStep());
    ENSURE(!model.has_revert_snapshot());
    const float reverted_score = model.ScoreStringWithContext(indexer, target, ctx0, true);
    AssertNear(reverted_score, base_score, 1e-5f, "RevertLastTrainStep must restore exact pre-step score");

    // 3. Test Full BPTT Online Learning
    gruscorer::TrainConfig full_cfg;
    full_cfg.mode = gruscorer::TrainMode::kFullBptt;
    full_cfg.learning_rate = 0.05f;
    full_cfg.weight_decay = 0.001f;
    full_cfg.max_grad_norm = 5.0f;

    float prev_score = base_score;
    for (int step = 0; step < 3; ++step)
    {
        auto step_ctx = model.ComputeContext(indexer, prefix);
        model.TrainOnStringWithContext(indexer, target, step_ctx, full_cfg);
        auto new_ctx = model.ComputeContext(indexer, prefix);
        float new_score = model.ScoreStringWithContext(indexer, target, new_ctx, true);
        ENSURE(new_score > prev_score && "Full BPTT training must monotonically improve score");
        prev_score = new_score;
    }

    // Verify that Full BPTT also updates hidden state representations
    auto ctx_after_full = model.ComputeContext(indexer, prefix);
    float h_diff = 0.0f;
    for (size_t i = 0; i < model.hidden_size(); ++i)
    {
        h_diff += std::fabs(ctx_after_full.h[i] - ctx0.h[i]);
    }
    ENSURE(h_diff > 1e-6f && "Full BPTT must update GRU / embedding weights");

    // 4. Test SaveModel & LoadUserWeights
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    std::string user_model_path = (tmpdir ? std::string(tmpdir) : "/tmp") + "/test_grumodel_user.bin";
    ENSURE(model.SaveModel(user_model_path));

    gruscorer::GruModel reloaded_model;
    ENSURE(reloaded_model.LoadModel(model_path));
    ENSURE(reloaded_model.LoadUserWeights(user_model_path));

    auto reloaded_ctx = reloaded_model.ComputeContext(indexer, prefix);
    const float reloaded_score = reloaded_model.ScoreStringWithContext(indexer, target, reloaded_ctx, true);
    AssertNear(reloaded_score, prev_score, 1e-5f, "Reloaded user weights must match trained model score");

    // 5. Test ResetToBaseWeights (preserves base anchor even after LoadUserWeights)
    reloaded_model.ResetToBaseWeights();
    auto reset_ctx = reloaded_model.ComputeContext(indexer, prefix);
    const float reset_score = reloaded_model.ScoreStringWithContext(indexer, target, reset_ctx, true);
    AssertNear(reset_score, base_score, 1e-5f, "ResetToBaseWeights must restore original base model score");

    // 6. Test Base Model Fingerprint Mismatch Protection:
    // If the user switches to a different base model (even with the same vocab_size & hidden_size),
    // LoadUserWeights must reject the old user_model_path so the new base model is not overwritten.
    std::string different_base_path = (tmpdir ? std::string(tmpdir) : "/tmp") + "/test_different_base.bin";
    {
        std::filesystem::copy_file(model_path, different_base_path,
                                   std::filesystem::copy_options::overwrite_existing);
        std::fstream fs(different_base_path, std::ios::in | std::ios::out | std::ios::binary);
        fs.seekp(sizeof(gruscorer::ModelHeader));
        float tweaked_val = 0.12345f;
        fs.write(reinterpret_cast<const char *>(&tweaked_val), sizeof(float));
    }
    gruscorer::GruModel new_base_model;
    ENSURE(new_base_model.LoadModel(different_base_path));
    ENSURE(!new_base_model.LoadUserWeights(user_model_path) &&
           "LoadUserWeights must reject user weights derived from a different base model");

    std::error_code ec;
    std::filesystem::remove(user_model_path, ec);
    std::filesystem::remove(different_base_path, ec);

    std::cout << "[Test] TestOnlineTrainingAndPersistence passed!" << std::endl;
}

void TestInstantModeAndSwapUserWeights()
{
    std::cout << "[Test] Running TestInstantModeAndSwapUserWeights..." << std::endl;

    gruscorer::GruModel model;
    std::string model_path = FindDataFile("dummy_model.bin");
    std::string chardata_path = FindDataFile("chardata.bin");
    ENSURE(model.LoadModel(model_path));

    charindex::CharIndex indexer;
    ENSURE(indexer.LoadFile(chardata_path));

    const std::string prefix = "私は";
    const std::string target1 = "東京";
    const std::string target2 = "福岡";

    auto ctx0 = model.ComputeContext(indexer, prefix);
    const float base_score1 = model.ScoreStringWithContext(indexer, target1, ctx0, true);
    const float base_score2 = model.ScoreStringWithContext(indexer, target2, ctx0, true);

    // 1. Verify Instant Mode safeguards with extreme learning rate (LR = 1.0f)
    gruscorer::TrainConfig instant_cfg;
    instant_cfg.instant_mode = true;
    instant_cfg.learning_rate = 1.0f; // Extreme learning rate
    instant_cfg.max_weight_delta = 2.0f;
    instant_cfg.max_grad_norm = 2.0f;
    // Deliberately request kFullBptt: instant_mode must force kOutputOnly to protect recurrence!
    instant_cfg.mode = gruscorer::TrainMode::kFullBptt;

    // Train 1 step on target1
    model.TrainOnStringWithContext(indexer, target1, ctx0, instant_cfg);

    // Verify hidden state of prefix is completely untouched
    auto ctx_after_instant = model.ComputeContext(indexer, prefix);
    for (size_t i = 0; i < model.hidden_size(); ++i)
    {
        AssertNear(ctx_after_instant.h[i], ctx0.h[i], 1e-6f,
                   "Instant mode must force OutputOnly, leaving recurrence state untouched");
    }

    // Verify score improved dramatically in 1 step
    const float instant_score1 = model.ScoreStringWithContext(indexer, target1, ctx0, true);
    ENSURE(instant_score1 > base_score1 + 0.1f && "Instant mode (LR=1.0) must achieve massive 1-step score boost");
    ENSURE(!std::isnan(instant_score1) && !std::isinf(instant_score1));

    // Save profile 1
    const char *tmpdir = std::getenv("TEST_TMPDIR");
    std::string profile1_path = (tmpdir ? std::string(tmpdir) : "/tmp") + "/test_grumodel_user_profile1.bin";
    std::string profile2_path = (tmpdir ? std::string(tmpdir) : "/tmp") + "/test_grumodel_user_profile2.bin";
    ENSURE(model.SaveModel(profile1_path));

    // 2. Train profile 2 from base weights on target2
    model.ResetToBaseWeights();
    AssertNear(model.ScoreStringWithContext(indexer, target1, ctx0, true), base_score1, 1e-5f,
               "ResetToBaseWeights must restore base score");

    model.TrainOnStringWithContext(indexer, target2, ctx0, instant_cfg);
    const float instant_score2 = model.ScoreStringWithContext(indexer, target2, ctx0, true);
    ENSURE(instant_score2 > base_score2 + 0.1f && "Profile 2 must boost target2");
    ENSURE(model.SaveModel(profile2_path));

    // 3. Test SwapUserWeights between profiles
    // Swap to profile 1
    ENSURE(model.SwapUserWeights(profile1_path));
    AssertNear(model.ScoreStringWithContext(indexer, target1, ctx0, true), instant_score1, 1e-4f,
               "SwapUserWeights to profile1 must restore target1 score");
    AssertNear(model.ScoreStringWithContext(indexer, target2, ctx0, true), base_score2, 1e-2f,
               "Profile 1 must not contain target2 adaptations");

    // Swap to profile 2
    ENSURE(model.SwapUserWeights(profile2_path));
    AssertNear(model.ScoreStringWithContext(indexer, target2, ctx0, true), instant_score2, 1e-4f,
               "SwapUserWeights to profile2 must restore target2 score");
    AssertNear(model.ScoreStringWithContext(indexer, target1, ctx0, true), base_score1, 1e-2f,
               "Profile 2 must not contain target1 adaptations");

    // Swap to empty (base model)
    ENSURE(model.SwapUserWeights(""));
    AssertNear(model.ScoreStringWithContext(indexer, target1, ctx0, true), base_score1, 1e-4f,
               "SwapUserWeights(\"\") must restore base model");
    AssertNear(model.ScoreStringWithContext(indexer, target2, ctx0, true), base_score2, 1e-4f,
               "SwapUserWeights(\"\") must restore base model");

    std::error_code ec;
    std::filesystem::remove(profile1_path, ec);
    std::filesystem::remove(profile2_path, ec);

    std::cout << "[Test] TestInstantModeAndSwapUserWeights passed!" << std::endl;
}

void BenchmarkInference()
{
    std::cout << "[Bench] Running BenchmarkInference..." << std::endl;

    gruscorer::GruModel model;
    std::string model_path = FindDataFile("dummy_model.bin");
    model.LoadModel(model_path);

#if defined(__x86_64__) || defined(_M_X64)
    std::cout << "  - Architecture: x86_64, AVX2+FMA: " << (gruscorer::math::HasAvx2Fma() ? "YES" : "NO") << std::endl;
#elif defined(__aarch64__) || defined(_M_ARM64)
    std::cout << "  - Architecture: ARM64 (NEON active)" << std::endl;
    std::cout << "  - ARMv8.2-A FP16 vector arithmetic: " << (gruscorer::math::HasArmFp16() ? "YES" : "NO") << std::endl;
    std::cout << "  - Model FP16 active: " << (model.is_fp16_active() ? "YES" : "NO") << std::endl;
#endif
    std::cout << "  - Precomputed input gates: " << (model.has_precomputed_input_gates() ? "YES" : "NO") << std::endl;

    const size_t H = model.hidden_size();
    const size_t V = model.vocab_size();

    std::vector<float> h_prev(H, 0.0f);
    std::vector<float> h_next(H, 0.0f);
    std::vector<float> log_probs(V, 0.0f);

    // Warm up
    for (int i = 0; i < 100; ++i) {
        model.Step(1, h_prev.data(), h_next.data(), log_probs.data());
    }

    constexpr int kIterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < kIterations; ++i)
    {
        uint16_t token = static_cast<uint16_t>((i % 100) + 1);
        model.Step(token, h_prev.data(), h_next.data(), log_probs.data());
        std::swap(h_prev, h_next);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end - start).count();
    double us_per_step = (total_ms * 1000.0) / kIterations;

    std::cout << "  - Executed " << kIterations << " steps in " << total_ms << " ms" << std::endl;
    std::cout << "  - Latency per character (Step + LogSoftmax): " << us_per_step << " microseconds" << std::endl;
    std::cout << "  - Throughput: " << static_cast<int>(1000000.0 / us_per_step) << " chars/sec" << std::endl;
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "Starting GRU Scorer Unit Tests & Benchmarks" << std::endl;
    std::cout << "========================================" << std::endl;

    TestMathOps();
    TestGruModelInference();
    TestCharIndexIntegration();
    TestGruContextCaching();
    TestOnlineTrainingAndPersistence();
    TestInstantModeAndSwapUserWeights();
    BenchmarkInference();

    std::cout << "========================================" << std::endl;
    std::cout << "All Tests & Benchmarks Passed Successfully!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
