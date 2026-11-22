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

#include "engine/engine.h"

#include <memory>
#include <string>
#include <utility>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "composer/composer.h"
#include "composer/table.h"
#include "config/gru_config.h"
#include "data_manager/data_manager.h"
#include "data_manager/testing/mock_data_manager.h"
#include "engine/engine_converter_interface.h"
#include "engine/engine_factory.h"
#include "engine/modules.h"
#include "engine/supplemental_model_interface.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "protocol/engine_builder.pb.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

ABSL_DECLARE_FLAG(bool, enable_live_conversion);

namespace mozc {
namespace engine {

namespace {

class SupplementalModelForTesting : public engine::SupplementalModelInterface {
 public:
  bool LoadAsync(const EngineReloadRequest& request) override {
    ++load_async_count_;
    last_request_ = request;
    return true;
  }

  void ClearOldModels() override { ++clear_old_models_count_; }

  int load_async_count() const { return load_async_count_; }
  int clear_old_models_count() const { return clear_old_models_count_; }
  const EngineReloadRequest& last_request() const { return last_request_; }

 private:
  int load_async_count_ = 0;
  int clear_old_models_count_ = 0;
  EngineReloadRequest last_request_;
};

constexpr absl::string_view kMockMagicNumber = "MOCK";
constexpr absl::string_view kOssMagicNumber = "\xEFMOZC\x0D\x0A";
constexpr int kMiddlePriority = 50;
}  // namespace

class EngineTest : public ::testing::Test {
 protected:
  EngineTest() {
    const std::string mock_path =
        testing::GetSourcePath({"data_manager", "testing", "mock_mozc.data"});
    mock_request_.set_file_path(mock_path);
    mock_request_.set_magic_number(kMockMagicNumber);
    mock_request_.set_priority(kMiddlePriority);

    const std::string oss_path =
        testing::GetSourcePath({"data_manager", "oss", "mozc.data"});
    oss_request_.set_file_path(oss_path);
    oss_request_.set_magic_number(kOssMagicNumber);
    oss_request_.set_priority(kMiddlePriority);

    const std::string invalid_path =
        testing::GetSourcePath({"data_manager", "invalid", "mozc.data"});
    invalid_path_request_.set_file_path(invalid_path);
    invalid_path_request_.set_magic_number(kOssMagicNumber);
    invalid_path_request_.set_priority(kMiddlePriority);

    invalid_data_request_.set_file_path(mock_path);
    invalid_data_request_.set_magic_number(kOssMagicNumber);
    invalid_data_request_.set_priority(kMiddlePriority);

    mock_version_ = DataManager::CreateFromFile(mock_request_.file_path(),
                                                mock_request_.magic_number())
                        .value()
                        ->GetDataVersion();
    oss_version_ = DataManager::CreateFromFile(oss_request_.file_path(),
                                               oss_request_.magic_number())
                       .value()
                       ->GetDataVersion();
  }

  void SetUp() override {
    engine_ = Engine::CreateEngine();
    engine_->SetAlwaysWaitForTesting(true);
  }

  std::unique_ptr<Engine> engine_;

  std::string mock_version_;
  std::string oss_version_;

  EngineReloadRequest mock_request_;
  EngineReloadRequest oss_request_;
  EngineReloadRequest invalid_path_request_;
  EngineReloadRequest invalid_data_request_;
};

TEST_F(EngineTest, ReloadModulesTest) {
  std::unique_ptr<Modules> modules =
      engine::Modules::Create(std::make_unique<testing::MockDataManager>())
          .value();

  CHECK_OK(engine_->ReloadModules(std::move(modules)));
}

// Tests the interaction with DataLoader for successful Engine
// reload event.
TEST_F(EngineTest, DataLoadSuccessfulScenarioTest) {
  EngineReloadResponse response;

  // The engine is not updated yet.
  EXPECT_NE(engine_->GetDataVersion(), mock_version_);

  // The engine is updated with the request.
  EXPECT_TRUE(engine_->SendEngineReloadRequest(mock_request_));
  EXPECT_TRUE(engine_->MaybeReloadEngine(&response));
  EXPECT_EQ(engine_->GetDataVersion(), mock_version_);

  // The engine is not updated with the same request.
  EXPECT_FALSE(engine_->SendEngineReloadRequest(mock_request_));
  EXPECT_FALSE(engine_->MaybeReloadEngine(&response));
  EXPECT_EQ(engine_->GetDataVersion(), mock_version_);
}

// Tests situations to handle multiple new requests.
TEST_F(EngineTest, DataUpdateSuccessfulScenarioTest) {
  EngineReloadResponse response;

  // Send a request, and update the engine.
  EXPECT_TRUE(engine_->SendEngineReloadRequest(mock_request_));
  EXPECT_TRUE(engine_->MaybeReloadEngine(&response));
  EXPECT_EQ(engine_->GetDataVersion(), mock_version_);

  // Send another request, and update the engine again.
  EXPECT_TRUE(engine_->SendEngineReloadRequest(oss_request_));
  EXPECT_TRUE(engine_->MaybeReloadEngine(&response));
  EXPECT_EQ(engine_->GetDataVersion(), oss_version_);
}

// Tests the interaction with DataLoader in the situation where
// requested data is broken.
TEST_F(EngineTest, ReloadInvalidDataTest) {
  EXPECT_TRUE(engine_->SendEngineReloadRequest(invalid_path_request_));

  // The new request is performed, but it returns invalid data.
  EngineReloadResponse response;
  EXPECT_FALSE(engine_->MaybeReloadEngine(&response));

  // Sends the same request again, but the request is already marked as
  // unregistered.
  EXPECT_FALSE(engine_->SendEngineReloadRequest(invalid_path_request_));
  EXPECT_FALSE(engine_->MaybeReloadEngine(&response));
}

// Tests the rollback scenario
TEST_F(EngineTest, RollbackDataTest) {
  // Sends multiple requests three times.
  EXPECT_TRUE(engine_->SendEngineReloadRequest(mock_request_));
  EXPECT_TRUE(engine_->SendEngineReloadRequest(invalid_path_request_));
  EXPECT_TRUE(engine_->SendEngineReloadRequest(invalid_data_request_));

  // The last two requests are invalid. The first request is immediately used as
  // a fallback.
  EngineReloadResponse response;
  EXPECT_TRUE(engine_->MaybeReloadEngine(&response));
  EXPECT_EQ(response.request().file_path(), mock_request_.file_path());

  // DataVersion comes from the first request (i.e. mock_request_).
  EXPECT_EQ(engine_->GetDataVersion(), mock_version_);
}

TEST_F(EngineTest, SupplementalModelReloadTest) {
  // Requests sent while converter_ is nullptr are deferred, keeping the latest.
  EXPECT_TRUE(engine_->SendSupplementalModelReloadRequest(mock_request_));
  EXPECT_TRUE(engine_->SendSupplementalModelReloadRequest(oss_request_));
  engine_->ClearOldSupplementalModels();

  auto supplemental_model = std::make_unique<SupplementalModelForTesting>();
  const SupplementalModelForTesting* supplemental_model_ptr =
      supplemental_model.get();
  std::unique_ptr<Modules> modules =
      engine::ModulesPresetBuilder()
          .PresetSupplementalModel(std::move(supplemental_model))
          .Build(std::make_unique<testing::MockDataManager>())
          .value();
  CHECK_OK(engine_->ReloadModules(std::move(modules)));

  // The pending latest request is loaded upon converter_ initialization.
  EXPECT_EQ(supplemental_model_ptr->load_async_count(), 1);
  EXPECT_EQ(supplemental_model_ptr->last_request().file_path(),
            oss_request_.file_path());

  // Subsequent requests after converter_ is initialized are loaded immediately.
  EXPECT_TRUE(engine_->SendSupplementalModelReloadRequest(mock_request_));
  EXPECT_EQ(supplemental_model_ptr->load_async_count(), 2);
  EXPECT_EQ(supplemental_model_ptr->last_request().file_path(),
            mock_request_.file_path());
  engine_->ClearOldSupplementalModels();
  EXPECT_EQ(supplemental_model_ptr->clear_old_models_count(), 1);
}

TEST_F(EngineTest, BugWoEndToEndOssConversionAndLiveConversion) {
  const std::string config_path = config::GruConfigManager::ResolveConfigFilePath(
      "", /*allow_in_test=*/true);
  GruPenaltyConfig cfg;
  cfg.enable_english_fallback = true;
  cfg.english_fallback_cost_threshold = 13000.0;
  cfg.english_fallback_min_word_length = 3;
  cfg.enable_live_conversion_english_fallback = true;
  cfg.user_english_words = {"git"};
  config::GruConfigManager::SaveConfig(cfg, config_path);
  config::GruConfigManager::InvalidateConfigCache();

  const bool orig_live = absl::GetFlag(FLAGS_enable_live_conversion);

  std::unique_ptr<Engine> oss_engine = EngineFactory::Create().value();
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

  auto extract_output = [](const commands::Output& output) {
    std::string text;
    if (output.has_preedit()) {
      for (const auto& seg : output.preedit().segment()) {
        text.append(seg.value());
      }
    }
    return text;
  };

  struct Case {
    const char* raw;
    const char* expected;
  };
  const Case cases[] = {
      {"bugwo", "bugを"},
      {"watashinobugwo", "私のbugを"},
      {"tagwo", "tagを"},
      {"logwo", "logを"},
      {"dogwo", "dogを"},
      {"bagwo", "bagを"},
      {"debugwo", "debugを"},
      {"gitwo", "gitを"},
      {"testwo", "testを"},
      {"classwo", "classを"},
      {"passwo", "passを"},
      {"wordwo", "wordを"},
      {"buguwo", "武具を"},
      {"fonto", "フォント"},
      {"hwonto", "フォント"},
      {"kyouwo", "今日を"},
      {"syuwo", "種を"},
      {"tsukatta", "使った"},
  };

  for (const Case& tc : cases) {
    absl::SetFlag(&FLAGS_enable_live_conversion, false);
    std::unique_ptr<EngineConverterInterface> ec =
        oss_engine->CreateEngineConverter();
    composer::Composer composer = type_raw(tc.raw);
    ASSERT_TRUE(ec->Convert(composer)) << tc.raw;
    ec->SyncComposerAfterConversion(&composer);
    commands::Output output;
    ec->FillOutput(composer, &output);
    EXPECT_EQ(extract_output(output), tc.expected) << "Convert: " << tc.raw;
  }

  for (const Case& tc : cases) {
    absl::SetFlag(&FLAGS_enable_live_conversion, true);
    std::unique_ptr<EngineConverterInterface> ec =
        oss_engine->CreateEngineConverter();
    composer::Composer composer = type_raw(tc.raw);
    ASSERT_TRUE(ec->Suggest(composer)) << tc.raw;
    commands::Output output;
    ec->FillOutput(composer, &output);
    EXPECT_EQ(extract_output(output), tc.expected) << "Suggest: " << tc.raw;
  }

  absl::SetFlag(&FLAGS_enable_live_conversion, orig_live);
  GruPenaltyConfig clean_cfg;
  clean_cfg.enable_english_fallback = true;
  config::GruConfigManager::SaveConfig(clean_cfg, config_path);
  config::GruConfigManager::InvalidateConfigCache();
}

TEST_F(EngineTest, TrailingWEndToEndOssConversionAndLiveConversion) {
  const std::string config_path = config::GruConfigManager::ResolveConfigFilePath(
      "", /*allow_in_test=*/true);
  const bool orig_live = absl::GetFlag(FLAGS_enable_live_conversion);

  std::unique_ptr<Engine> oss_engine = EngineFactory::Create().value();
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

  auto extract_output = [](const commands::Output& output) {
    std::string text;
    if (output.has_preedit()) {
      for (const auto& seg : output.preedit().segment()) {
        text.append(seg.value());
      }
    }
    return text;
  };

  for (const double threshold : {11000.0, 13000.0, 15000.0}) {
    GruPenaltyConfig cfg;
    cfg.enable_gru_rewriter = true;
    cfg.enable_resegmentation = true;
    cfg.enable_english_fallback = true;
    cfg.english_fallback_cost_threshold = threshold;
    cfg.english_fallback_min_word_length = 3;
    cfg.enable_live_conversion_english_fallback = true;
    config::GruConfigManager::SaveConfig(cfg, config_path);
    config::GruConfigManager::InvalidateConfigCache();

    struct Case {
      const char* raw;
      const char* expected;
    };

    const Case conv_cases[] = {
        {"follow", "follow"},
        {"followwo", "followを"},
        {"followsuru", "followする"},
        {"followshite", "followして"},
        {"followsareta", "followされた"},
        {"followsurutowindowwo", "followするとwindowを"},
        {"watashinofollow", "私のfollow"},
        {"watashinofollowwo", "私のfollowを"},
        {"watashinofollowsuru", "私のfollowする"},
        {"followni", "followに"},
        {"followga", "followが"},
        {"followha", "followは"},
        {"followno", "followの"},
        {"showsuru", "showする"},
        {"worksuru", "workする"},
        {"mergesuru", "mergeする"},
        {"viewsuru", "viewする"},
        {"reviewsuru", "reviewする"},
        {"window", "window"},
        {"windowwo", "windowを"},
        {"watashinowindow", "私のwindow"},
        {"allow", "allow"},
        {"allowwo", "allowを"},
        {"arrow", "arrow"},
        {"arrowwo", "arrowを"},
        {"show", "show"},
        {"showwo", "showを"},
        {"low", "low"},
        {"below", "below"},
        {"yellow", "yellow"},
        {"hollow", "hollow"},
        {"narrow", "narrow"},
        {"review", "review"},
        {"view", "view"},
        {"new", "new"},
        {"now", "now"},
        {"how", "how"},
        {"few", "few"},
        {"know", "know"},
        {"flow", "flow"},
        {"slow", "slow"},
        {"follows", "follows"},
        // Japanese + slang 'w' must not be misconverted to English words.
        {"w", "w"},
        {"watashiw", "私w"},
        {"watashinow", "私のw"},
        {"kusaw", "草w"},
        {"majiw", "マジw"},
        {"hontow", "ほんとw"},
        {"korewow", "これをw"},
        {"sorewow", "それをw"},
        {"korenew", "これねw"},
        {"sugoinew", "すごいねw"},
        {"koresaw", "これさw"},
        {"aresaw", "あれさw"},
        {"nanikoresaw", "なにこれさw"},
        {"soudeshow", "そうでしょw"},
        {"majideshow", "マジでしょw"},
        {"koremirow", "これ見ろw"},
        {"nandarow", "何だろw"},
    };

    for (const Case& tc : conv_cases) {
      absl::SetFlag(&FLAGS_enable_live_conversion, false);
      std::unique_ptr<EngineConverterInterface> ec =
          oss_engine->CreateEngineConverter();
      composer::Composer composer = type_raw(tc.raw);
      ASSERT_TRUE(ec->Convert(composer)) << tc.raw;
      ec->SyncComposerAfterConversion(&composer);
      commands::Output output;
      ec->FillOutput(composer, &output);
      EXPECT_EQ(extract_output(output), tc.expected)
          << "Convert(thresh=" << threshold << "): " << tc.raw;
    }

    const Case live_cases[] = {
        {"followwo", "followを"},
        {"watashinofollowwo", "私のfollowを"},
        {"followsuru", "followする"},
        {"watashinofollowsuru", "私のfollowする"},
        {"windowwo", "windowを"},
        {"allowwo", "allowを"},
        {"arrowwo", "arrowを"},
        {"showwo", "showを"},
        {"follows", "follows"},
    };

    for (const Case& tc : live_cases) {
      absl::SetFlag(&FLAGS_enable_live_conversion, true);
      std::unique_ptr<EngineConverterInterface> ec =
          oss_engine->CreateEngineConverter();
      composer::Composer composer = type_raw(tc.raw);
      ASSERT_TRUE(ec->Suggest(composer)) << tc.raw;
      commands::Output output;
      ec->FillOutput(composer, &output);
      EXPECT_EQ(extract_output(output), tc.expected)
          << "Suggest(thresh=" << threshold << "): " << tc.raw;
    }
  }

  absl::SetFlag(&FLAGS_enable_live_conversion, orig_live);
  GruPenaltyConfig clean_cfg;
  clean_cfg.enable_english_fallback = true;
  config::GruConfigManager::SaveConfig(clean_cfg, config_path);
  config::GruConfigManager::InvalidateConfigCache();
}

TEST_F(EngineTest, ProfileWoAndTrailingParticleEndToEnd) {
  const std::string config_path = config::GruConfigManager::ResolveConfigFilePath(
      "", /*allow_in_test=*/true);
  const bool orig_live = absl::GetFlag(FLAGS_enable_live_conversion);

  std::unique_ptr<Engine> oss_engine = EngineFactory::Create().value();
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

  auto extract_output = [](const commands::Output& output) {
    std::string text;
    if (output.has_preedit()) {
      for (const auto& seg : output.preedit().segment()) {
        text.append(seg.value());
      }
    }
    return text;
  };

  for (const double threshold : {11000.0, 13000.0, 15000.0}) {
    GruPenaltyConfig cfg;
    cfg.enable_gru_rewriter = true;
    cfg.enable_resegmentation = true;
    cfg.resegment_threshold = 300.0;
    cfg.gru_score_weight = 1000.0;
    cfg.katakana_multiplier = 2.0;
    cfg.hiragana_multiplier = 1.2;
    cfg.enable_english_fallback = true;
    cfg.english_fallback_cost_threshold = threshold;
    cfg.english_fallback_min_word_length = 3;
    cfg.enable_live_conversion_english_fallback = true;
    config::GruConfigManager::SaveConfig(cfg, config_path);
    config::GruConfigManager::InvalidateConfigCache();

    struct Case {
      const char* raw;
      const char* expected;
    };

    const Case conv_cases[] = {
        {"profile", "profile"},
        {"profilewo", "profileを"},
        {"profilega", "profileが"},
        {"profileni", "profileに"},
        {"profileha", "profileは"},
        {"profilede", "profileで"},
        {"profileto", "profileと"},
        {"profileno", "profileの"},
        {"profilemo", "profileも"},
        {"profilekara", "profileから"},
        {"profilemade", "profileまで"},
        {"profiledesu", "profileです"},
        {"watashinoprofilewo", "私のprofileを"},
        {"Profilewo", "Profileを"},
        {"applewo", "appleを"},
        {"tablewo", "tableを"},
        {"stylewo", "styleを"},
        {"updatewo", "updateを"},
        {"mergewo", "mergeを"},
        // Explicit Shift ASCII prefix before healthy Japanese must stay intact.
        {"Bannnara", "B案なら"},
        {"Bannna", "Bあんな"},
        {"Bannni", "B案に"},
        {"Pannwo", "P案を"},
    };

    for (const Case& tc : conv_cases) {
      absl::SetFlag(&FLAGS_enable_live_conversion, false);
      std::unique_ptr<EngineConverterInterface> ec =
          oss_engine->CreateEngineConverter();
      composer::Composer composer = type_raw(tc.raw);
      ASSERT_TRUE(ec->Convert(composer)) << tc.raw;
      ec->SyncComposerAfterConversion(&composer);
      commands::Output output;
      ec->FillOutput(composer, &output);
      EXPECT_EQ(extract_output(output), tc.expected)
          << "Convert(thresh=" << threshold << "): " << tc.raw;
    }

    const Case live_cases[] = {
        {"profilewo", "profileを"},
        {"profilega", "profileが"},
        {"profileni", "profileに"},
        {"watashinoprofilewo", "私のprofileを"},
        {"Profilewo", "Profileを"},
        {"applewo", "appleを"},
        {"tablewo", "tableを"},
        {"stylewo", "styleを"},
        {"updatewo", "updateを"},
        {"mergewo", "mergeを"},
        {"Bannnara", "B案なら"},
        {"Pannwo", "P案を"},
    };

    for (const Case& tc : live_cases) {
      absl::SetFlag(&FLAGS_enable_live_conversion, true);
      std::unique_ptr<EngineConverterInterface> ec =
          oss_engine->CreateEngineConverter();
      composer::Composer composer(table, client_request, default_config);
      for (char ch : absl::string_view(tc.raw)) {
        commands::KeyEvent key;
        key.set_key_code(ch);
        composer.InsertCharacterKeyEvent(key);
        ec->Suggest(composer);
      }
      commands::Output output;
      ec->FillOutput(composer, &output);
      EXPECT_EQ(extract_output(output), tc.expected)
          << "LiveStep(thresh=" << threshold << "): " << tc.raw;
    }
  }

  absl::SetFlag(&FLAGS_enable_live_conversion, orig_live);
  GruPenaltyConfig clean;
  clean.enable_english_fallback = true;
  config::GruConfigManager::SaveConfig(clean, config_path);
  config::GruConfigManager::InvalidateConfigCache();
}
}  // namespace engine
}  // namespace mozc
