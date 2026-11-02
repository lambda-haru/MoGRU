/*
 * Copyright (C) 2017~2017 by CSSlayer
 * wengxt@gmail.com
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; see the file COPYING. If not,
 * see <http://www.gnu.org/licenses/>.
 */

#include "unix/fcitx5/mozc_engine.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/semver.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx/action.h>
#include <fcitx/addoninstance.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>

#include <cstddef>
#include <memory>
#include <string>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "base/init_mozc.h"
#include "base/process.h"
#include "config/gru_config.h"
#include "protocol/commands.pb.h"
#include "unix/fcitx5/i18nwrapper.h"
#include "unix/fcitx5/mozc_client_interface.h"
#include "unix/fcitx5/mozc_client_pool.h"
#include "unix/fcitx5/mozc_response_parser.h"
#include "unix/fcitx5/mozc_state.h"

ABSL_DECLARE_FLAG(bool, enable_gru_rewriter);
ABSL_DECLARE_FLAG(std::string, gru_model_path);
ABSL_DECLARE_FLAG(std::string, gru_chardata_path);
ABSL_DECLARE_FLAG(std::string, gru_user_model_path);
ABSL_DECLARE_FLAG(double, gru_score_weight);
ABSL_DECLARE_FLAG(bool, show_gru_score);
ABSL_DECLARE_FLAG(bool, enable_gru_resegmentation);
ABSL_DECLARE_FLAG(int32_t, gru_resegment_cost_threshold);
ABSL_DECLARE_FLAG(bool, enable_live_conversion);
ABSL_DECLARE_FLAG(bool, enable_gru_online_learning);

namespace fcitx {

const struct CompositionModeInfo {
  const char* name;
  const char* icon;
  const char* label;
  const char* description;
  mozc::commands::CompositionMode mode;
} kPropCompositionModes[] = {
    {
        "mogru-mode-direct",
        "fcitx_mogru_direct",
        "A",
        N_("Direct"),
        mozc::commands::DIRECT,
    },
    {
        "mogru-mode-hiragana",
        "fcitx_mogru_hiragana",
        "\xe3\x81\x82",  // Hiragana letter A in UTF-8.
        N_("Hiragana"),
        mozc::commands::HIRAGANA,
    },
    {
        "mogru-mode-katakana_full",
        "fcitx_mogru_katakana_full",
        "\xe3\x82\xa2",  // Katakana letter A.
        N_("Full Katakana"),
        mozc::commands::FULL_KATAKANA,
    },
    {

        "mogru-mode-alpha_half",
        "fcitx_mogru_alpha_half",
        "A",
        N_("Half ASCII"),
        mozc::commands::HALF_ASCII,
    },
    {

        "mogru-mode-alpha_full",
        "fcitx_mogru_alpha_full",
        "\xef\xbc\xa1",  // Full width ASCII letter A.
        N_("Full ASCII"),
        mozc::commands::FULL_ASCII,
    },
    {
        "mogru-mode-katakana_half",
        "fcitx_mogru_katakana_half",
        "\xef\xbd\xb1",  // Half width Katakana letter A.
        N_("Half Katakana"),
        mozc::commands::HALF_KATAKANA,
    },
};
const size_t kNumCompositionModes = FCITX_ARRAY_SIZE(kPropCompositionModes);

MozcModeSubAction::MozcModeSubAction(MozcEngine* engine,
                                     mozc::commands::CompositionMode mode)
    : engine_(engine), mode_(mode) {
  setShortText(_(kPropCompositionModes[mode].description));
  setLongText(_(kPropCompositionModes[mode].description));
  setIcon(kPropCompositionModes[mode].icon);
  setCheckable(true);
}

bool MozcModeSubAction::isChecked(InputContext* ic) const {
  auto* mozc_state = engine_->mozcState(ic);
  return mozc_state->GetCompositionMode() == mode_;
}

void MozcModeSubAction::activate(InputContext* ic) {
  auto* mozc_state = engine_->mozcState(ic);
  mozc_state->SendCompositionMode(mode_);
}

// This array must correspond with the CompositionMode enum in the
// mozc/session/command.proto file.
static_assert(mozc::commands::NUM_OF_COMPOSITIONS == kNumCompositionModes,
              "number of modes must match");

Instance* Init(Instance* instance) {
  int argc = 1;
  char argv0[] = "fcitx_mogru";
  char* _argv[] = {argv0};
  char** argv = _argv;
  mozc::InitMozc(argv[0], &argc, &argv);
  return instance;
}

MozcEngine::MozcEngine(Instance* instance, std::string addonName)
    : instance_(Init(instance)),
      parser_(std::make_unique<MozcResponseParser>(this)),
      client_(createClient()),
      factory_([this](InputContext& ic) { return new MozcState(&ic, this); }),
      addonName_(std::move(addonName)) {
  pool_ = std::make_unique<MozcClientPool>(GetSharedStatePolicy());
  for (auto command :
       {mozc::commands::DIRECT, mozc::commands::HIRAGANA,
        mozc::commands::FULL_KATAKANA, mozc::commands::FULL_ASCII,
        mozc::commands::HALF_ASCII, mozc::commands::HALF_KATAKANA}) {
    modeActions_.push_back(std::make_unique<MozcModeSubAction>(this, command));
  }

  instance_->inputContextManager().registerProperty("mogruState", &factory_);
  instance_->userInterfaceManager().registerAction("mogru-tool",
                                                   &toolAction_);
  toolAction_.setShortText(_("MoGRU Settings"));
  toolAction_.setLongText(_("MoGRU Settings"));
  toolAction_.setIcon("fcitx_mogru_tool");

  int i = 0;
  for (auto& modeAction : modeActions_) {
    instance_->userInterfaceManager().registerAction(
        kPropCompositionModes[i].name, modeAction.get());
    toolMenu_.addAction(modeAction.get());
    i++;
  }

  separatorAction_.setSeparator(true);
  instance_->userInterfaceManager().registerAction("mogru-separator",
                                                   &separatorAction_);

  SemanticVersion version;
  version.setMajor(5);
  version.setMinor(0);
  version.setPatch(22);
  // Where we fix the support for separator
  if (auto fcitxVersion = SemanticVersion::parse(Instance::version());
      fcitxVersion && *fcitxVersion >= version) {
    toolMenu_.addAction(&separatorAction_);
  }

  instance_->userInterfaceManager().registerAction("mogru-tool-config",
                                                   &configToolAction_);
  configToolAction_.setShortText(_("Configuration Tool"));
  configToolAction_.setIcon("fcitx_mogru_properties");
  configToolAction_.connect<SimpleAction::Activated>([](InputContext*) {
    mozc::Process::SpawnMozcProcess("mozc_tool", "--mode=config_dialog");
  });

  instance_->userInterfaceManager().registerAction("mogru-tool-dict",
                                                   &dictionaryToolAction_);
  dictionaryToolAction_.setShortText(_("Dictionary Tool"));
  dictionaryToolAction_.setIcon("fcitx_mogru_dictionary");
  dictionaryToolAction_.connect<SimpleAction::Activated>([](InputContext*) {
    mozc::Process::SpawnMozcProcess("mozc_tool", "--mode=dictionary_tool");
  });

  instance_->userInterfaceManager().registerAction("mogru-tool-add",
                                                   &addWordAction_);
  addWordAction_.setShortText(_("Add Word"));
  addWordAction_.connect<SimpleAction::Activated>([](InputContext*) {
    mozc::Process::SpawnMozcProcess("mozc_tool", "--mode=word_register_dialog");
  });

  instance_->userInterfaceManager().registerAction("mogru-tool-about",
                                                   &aboutAction_);
  aboutAction_.setShortText(_("About MoGRU"));
  aboutAction_.connect<SimpleAction::Activated>([](InputContext*) {
    mozc::Process::SpawnMozcProcess("mozc_tool", "--mode=about_dialog");
  });

  toolMenu_.addAction(&configToolAction_);
  toolMenu_.addAction(&dictionaryToolAction_);
  toolMenu_.addAction(&addWordAction_);
  toolMenu_.addAction(&aboutAction_);

  toolAction_.setMenu(&toolMenu_);

  globalConfigReloadHandle_ =
      instance_->watchEvent(EventType::GlobalConfigReloaded,
                            EventWatcherPhase::Default, [this](Event&) {
                              reloadConfig();
                              return true;
                            });

  reloadConfig();
}

MozcEngine::~MozcEngine() {}
 
std::string MozcEngine::GetConfigPath() const {
  std::string name;
  if (addonInfo()) {
    name = addonInfo()->uniqueName();
  } else if (!addonName_.empty()) {
    name = addonName_;
  } else {
    name = "mogru";
  }

  const std::string preferred_path = "conf/" + name + ".conf";
  const char* home = std::getenv("HOME");
  if (home != nullptr) {
    const std::string full_path =
        mozc::FileUtil::JoinPath(home, ".config/fcitx5/" + preferred_path);
    if (mozc::FileUtil::FileExists(full_path).ok()) {
      return preferred_path;
    }
    // Fallback: check if old mozc-gru.conf exists when running as mogru
    if (name == "mogru") {
      const std::string old_path =
          mozc::FileUtil::JoinPath(home, ".config/fcitx5/conf/mozc-gru.conf");
      if (mozc::FileUtil::FileExists(old_path).ok()) {
        return "conf/mozc-gru.conf";
      }
    }
  }
  return preferred_path;
}

const Configuration* MozcEngine::getConfig() const {
  return &config_;
}

void MozcEngine::SyncGruFlags() {
  const mozc::GruPenaltyConfig cfg =
      mozc::config::GruConfigManager::LoadConfig();
  absl::SetFlag(&FLAGS_enable_gru_rewriter, cfg.enable_gru_rewriter);
  absl::SetFlag(&FLAGS_gru_model_path, cfg.gru_model_path);
  absl::SetFlag(&FLAGS_gru_chardata_path, cfg.gru_chardata_path);
  absl::SetFlag(&FLAGS_gru_user_model_path, cfg.gru_user_model_path);
  absl::SetFlag(&FLAGS_gru_score_weight, cfg.gru_score_weight);
  absl::SetFlag(&FLAGS_show_gru_score, cfg.show_gru_score);
  absl::SetFlag(&FLAGS_enable_gru_resegmentation, cfg.enable_resegmentation);
  absl::SetFlag(&FLAGS_gru_resegment_cost_threshold,
                static_cast<int32_t>(cfg.resegment_threshold));
  absl::SetFlag(&FLAGS_enable_live_conversion, cfg.enable_live_conversion);
  absl::SetFlag(&FLAGS_enable_gru_online_learning, cfg.enable_online_learning);
}

void MozcEngine::setConfig(const RawConfig& config) {
  config_.load(config, true);
  safeSaveAsIni(config_, GetConfigPath());
  SyncGruFlags();
  ResetClientPool();
}

void MozcEngine::reloadConfig() {
  readAsIni(config_, GetConfigPath());
  safeSaveAsIni(config_, GetConfigPath());
  SyncGruFlags();
  ResetClientPool();
}
void MozcEngine::activate(const fcitx::InputMethodEntry& /*entry*/,
                          fcitx::InputContextEvent& event) {
  if (client_) {
    client_->EnsureConnection();
  }
  auto* ic = event.inputContext();
  auto* mozc_state = mozcState(ic);
  mozc_state->FocusIn();
  ic->statusArea().addAction(StatusGroup::InputMethod, &toolAction_);
}
void MozcEngine::deactivate(const fcitx::InputMethodEntry& /*entry*/,
                            fcitx::InputContextEvent& event) {
  auto* ic = event.inputContext();
  deactivating_ = true;
  auto* mozc_state = mozcState(ic);
  mozc_state->FocusOut(event);
  deactivating_ = false;
}
void MozcEngine::keyEvent(const InputMethodEntry& entry, KeyEvent& event) {
  auto* mozc_state = mozcState(event.inputContext());

  const auto& group = instance_->inputMethodManager().currentGroup();
  std::string layout = group.layoutFor(entry.uniqueName());
  if (layout.empty()) {
    layout = group.defaultLayout();
  }

  const bool isJP = (layout == "jp" || layout.starts_with("jp-"));

  if (mozc_state->ProcessKeyEvent(event.rawKey().sym(), event.rawKey().code(),
                                  event.rawKey().states(), isJP,
                                  event.isRelease())) {
    event.filterAndAccept();
  }
}

void MozcEngine::reset(const InputMethodEntry& /*entry*/,
                       InputContextEvent& event) {
  auto* mozc_state = mozcState(event.inputContext());
  mozc_state->Reset();
}

void MozcEngine::save() {
  if (client_ == nullptr) {
    return;
  }
  client_->SyncData();
}

std::string MozcEngine::subMode(const fcitx::InputMethodEntry& /*entry*/,
                                fcitx::InputContext& ic) {
  auto* mozc_state = mozcState(&ic);
  return _(kPropCompositionModes[mozc_state->GetCompositionMode()].description);
}

std::string MozcEngine::subModeIconImpl(
    const fcitx::InputMethodEntry& /*unused*/, fcitx::InputContext& ic) {
  auto* mozc_state = mozcState(&ic);
  return _(kPropCompositionModes[mozc_state->GetCompositionMode()].icon);
}

MozcState* MozcEngine::mozcState(InputContext* ic) {
  return ic->propertyFor(&factory_);
}

void MozcEngine::compositionModeUpdated(InputContext* ic) {
  for (const auto& modeAction : modeActions_) {
    modeAction->update(ic);
  }
  ic->updateUserInterface(UserInterfaceComponent::StatusArea);
}

AddonInstance* MozcEngine::clipboardAddon() { return clipboard(); }

void MozcEngine::ResetClientPool() {
  if (pool_->policy() != GetSharedStatePolicy()) {
    instance_->inputContextManager().foreach ([this](InputContext* ic) {
      if (auto* state = this->mozcState(ic)) {
        state->ReleaseClient();
      }
      return true;
    });
    pool_->setPolicy(GetSharedStatePolicy());
  }
}

PropertyPropagatePolicy MozcEngine::GetSharedStatePolicy() {
  switch (*config_.sharedStatePolicy) {
    case SharedStatePolicy::All:
      return PropertyPropagatePolicy::All;
    case SharedStatePolicy::Program:
      return PropertyPropagatePolicy::Program;
    case SharedStatePolicy::No:
      return PropertyPropagatePolicy::No;
    case SharedStatePolicy::FollowGlobalConfig:
    default:
      return instance_->globalConfig().shareInputState();
  }
}

}  // namespace fcitx
