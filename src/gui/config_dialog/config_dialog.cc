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

// Qt component of configure dialog for Mozc
#include "gui/config_dialog/config_dialog.h"

#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <algorithm>
#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "base/config_file_stream.h"
#include "base/file_util.h"
#include "client/client.h"
#include "config/config_handler.h"
#include "config/conversion_stats.h"
#include "config/gru_config.h"
#include "gui/base/stats_config_util.h"
#include "gui/base/util.h"
#include "gui/config_dialog/keymap_editor.h"
#include "gui/config_dialog/roman_table_editor.h"
#include "protocol/config.pb.h"
#include "session/keymap.h"

#if defined(__ANDROID__) || defined(__wasm__)
#error "This platform is not supported."
#endif  // __ANDROID__ || __wasm__

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <QGuiApplication>
// clang-format on

#include "base/run_level.h"
#include "gui/base/win_util.h"
#endif  // _WIN32

#ifdef __APPLE__
#include "base/mac/mac_util.h"
#endif  // __APPLE__

namespace {
template <typename T>
void Connect(const QList<T*>& objects, const char* signal,
             const QObject* receiver, const char* slot) {
  for (typename QList<T*>::const_iterator itr = objects.begin();
       itr != objects.end(); ++itr) {
    QObject::connect(*itr, signal, receiver, slot);
  }
}
}  // namespace

namespace mozc {
namespace gui {

// Qt Style sheet for the config dialog.
// https://doc.qt.io/qt-6/stylesheet-reference.html
constexpr absl::string_view kQss = R"(
QFrame[class="setting-group-layout"] {
  padding: 0em 1em;
}
QFrame[class="setting-group-header"] {
}
QFrame[class="setting-group-line"] {
}
)";

ConfigDialog::ConfigDialog()
    : client_(client::ClientFactory::NewClient()),
      initial_preedit_method_(0),
      initial_use_keyboard_to_change_preedit_method_(false),
      initial_use_mode_indicator_(true) {
  setupUi(this);
  setStyleSheet(QString::fromUtf8(kQss.data(), kQss.size()));

  // Remove the context help button (question mark button) from the window.
  Qt::WindowFlags flags = windowFlags();
  flags &= ~Qt::WindowContextHelpButtonHint;
  setWindowFlags(flags);

  setWindowModality(Qt::NonModal);

#ifdef _WIN32
  miscStartupWidget->setVisible(false);
#endif  // _WIN32

#ifdef __APPLE__
  miscDefaultIMEWidget->setVisible(false);
  miscAdministrationWidget->setVisible(false);
  setWindowTitle(tr("%1 Preferences").arg(GuiUtil::ProductName()));
#endif  // __APPLE__

#if defined(__linux__)
  miscDefaultIMEWidget->setVisible(false);
  miscAdministrationWidget->setVisible(false);
  miscStartupWidget->setVisible(false);
#endif  // __linux__

#ifdef NDEBUG
  // disable logging options
  miscLoggingWidget->setVisible(false);

#if defined(__linux__)
  // The "misc" tab has no valid configs on Linux
  const int misc_tab_index = configDialogTabWidget->indexOf(miscTab);
  if (misc_tab_index >= 0) {
    configDialogTabWidget->removeTab(misc_tab_index);
  }
#endif  // __linux__
#endif  // NDEBUG

  suggestionsSizeSpinBox->setRange(1, 9);

  punctuationsSettingComboBox->addItem(QString::fromUtf8("、。"));
  punctuationsSettingComboBox->addItem(QString::fromUtf8("，．"));
  punctuationsSettingComboBox->addItem(QString::fromUtf8("、．"));
  punctuationsSettingComboBox->addItem(QString::fromUtf8("，。"));

  symbolsSettingComboBox->addItem(QString::fromUtf8("「」・"));
  symbolsSettingComboBox->addItem(QString::fromUtf8("[]／"));
  symbolsSettingComboBox->addItem(QString::fromUtf8("「」／"));
  symbolsSettingComboBox->addItem(QString::fromUtf8("[]・"));

  keymapSettingComboBox->addItem(tr("Custom keymap"));
  keymapSettingComboBox->addItem(tr("ATOK"));
  keymapSettingComboBox->addItem(tr("MS-IME"));
  keymapSettingComboBox->addItem(tr("Kotoeri"));

  keymapname_sessionkeymap_map_[tr("ATOK")] = config::Config::ATOK;
  keymapname_sessionkeymap_map_[tr("MS-IME")] = config::Config::MSIME;
  keymapname_sessionkeymap_map_[tr("Kotoeri")] = config::Config::KOTOERI;

  inputModeComboBox->addItem(tr("Romaji"));
  inputModeComboBox->addItem(tr("Kana"));
#ifdef _WIN32
  // These options changing the preedit method by a hot key are only
  // supported by Windows.
  inputModeComboBox->addItem(tr("Romaji (switchable)"));
  inputModeComboBox->addItem(tr("Kana (switchable)"));
#endif  // _WIN32

  spaceCharacterFormComboBox->addItem(tr("Follow input mode"));
  spaceCharacterFormComboBox->addItem(tr("Fullwidth"));
  spaceCharacterFormComboBox->addItem(tr("Halfwidth"));

  selectionShortcutModeComboBox->addItem(tr("No shortcut"));
  selectionShortcutModeComboBox->addItem(tr("1 -- 9"));
  selectionShortcutModeComboBox->addItem(tr("A -- L"));

  historyLearningLevelComboBox->addItem(tr("Yes"));
  historyLearningLevelComboBox->addItem(tr("Yes (don't record new data)"));
  historyLearningLevelComboBox->addItem(tr("No"));

  shiftKeyModeSwitchComboBox->addItem(tr("Off"));
  shiftKeyModeSwitchComboBox->addItem(tr("Alphanumeric"));
  shiftKeyModeSwitchComboBox->addItem(tr("Katakana"));

  numpadCharacterFormComboBox->addItem(tr("Follow input mode"));
  numpadCharacterFormComboBox->addItem(tr("Fullwidth"));
  numpadCharacterFormComboBox->addItem(tr("Halfwidth"));
  numpadCharacterFormComboBox->addItem(tr("Direct input"));

  verboseLevelComboBox->addItem(tr("0"));
  verboseLevelComboBox->addItem(tr("1"));
  verboseLevelComboBox->addItem(tr("2"));

  yenSignComboBox->addItem(tr("Yen Sign ¥"));
  yenSignComboBox->addItem(tr("Backslash \\"));

#ifndef __APPLE__
  // On Windows/Linux, yenSignCombBox can be hidden.
  yenSignLabel->hide();
  yenSignComboBox->hide();
  // On Windows/Linux, useJapaneseLayout checkbox should be invisible.
  useJapaneseLayout->hide();
#endif  // !__APPLE__

#ifndef _WIN32
  // Mode indicator is available only on Windows.
  useModeIndicator->hide();
#endif  // !_WIN32

  // Reset texts explicitly for translations.
  configDialogButtonBox->button(QDialogButtonBox::Ok)->setText(tr("  Ok  "));
  configDialogButtonBox->button(QDialogButtonBox::Cancel)
      ->setText(tr("Cancel"));
  configDialogButtonBox->button(QDialogButtonBox::Apply)->setText(tr("Apply"));

  // signal/slot
  QObject::connect(configDialogButtonBox, SIGNAL(clicked(QAbstractButton*)),
                   this, SLOT(clicked(QAbstractButton*)));
  QObject::connect(clearUserHistoryButton, SIGNAL(clicked()), this,
                   SLOT(ClearUserHistory()));
  QObject::connect(editUserDictionaryButton, SIGNAL(clicked()), this,
                   SLOT(EditUserDictionary()));
  QObject::connect(editKeymapButton, SIGNAL(clicked()), this,
                   SLOT(EditKeymap()));
  QObject::connect(resetToDefaultsButton, SIGNAL(clicked()), this,
                   SLOT(ResetToDefaults()));
  QObject::connect(editRomanTableButton, SIGNAL(clicked()), this,
                   SLOT(EditRomanTable()));
  QObject::connect(inputModeComboBox, SIGNAL(currentIndexChanged(int)), this,
                   SLOT(SelectInputModeSetting(int)));
  QObject::connect(useAutoConversion, SIGNAL(stateChanged(int)), this,
                   SLOT(SelectAutoConversionSetting(int)));
  QObject::connect(historySuggestCheckBox, SIGNAL(stateChanged(int)), this,
                   SLOT(SelectSuggestionSetting(int)));
  QObject::connect(dictionarySuggestCheckBox, SIGNAL(stateChanged(int)), this,
                   SLOT(SelectSuggestionSetting(int)));
  QObject::connect(realtimeConversionCheckBox, SIGNAL(stateChanged(int)), this,
                   SLOT(SelectSuggestionSetting(int)));
  QObject::connect(launchAdministrationDialogButton, SIGNAL(clicked()), this,
                   SLOT(LaunchAdministrationDialog()));
  QObject::connect(launchAdministrationDialogButtonForUsageStats,
                   SIGNAL(clicked()), this, SLOT(LaunchAdministrationDialog()));
  QObject::connect(browseGruModelPathButton, SIGNAL(clicked()), this,
                   SLOT(BrowseGruModelPath()));
  QObject::connect(browseGruChardataPathButton, SIGNAL(clicked()), this,
                   SLOT(BrowseGruChardataPath()));
  QObject::connect(browseGruUserModelPathButton, SIGNAL(clicked()), this,
                   SLOT(BrowseGruUserModelPath()));
  QObject::connect(resetGruUserModelButton, SIGNAL(clicked()), this,
                   SLOT(ResetGruUserModel()));
  QObject::connect(reloadGruConfigButton, SIGNAL(clicked()), this,
                   SLOT(ReloadGruConfigFromDisk()));

  SetupMogruUi();

  // Event handlers to enable 'Apply' button.
  Connect(findChildren<QPushButton*>(), SIGNAL(clicked()), this,
          SLOT(EnableApplyButton()));
  Connect(findChildren<QCheckBox*>(), SIGNAL(clicked()), this,
          SLOT(EnableApplyButton()));
  Connect(findChildren<QComboBox*>(), SIGNAL(activated(int)), this,
          SLOT(EnableApplyButton()));
  Connect(findChildren<QSpinBox*>(), SIGNAL(editingFinished()), this,
          SLOT(EnableApplyButton()));
  Connect(findChildren<QSpinBox*>(), SIGNAL(valueChanged(int)), this,
          SLOT(EnableApplyButton()));
  Connect(findChildren<QDoubleSpinBox*>(), SIGNAL(editingFinished()), this,
          SLOT(EnableApplyButton()));
  Connect(findChildren<QDoubleSpinBox*>(), SIGNAL(valueChanged(double)), this,
          SLOT(EnableApplyButton()));
  Connect(findChildren<QLineEdit*>(), SIGNAL(textEdited(QString)), this,
          SLOT(EnableApplyButton()));
  Connect(findChildren<QPlainTextEdit*>(), SIGNAL(textChanged()), this,
          SLOT(EnableApplyButton()));
  // 'Apply' button is disabled on launching.
  configDialogButtonBox->button(QDialogButtonBox::Apply)->setEnabled(false);

  // When clicking these messages, CheckBoxs corresponding
  // to them should be toggled.
  // We cannot use connect/slot as QLabel doesn't define
  // clicked slot by default.
  usageStatsMessage->installEventFilter(this);
  incognitoModeMessage->installEventFilter(this);

#ifndef _WIN32
  checkDefaultCheckBox->setVisible(false);
  checkDefaultLine->setVisible(false);
  checkDefaultLabel->setVisible(false);
#endif  // !_WIN32

#ifdef _WIN32
  launchAdministrationDialogButton->setEnabled(true);
  // if the current application is not elevated by UAC,
  // add a shield icon
  if (!mozc::RunLevel::IsElevatedByUAC()) {
    const QIcon& vista_shield_icon =
        QApplication::style()->standardIcon(QStyle::SP_VistaShield);
    launchAdministrationDialogButton->setIcon(vista_shield_icon);
    launchAdministrationDialogButtonForUsageStats->setIcon(vista_shield_icon);
  }

  usageStatsCheckBox->setDisabled(true);
  usageStatsCheckBox->setVisible(false);
  usageStatsMessage->setDisabled(true);
  usageStatsMessage->setVisible(false);
#else   // _WIN32
  launchAdministrationDialogButton->setEnabled(false);
  launchAdministrationDialogButton->setVisible(false);
  launchAdministrationDialogButtonForUsageStats->setEnabled(false);
  launchAdministrationDialogButtonForUsageStats->setVisible(false);
  administrationLine->setVisible(false);
  administrationLabel->setVisible(false);
  dictionaryPreloadingAndUACLabel->setVisible(false);
#endif  // _WIN32

#ifdef __linux__
  // On Linux, disable all fields for UsageStats
  usageStatsLabel->setEnabled(false);
  usageStatsLabel->setVisible(false);
  usageStatsLine->setEnabled(false);
  usageStatsLine->setVisible(false);
  usageStatsMessage->setEnabled(false);
  usageStatsMessage->setVisible(false);
  usageStatsCheckBox->setEnabled(false);
  usageStatsCheckBox->setVisible(false);
#endif  // __linux__

  GuiUtil::ReplaceWidgetLabels(this);

  Reload();

  gru_config_watch_timer_ = new QTimer(this);
  QObject::connect(gru_config_watch_timer_, SIGNAL(timeout()), this,
                   SLOT(CheckGruConfigFileModified()));
  gru_config_watch_timer_->start(1000);

#ifdef _WIN32
  IMEHotKeyDisabledCheckBox->setChecked(WinUtil::GetIMEHotKeyDisabled());
#else   // _WIN32
  IMEHotKeyDisabledCheckBox->setVisible(false);
#endif  // _WIN32

#ifdef CHANNEL_DEV
  usageStatsCheckBox->setEnabled(false);
#endif  // CHANNEL_DEV
}

bool ConfigDialog::SetConfig(const config::Config& config) {
#ifdef __linux__
  // On Linux, fcitx5-mogru.so runs in-process (use_server=False), so write
  // config1.db directly via ConfigHandler first to avoid spawning/mismatching
  // against a system-wide /usr/lib/mozc/mozc_server.
  config::ConfigHandler::SetConfig(config);
  return true;
#else
  if (!client_->CheckVersionOrRestartServer()) {
    LOG(WARNING) << "CheckVersionOrRestartServer failed; falling back to "
                    "direct ConfigHandler::SetConfig";
    config::ConfigHandler::SetConfig(config);
    return true;
  }

  if (!client_->SetConfig(config)) {
    LOG(WARNING) << "client_->SetConfig failed; falling back to "
                    "direct ConfigHandler::SetConfig";
    config::ConfigHandler::SetConfig(config);
    return true;
  }

  return true;
#endif
}

bool ConfigDialog::GetConfig(config::Config* config) {
#ifdef __linux__
  config::ConfigHandler::Reload();
  *config = config::ConfigHandler::GetCopiedConfig();
  return true;
#else
  if (!client_->CheckVersionOrRestartServer()) {
    LOG(WARNING) << "CheckVersionOrRestartServer failed; falling back to "
                    "direct ConfigHandler::GetConfig";
    config::ConfigHandler::Reload();
    *config = config::ConfigHandler::GetCopiedConfig();
    return true;
  }

  if (!client_->GetConfig(config)) {
    LOG(WARNING) << "client_->GetConfig failed; falling back to "
                    "direct ConfigHandler::GetConfig";
    config::ConfigHandler::Reload();
    *config = config::ConfigHandler::GetCopiedConfig();
    return true;
  }

  return true;
#endif
}

void ConfigDialog::Reload() {
  config::Config config;
  if (!GetConfig(&config)) {
    QMessageBox::critical(this, windowTitle(),
                          tr("Failed to get current config values."));
  }
  ConvertFromProto(config);
  ReloadGruConfigFromDisk();

  SelectAutoConversionSetting(static_cast<int>(config.use_auto_conversion()));

  initial_preedit_method_ = static_cast<int>(config.preedit_method());
  initial_use_keyboard_to_change_preedit_method_ =
      config.use_keyboard_to_change_preedit_method();
  initial_use_mode_indicator_ = config.use_mode_indicator();
}

bool ConfigDialog::Update() {
  config::Config config;
  ConvertToProto(&config);

  if (config.session_keymap() == config::Config::CUSTOM &&
      config.custom_keymap_table().empty()) {
    QMessageBox::warning(this, windowTitle(),
                         tr("The current custom keymap table is empty. "
                            "When custom keymap is selected, "
                            "you must customize it."));
    return false;
  }

#if defined(_WIN32)
  if ((initial_preedit_method_ != static_cast<int>(config.preedit_method())) ||
      (initial_use_keyboard_to_change_preedit_method_ !=
       config.use_keyboard_to_change_preedit_method())) {
    QMessageBox::information(this, windowTitle(),
                             tr("Romaji/Kana setting is enabled from"
                                " new applications."));
    initial_preedit_method_ = static_cast<int>(config.preedit_method());
    initial_use_keyboard_to_change_preedit_method_ =
        config.use_keyboard_to_change_preedit_method();
  }
#endif  // _WIN32

#ifdef _WIN32
  if (initial_use_mode_indicator_ != config.use_mode_indicator()) {
    QMessageBox::information(this, windowTitle(),
                             tr("Input mode indicator setting is enabled from"
                                " new applications."));
    initial_use_mode_indicator_ = config.use_mode_indicator();
  }
#endif  // _WIN32

  if (!SetConfig(config)) {
    QMessageBox::critical(this, windowTitle(), tr("Failed to update config"));
  }

  if (!SaveGruConfigFromUi()) {
    QMessageBox::warning(this, windowTitle(),
                         tr("Failed to save MoGRU configuration to "
                            "gru_config.ini."));
  }

#ifndef __linux__
  if (client_->CheckVersionOrRestartServer()) {
    client_->Reload();
  }
#endif

#ifdef _WIN32
  if (!WinUtil::SetIMEHotKeyDisabled(IMEHotKeyDisabledCheckBox->isChecked())) {
    // Do not show any dialog here, since this operation will not fail
    // in almost all cases.
    // TODO(taku): better to show dialog?
    LOG(ERROR) << "Failed to update IME HotKey status";
  }
#endif  // _WIN32

#ifdef __APPLE__
  if (startupCheckBox->isChecked()) {
    if (!MacUtil::CheckPrelauncherLoginItemStatus()) {
      MacUtil::AddPrelauncherLoginItem();
    }
  } else {
    if (MacUtil::CheckPrelauncherLoginItemStatus()) {
      MacUtil::RemovePrelauncherLoginItem();
    }
  }
#endif  // __APPLE__

  configDialogButtonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
  return true;
}

void ConfigDialog::SetSendStatsCheckBox() {
  // On windows, usage_stats flag is managed by
  // administration_dialog. http://b/2889759
#ifndef _WIN32
  const bool val = StatsConfigUtil::IsEnabled();
  usageStatsCheckBox->setChecked(val);
#endif  // _WIN32
}

void ConfigDialog::GetSendStatsCheckBox() const {
  // On windows, usage_stats flag is managed by
  // administration_dialog. http://b/2889759
#ifndef _WIN32
  const bool val = usageStatsCheckBox->isChecked();
  StatsConfigUtil::SetEnabled(val);
#endif  // _WIN32
}

#define SET_COMBOBOX(combobox, enumname, field)                    \
  do {                                                             \
    (combobox)->setCurrentIndex(static_cast<int>(config.field())); \
  } while (0)

#define SET_CHECKBOX(checkbox, field)       \
  do {                                      \
    (checkbox)->setChecked(config.field()); \
  } while (0)

#define GET_COMBOBOX(combobox, enumname, field)                              \
  do {                                                                       \
    config->set_##field(                                                     \
        static_cast<config::Config_##enumname>((combobox)->currentIndex())); \
  } while (0)

#define GET_CHECKBOX(checkbox, field)             \
  do {                                            \
    config->set_##field((checkbox)->isChecked()); \
  } while (0)

namespace {
static constexpr int kPreeditMethodSize = 2;

void SetComboboxForPreeditMethod(const config::Config& config,
                                 QComboBox* combobox) {
  int index = static_cast<int>(config.preedit_method());
#ifdef _WIN32
  if (config.use_keyboard_to_change_preedit_method()) {
    index += kPreeditMethodSize;
  }
#endif  // _WIN32
  combobox->setCurrentIndex(index);
}

void GetComboboxForPreeditMethod(const QComboBox* combobox,
                                 config::Config* config) {
  int index = combobox->currentIndex();
  if (index >= kPreeditMethodSize) {
    // |use_keyboard_to_change_preedit_method| should be true and
    // |index| should be adjusted to smaller than kPreeditMethodSize.
    config->set_preedit_method(
        static_cast<config::Config_PreeditMethod>(index - kPreeditMethodSize));
    config->set_use_keyboard_to_change_preedit_method(true);
  } else {
    config->set_preedit_method(
        static_cast<config::Config_PreeditMethod>(index));
    config->set_use_keyboard_to_change_preedit_method(false);
  }
}
}  // namespace

// TODO(taku)
// Actually ConvertFromProto and ConvertToProto are almost the same.
// The difference only SET_ and GET_. We would like to unify the twos.
void ConfigDialog::ConvertFromProto(const config::Config& config) {
  base_config_ = config;
  // tab1
  SetComboboxForPreeditMethod(config, inputModeComboBox);
  SET_COMBOBOX(punctuationsSettingComboBox, PunctuationMethod,
               punctuation_method);
  SET_COMBOBOX(symbolsSettingComboBox, SymbolMethod, symbol_method);
  SET_COMBOBOX(spaceCharacterFormComboBox, FundamentalCharacterForm,
               space_character_form);
  SET_COMBOBOX(selectionShortcutModeComboBox, SelectionShortcut,
               selection_shortcut);
  SET_COMBOBOX(numpadCharacterFormComboBox, NumpadCharacterForm,
               numpad_character_form);
  SET_COMBOBOX(keymapSettingComboBox, SessionKeymap, session_keymap);

  custom_keymap_table_ = config.custom_keymap_table();
  custom_roman_table_ = config.custom_roman_table();

  // tab2
  SET_COMBOBOX(historyLearningLevelComboBox, HistoryLearningLevel,
               history_learning_level);
  SET_CHECKBOX(singleKanjiConversionCheckBox, use_single_kanji_conversion);
  SET_CHECKBOX(symbolConversionCheckBox, use_symbol_conversion);
  SET_CHECKBOX(emoticonConversionCheckBox, use_emoticon_conversion);
  SET_CHECKBOX(dateConversionCheckBox, use_date_conversion);
  SET_CHECKBOX(emojiConversionCheckBox, use_emoji_conversion);
  SET_CHECKBOX(numberConversionCheckBox, use_number_conversion);
  SET_CHECKBOX(calculatorCheckBox, use_calculator);
  SET_CHECKBOX(t13nConversionCheckBox, use_t13n_conversion);
  SET_CHECKBOX(zipcodeConversionCheckBox, use_zip_code_conversion);
  SET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);

  // InfoListConfig
  localUsageDictionaryCheckBox->setChecked(
      config.information_list_config().use_local_usage_dictionary());

  // tab3
  SET_CHECKBOX(autoSwitchCompositionMode, auto_switch_composition_mode);

  SET_CHECKBOX(useAutoConversion, use_auto_conversion);
  kutenCheckBox->setChecked(config.auto_conversion_key() &
                            config::Config::AUTO_CONVERSION_KUTEN);
  toutenCheckBox->setChecked(config.auto_conversion_key() &
                             config::Config::AUTO_CONVERSION_TOUTEN);
  questionMarkCheckBox->setChecked(
      config.auto_conversion_key() &
      config::Config::AUTO_CONVERSION_QUESTION_MARK);
  exclamationMarkCheckBox->setChecked(
      config.auto_conversion_key() &
      config::Config::AUTO_CONVERSION_EXCLAMATION_MARK);

  SET_COMBOBOX(shiftKeyModeSwitchComboBox, ShiftKeyModeSwitch,
               shift_key_mode_switch);

  SET_CHECKBOX(useJapaneseLayout, use_japanese_layout);

  SET_CHECKBOX(useModeIndicator, use_mode_indicator);

  // tab4
  SET_CHECKBOX(historySuggestCheckBox, use_history_suggest);
  SET_CHECKBOX(dictionarySuggestCheckBox, use_dictionary_suggest);
  SET_CHECKBOX(realtimeConversionCheckBox, use_realtime_conversion);

  suggestionsSizeSpinBox->setValue(
      std::clamp<int>(config.suggestions_size(), 1, 9));

  // tab5
  SetSendStatsCheckBox();
  SET_CHECKBOX(incognitoModeCheckBox, incognito_mode);
  SET_CHECKBOX(presentationModeCheckBox, presentation_mode);

  // tab6
  SET_COMBOBOX(verboseLevelComboBox, int, verbose_level);
  SET_CHECKBOX(checkDefaultCheckBox, check_default);
  SET_COMBOBOX(yenSignComboBox, YenSignCharacter, yen_sign_character);

  characterFormEditor->Load(config);

#ifdef __APPLE__
  startupCheckBox->setChecked(MacUtil::CheckPrelauncherLoginItemStatus());
#endif  // __APPLE__
}

void ConfigDialog::ConvertToProto(config::Config* config) const {
  *config = base_config_;

  // tab1
  GetComboboxForPreeditMethod(inputModeComboBox, config);
  GET_COMBOBOX(punctuationsSettingComboBox, PunctuationMethod,
               punctuation_method);
  GET_COMBOBOX(symbolsSettingComboBox, SymbolMethod, symbol_method);
  GET_COMBOBOX(spaceCharacterFormComboBox, FundamentalCharacterForm,
               space_character_form);
  GET_COMBOBOX(selectionShortcutModeComboBox, SelectionShortcut,
               selection_shortcut);
  GET_COMBOBOX(numpadCharacterFormComboBox, NumpadCharacterForm,
               numpad_character_form);
  GET_COMBOBOX(keymapSettingComboBox, SessionKeymap, session_keymap);

  config->set_custom_keymap_table(custom_keymap_table_);

  config->clear_custom_roman_table();
  if (!custom_roman_table_.empty()) {
    config->set_custom_roman_table(custom_roman_table_);
  }

  // tab2
  GET_COMBOBOX(historyLearningLevelComboBox, HistoryLearningLevel,
               history_learning_level);
  GET_CHECKBOX(singleKanjiConversionCheckBox, use_single_kanji_conversion);
  GET_CHECKBOX(symbolConversionCheckBox, use_symbol_conversion);
  GET_CHECKBOX(emoticonConversionCheckBox, use_emoticon_conversion);
  GET_CHECKBOX(dateConversionCheckBox, use_date_conversion);
  GET_CHECKBOX(emojiConversionCheckBox, use_emoji_conversion);
  GET_CHECKBOX(numberConversionCheckBox, use_number_conversion);
  GET_CHECKBOX(calculatorCheckBox, use_calculator);
  GET_CHECKBOX(t13nConversionCheckBox, use_t13n_conversion);
  GET_CHECKBOX(zipcodeConversionCheckBox, use_zip_code_conversion);
  GET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);

  // InformationListConfig
  config->mutable_information_list_config()->set_use_local_usage_dictionary(
      localUsageDictionaryCheckBox->isChecked());

  // tab3
  GET_CHECKBOX(autoSwitchCompositionMode, auto_switch_composition_mode);

  GET_CHECKBOX(useAutoConversion, use_auto_conversion);

  GET_CHECKBOX(useJapaneseLayout, use_japanese_layout);

  GET_CHECKBOX(useModeIndicator, use_mode_indicator);

  uint32_t auto_conversion_key = 0;
  if (kutenCheckBox->isChecked()) {
    auto_conversion_key |= config::Config::AUTO_CONVERSION_KUTEN;
  }
  if (toutenCheckBox->isChecked()) {
    auto_conversion_key |= config::Config::AUTO_CONVERSION_TOUTEN;
  }
  if (questionMarkCheckBox->isChecked()) {
    auto_conversion_key |= config::Config::AUTO_CONVERSION_QUESTION_MARK;
  }
  if (exclamationMarkCheckBox->isChecked()) {
    auto_conversion_key |= config::Config::AUTO_CONVERSION_EXCLAMATION_MARK;
  }
  config->set_auto_conversion_key(auto_conversion_key);

  GET_COMBOBOX(shiftKeyModeSwitchComboBox, ShiftKeyModeSwitch,
               shift_key_mode_switch);

  // tab4
  GET_CHECKBOX(historySuggestCheckBox, use_history_suggest);
  GET_CHECKBOX(dictionarySuggestCheckBox, use_dictionary_suggest);
  GET_CHECKBOX(realtimeConversionCheckBox, use_realtime_conversion);

  config->set_suggestions_size(
      static_cast<uint32_t>(suggestionsSizeSpinBox->value()));

  // tab5
  GetSendStatsCheckBox();
  GET_CHECKBOX(incognitoModeCheckBox, incognito_mode);
  GET_CHECKBOX(presentationModeCheckBox, presentation_mode);

  // tab6
  config->set_verbose_level(verboseLevelComboBox->currentIndex());
  GET_CHECKBOX(checkDefaultCheckBox, check_default);
  GET_COMBOBOX(yenSignComboBox, YenSignCharacter, yen_sign_character);

  characterFormEditor->Save(config);
}

#undef SET_COMBOBOX
#undef SET_CHECKBOX
#undef GET_COMBOBOX
#undef GET_CHECKBOX

void ConfigDialog::clicked(QAbstractButton* button) {
  switch (configDialogButtonBox->buttonRole(button)) {
    case QDialogButtonBox::AcceptRole:
      if (Update()) {
        QWidget::close();
      }
      break;
    case QDialogButtonBox::ApplyRole:
      Update();
      break;
    case QDialogButtonBox::RejectRole:
      QWidget::close();
      break;
    default:
      break;
  }
}

void ConfigDialog::ClearUserHistory() {
  if (QMessageBox::Ok !=
      QMessageBox::question(
          this, windowTitle(), tr("Do you want to clear all history data?"),
          QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel)) {
    return;
  }

  const std::string user_model_path =
      config::GruConfigManager::ResolveUserModelFilePath(
          gruUserModelPathLineEdit->text().trimmed().toStdString());
  if (!user_model_path.empty()) {
    FileUtil::UnlinkIfExists(user_model_path).IgnoreError();
  }

  if (client_->CheckVersionOrRestartServer()) {
    if (!client_->ClearUserHistory() || !client_->ClearUserPrediction()) {
      QMessageBox::critical(this, windowTitle(),
                            tr("%1 Converter is not running. "
                               "Settings were not saved.")
                                .arg(GuiUtil::ProductName()));
    }
  }
}

void ConfigDialog::EditUserDictionary() {
  client_->LaunchTool("dictionary_tool", "");
}

void ConfigDialog::EditKeymap() {
  std::string current_keymap_table = "";
  const QString keymap_name = keymapSettingComboBox->currentText();
  const std::map<QString, config::Config::SessionKeymap>::const_iterator itr =
      keymapname_sessionkeymap_map_.find(keymap_name);
  if (itr != keymapname_sessionkeymap_map_.end()) {
    // Load from predefined mapping file.
    const char* keymap_file =
        keymap::KeyMapManager::GetKeyMapFileName(itr->second);
    std::unique_ptr<std::istream> ifs(
        ConfigFileStream::LegacyOpen(keymap_file));
    CHECK(ifs.get() != nullptr);  // should never happen
    std::stringstream buffer;
    buffer << ifs->rdbuf();
    current_keymap_table = buffer.str();
  } else {
    current_keymap_table = custom_keymap_table_;
  }
  std::string output;
  if (gui::KeyMapEditorDialog::Show(this, current_keymap_table, &output)) {
    custom_keymap_table_ = output;
    // set keymapSettingComboBox to "Custom keymap"
    keymapSettingComboBox->setCurrentIndex(0);
  }
}

void ConfigDialog::EditRomanTable() {
  std::string output;
  if (gui::RomanTableEditorDialog::Show(this, custom_roman_table_, &output)) {
    custom_roman_table_ = output;
  }
}

void ConfigDialog::SelectInputModeSetting(int index) {
  // enable "EDIT" button if roman mode is selected
  editRomanTableButton->setEnabled((index == 0));
}

void ConfigDialog::SelectAutoConversionSetting(int state) {
  kutenCheckBox->setEnabled(static_cast<bool>(state));
  toutenCheckBox->setEnabled(static_cast<bool>(state));
  questionMarkCheckBox->setEnabled(static_cast<bool>(state));
  exclamationMarkCheckBox->setEnabled(static_cast<bool>(state));
}

void ConfigDialog::SelectSuggestionSetting(int state) {
  if (historySuggestCheckBox->isChecked() ||
      dictionarySuggestCheckBox->isChecked() ||
      realtimeConversionCheckBox->isChecked()) {
    presentationModeCheckBox->setEnabled(true);
  } else {
    presentationModeCheckBox->setEnabled(false);
  }
}

void ConfigDialog::ResetToDefaults() {
  const QString message =
      tr("When you reset %1 settings, any changes "
         "you've made will be reverted to the default settings. "
         "Do you want to reset settings? "
         "The following items are not reset with this operation.\n"
         " - Personalization data\n"
         " - Input history\n"
         " - Usage statistics and crash reports\n"
         " - Administrator settings")
          .arg(GuiUtil::ProductName());
  if (QMessageBox::Ok ==
      QMessageBox::question(this, windowTitle(), message,
                            QMessageBox::Ok | QMessageBox::Cancel,
                            QMessageBox::Cancel)) {
    // TODO(taku): remove the dependency to config::ConfigHandler
    // nice to have GET_DEFAULT_CONFIG command
    ConvertFromProto(config::ConfigHandler::DefaultConfig());
    LoadGruConfigToUi(GruPenaltyConfig());
    EnableApplyButton();
  }
}

void ConfigDialog::SetupMogruUi() {
  gruOnlineLearningModeComboBox->clear();
  const bool is_ja = (QLocale::system().language() == QLocale::Japanese);
  if (is_ja) {
    gruOnlineLearningModeComboBox->addItem(
        QString::fromUtf8("出力層のみ更新 (output_only : 高速・推奨)"),
        QString::fromUtf8("output_only"));
    gruOnlineLearningModeComboBox->addItem(
        QString::fromUtf8("BPTT 全層更新 (full : 高表現力)"),
        QString::fromUtf8("full"));

    const int mogru_index = configDialogTabWidget->indexOf(mogruTab);
    if (mogru_index >= 0) {
      configDialogTabWidget->setTabText(mogru_index,
                                        QString::fromUtf8("MoGRU設定"));
    }
    const int mogru_tuning_index =
        configDialogTabWidget->indexOf(mogruTuningTab);
    if (mogru_tuning_index >= 0) {
      configDialogTabWidget->setTabText(mogru_tuning_index,
                                        QString::fromUtf8("ビームサーチ・ペナルティ"));
    }
    const int mogru_model_index =
        configDialogTabWidget->indexOf(mogruModelTab);
    if (mogru_model_index >= 0) {
      configDialogTabWidget->setTabText(mogru_model_index,
                                        QString::fromUtf8("モデル・学習"));
    }
    gruGeneralGroupBox->setTitle(
        QString::fromUtf8("MoGRU ニューラル変換・ライブ変換設定"));
    enableGruRewriterCheckBox->setText(QString::fromUtf8(
        "GRUニューラル変換（候補リランキング・生成）を有効にする"));
    enableLiveConversionCheckBox->setText(
        QString::fromUtf8("ライブ変換（入力中の自動かな漢字変換）を有効にする"));
    showGruScoreCheckBox->setText(
        QString::fromUtf8("候補ウィンドウにGRUスコアとコスト内訳を表示する"));
    showSegmentSeparatorInConversionCheckBox->setText(
        QString::fromUtf8("通常変換時（スペースキー等）に文節区切り「/」を表示する"));
    showSegmentSeparatorInLiveConversionCheckBox->setText(
        QString::fromUtf8("ライブ変換時にも文節区切り「/」を表示する"));
    enableLiveConversionPrefixFreezingCheckBox->setText(
        QString::fromUtf8("指定文字数を超えた場合に前文節を固定（計算量抑制）"));
    enableLiveConversionAutoCommitCheckBox->setText(
        QString::fromUtf8("前文節固定の次の更新タイミングで自動確定する"));
    liveConversionLengthThresholdLabel->setText(
        QString::fromUtf8("固定・自動確定トリガー文字数:"));
    liveConversionExcludedHiraganaCharactersLabel->setText(
        QString::fromUtf8("除外する直近の平仮名文字数:"));
    gruScoreWeightLabel->setText(
        QString::fromUtf8("GRUスコア重み (α / gru_score_weight):"));

    gruModelPathsGroupBox->setTitle(
        QString::fromUtf8("モデルファイル設定（空欄で規定のインストール先を使用）"));
    gruModelPathLabel->setText(
        QString::fromUtf8("ベースモデル (grumodel.bin):"));
    gruChardataPathLabel->setText(
        QString::fromUtf8("文字辞書データ (chardata.bin):"));
    gruUserModelPathLabel->setText(
        QString::fromUtf8("個人学習モデル (grumodel_user.bin):"));
    browseGruModelPathButton->setText(QString::fromUtf8("参照..."));
    browseGruChardataPathButton->setText(QString::fromUtf8("参照..."));
    browseGruUserModelPathButton->setText(QString::fromUtf8("参照..."));

    gruResegmentationGroupBox->setTitle(
        QString::fromUtf8("ビームサーチ・文節再分割 (Resegmentation)"));
    enableGruResegmentationCheckBox->setText(QString::fromUtf8(
        "不自然な文節区切りを検出した際にGRUビームサーチで再分割する"));
    gruResegmentThresholdLabel->setText(
        QString::fromUtf8("再分割トリガー閾値 (平均NLL × α):"));

    gruEnglishFallbackGroupBox->setTitle(
        QString::fromUtf8("英語自動フォールバック (高コスト未知語の英単語置換)"));
    enableEnglishFallbackCheckBox->setText(
        QString::fromUtf8("高コストセグメントの英語自動フォールバックを有効にする"));
    enableLiveConversionEnglishFallbackCheckBox->setText(
        QString::fromUtf8("ライブ変換中にも英語自動フォールバックを有効にする"));
    englishFallbackCostThresholdLabel->setText(
        QString::fromUtf8("フォールバック発動コスト閾値 (english_fallback_cost_threshold):"));
    englishFallbackMinWordLengthLabel->setText(
        QString::fromUtf8("対象最小英単語長 (english_fallback_min_word_length):"));
    userEnglishWordsLabel->setText(
        QString::fromUtf8("ユーザー追加英単語リスト (1行に1単語、またはカンマ区切り):"));

    gruPenaltyGroupBox->setTitle(
        QString::fromUtf8("文字種ペナルティ設定 (gru_config.ini)"));
    gruKatakanaMultiplierLabel->setText(
        QString::fromUtf8("カタカナ ペナルティ倍率 (katakana_multiplier):"));
    gruHiraganaMultiplierLabel->setText(
        QString::fromUtf8("ひらがな ペナルティ倍率 (hiragana_multiplier):"));
    gruHiraganaLengthPowerLabel->setText(
        QString::fromUtf8("ひらがな 連続長指数 (hiragana_length_power):"));
    gruAlphabetMultiplierLabel->setText(
        QString::fromUtf8("半角英字 ペナルティ倍率 (alphabet_multiplier):"));
    gruEmojiPenaltyLabel->setText(
        QString::fromUtf8("絵文字・記号 固定ペナルティ (emoji_penalty):"));

    gruProfileGroupBox->setTitle(
        QString::fromUtf8("コンテキストプロファイル (文脈切り替え)"));
    gruProfileLabel->setText(
        QString::fromUtf8("アクティブプロファイル (current_profile):"));
    addGruProfileButton->setText(QString::fromUtf8("追加..."));
    deleteGruProfileButton->setText(QString::fromUtf8("削除"));
    enableAppProfileSwitchingCheckBox->setText(
        QString::fromUtf8("アクティブなアプリケーションに応じたプロファイル自動切り替え"));
    appProfileMapLabel->setText(
        QString::fromUtf8("連動ルール (app:profile,...):"));

    enableInstantModeCheckBox->setText(
        QString::fromUtf8("インスタント速習モードを有効にする (極小ステップ即時適応)"));
    instantLearningRateLabel->setText(
        QString::fromUtf8("インスタント学習率 (instant_learning_rate):"));
    instantMaxWeightDeltaLabel->setText(
        QString::fromUtf8("最大重み変化量制限 (instant_max_weight_delta):"));

    gruOnlineLearningGroupBox->setTitle(
        QString::fromUtf8("オンライン個人学習 (Online Learning)"));
    enableGruOnlineLearningCheckBox->setText(
        QString::fromUtf8("変換確定時にオンライン個人学習を行う"));
    gruOnlineLearningModeLabel->setText(
        QString::fromUtf8("オンライン学習モード (online_learning_mode):"));
    gruOnlineLearningRateLabel->setText(
        QString::fromUtf8("学習率 (online_learning_rate):"));
    gruOnlineLearningWeightDecayLabel->setText(QString::fromUtf8(
        "重み減衰・忘却率 (online_learning_weight_decay):"));
    gruOnlineLearningSaveIntervalLabel->setText(
        QString::fromUtf8("学習モデル保存間隔（確定回数）:"));
    resetGruUserModelButton->setText(QString::fromUtf8(
        "個人学習したGRUモデルをリセット (grumodel_user.bin を削除)"));

    gruConfigFileGroupBox->setTitle(
        QString::fromUtf8("設定ファイル同期 (gru_config.ini)"));
    reloadGruConfigButton->setText(
        QString::fromUtf8("gru_config.ini から再読み込み"));

    gruStatsGroupBox->setTitle(
        QString::fromUtf8("変換正確率の統計"));
    liveConversionAccuracyTitleLabel->setText(
        QString::fromUtf8("ライブ変換 正確率:"));
    spaceConversionAccuracyTitleLabel->setText(
        QString::fromUtf8("スペースキー変換 正確率 (Top-1):"));
    overallStatsTitleLabel->setText(
        QString::fromUtf8("総確定数 / 総文字数:"));
    enableConversionHistoryLogCheckBox->setText(
        QString::fromUtf8("確定ごとの詳細履歴ログ (TSV) を保存する"));
    refreshConversionStatsButton->setText(
        QString::fromUtf8("最新に更新"));
    resetConversionStatsButton->setText(
        QString::fromUtf8("統計をリセット"));
  } else {
    gruOnlineLearningModeComboBox->addItem(
        QString::fromUtf8("Output layer only (output_only - fast, recommended)"),
        QString::fromUtf8("output_only"));
    gruOnlineLearningModeComboBox->addItem(
        QString::fromUtf8("Full BPTT (full - higher expressiveness)"),
        QString::fromUtf8("full"));
  }

  gruModelPathLineEdit->setPlaceholderText(QString::fromUtf8(
      config::GruConfigManager::GetDefaultDataFilePath("grumodel.bin").c_str()));
  gruChardataPathLineEdit->setPlaceholderText(QString::fromUtf8(
      config::GruConfigManager::GetDefaultDataFilePath("chardata.bin").c_str()));
  gruUserModelPathLineEdit->setPlaceholderText(QString::fromUtf8(
      config::GruConfigManager::ResolveUserModelFilePath("").c_str()));

  QObject::connect(gruProfileComboBox,
                   QOverload<int>::of(&QComboBox::currentIndexChanged),
                   this, &ConfigDialog::OnGruProfileChanged);

  QObject::connect(addGruProfileButton, &QPushButton::clicked,
                   this, &ConfigDialog::AddGruProfile);
  QObject::connect(deleteGruProfileButton, &QPushButton::clicked,
                   this, &ConfigDialog::DeleteGruProfile);

  QObject::connect(enableAppProfileSwitchingCheckBox, &QCheckBox::toggled,
                   [this](bool checked) {
                     appProfileMapLineEdit->setEnabled(checked);
                   });

  QObject::connect(enableInstantModeCheckBox, &QCheckBox::toggled,
                   [this](bool checked) {
                     instantLearningRateSpinBox->setEnabled(checked);
                     instantMaxWeightDeltaSpinBox->setEnabled(checked);
                   });

  QObject::connect(enableEnglishFallbackCheckBox, &QCheckBox::toggled,
                   [this](bool checked) {
                     enableLiveConversionEnglishFallbackCheckBox->setEnabled(checked);
                     englishFallbackCostThresholdSpinBox->setEnabled(checked);
                     englishFallbackMinWordLengthSpinBox->setEnabled(checked);
                     userEnglishWordsPlainTextEdit->setEnabled(checked);
                   });

  QObject::connect(refreshConversionStatsButton, &QPushButton::clicked,
                   this, &ConfigDialog::RefreshConversionStatsUi);
  QObject::connect(resetConversionStatsButton, &QPushButton::clicked,
                   this, &ConfigDialog::ResetConversionStats);

  RefreshConversionStatsUi();
}

void ConfigDialog::OnGruProfileChanged(int index) {
  if (index < 0) return;
  const std::string new_profile =
      gruProfileComboBox->itemData(index).toString().toStdString();
  deleteGruProfileButton->setEnabled(new_profile != "default");
  if (new_profile == current_dialog_profile_) {
    return;
  }

  // Save current UI state for the previously displayed profile
  SaveWidgetsToProfile(&working_profiles_[current_dialog_profile_]);

  // Ensure new_profile exists in working_profiles_
  if (working_profiles_.find(new_profile) == working_profiles_.end()) {
    working_profiles_[new_profile] =
        config::GruConfigManager::GetEffectiveProfileConfig(
            cached_gru_config_, new_profile);
  }

  current_dialog_profile_ = new_profile;
  PopulateWidgetsFromProfile(working_profiles_[new_profile]);
  EnableApplyButton();
}

void ConfigDialog::AddGruProfile() {
  const bool is_ja = (QLocale::system().language() == QLocale::Japanese);
  bool ok = false;
  const QString title =
      is_ja ? QString::fromUtf8("プロファイルの追加")
            : QString::fromUtf8("Add Profile");
  const QString label =
      is_ja ? QString::fromUtf8(
                  "新しいプロファイル名を入力してください (半角英数字・_・-):\n"
                  "※現在表示中のプロファイルの設定値を初期値として引き継ぎます。")
            : QString::fromUtf8(
                  "Enter new profile name (alphanumeric, '_', '-'):\n"
                  "* Initial settings will be copied from the currently displayed profile.");
  const QString input = QInputDialog::getText(
      this, title, label, QLineEdit::Normal, QString(), &ok);
  if (!ok || input.trimmed().isEmpty()) {
    return;
  }

  const std::string raw_name = input.trimmed().toStdString();
  const std::string sanitized =
      config::GruConfigManager::SanitizeProfileName(raw_name);
  if (sanitized.empty() || sanitized == "default") {
    const QString warn_msg =
        is_ja ? QString::fromUtf8(
                    "無効なプロファイル名です。半角英数字（default 以外）を指定してください。")
              : QString::fromUtf8(
                    "Invalid profile name. Please specify an alphanumeric name other than 'default'.");
    QMessageBox::warning(this, title, warn_msg);
    return;
  }

  if (working_profiles_.find(sanitized) != working_profiles_.end()) {
    const QString warn_msg =
        is_ja ? QString::fromUtf8("プロファイル「%1」は既に存在します。")
                    .arg(QString::fromUtf8(sanitized.c_str()))
              : QString::fromUtf8("Profile '%1' already exists.")
                    .arg(QString::fromUtf8(sanitized.c_str()));
    QMessageBox::warning(this, title, warn_msg);
    return;
  }

  // Save current widgets to the current profile first
  SaveWidgetsToProfile(&working_profiles_[current_dialog_profile_]);

  // Initialize new profile from the currently displayed profile's settings
  GruPenaltyConfig new_cfg = working_profiles_[current_dialog_profile_];
  new_cfg.current_profile = sanitized;
  working_profiles_[sanitized] = new_cfg;
  working_deleted_profiles_.erase(sanitized);

  auto format_profile_label = [is_ja](const std::string& prof_name) -> QString {
    if (prof_name == "default") {
      return is_ja ? QString::fromUtf8("標準 (default)")
                   : QString::fromUtf8("Default (default)");
    } else if (prof_name == "business") {
      return is_ja ? QString::fromUtf8("ビジネス (business)")
                   : QString::fromUtf8("Business (business)");
    } else if (prof_name == "coding") {
      return is_ja ? QString::fromUtf8("コーディング (coding)")
                   : QString::fromUtf8("Coding (coding)");
    } else if (prof_name == "casual") {
      return is_ja ? QString::fromUtf8("日常・SNS (casual)")
                   : QString::fromUtf8("Casual / SNS (casual)");
    } else if (prof_name == "creative") {
      return is_ja ? QString::fromUtf8("創作・文芸 (creative)")
                   : QString::fromUtf8("Creative (creative)");
    } else if (prof_name == "instant") {
      return is_ja ? QString::fromUtf8("インスタント (instant)")
                   : QString::fromUtf8("Instant (instant)");
    }
    return QString::fromUtf8(prof_name.c_str());
  };

  gruProfileComboBox->blockSignals(true);
  gruProfileComboBox->addItem(format_profile_label(sanitized),
                              QString::fromUtf8(sanitized.c_str()));
  const int new_idx = gruProfileComboBox->count() - 1;
  gruProfileComboBox->setCurrentIndex(new_idx);
  gruProfileComboBox->blockSignals(false);

  current_dialog_profile_ = sanitized;
  deleteGruProfileButton->setEnabled(true);
  PopulateWidgetsFromProfile(working_profiles_[sanitized]);
  EnableApplyButton();
}

void ConfigDialog::DeleteGruProfile() {
  const int prof_idx = gruProfileComboBox->currentIndex();
  if (prof_idx < 0) return;
  const std::string target_prof =
      gruProfileComboBox->itemData(prof_idx).toString().toStdString();
  if (target_prof.empty() || target_prof == "default") {
    return;
  }

  const bool is_ja = (QLocale::system().language() == QLocale::Japanese);
  const QString prompt =
      is_ja ? QString::fromUtf8(
                  "プロファイル「%1」を削除しますか？\n"
                  "（このプロファイル専用の学習データ・履歴ファイルも削除されます）")
                  .arg(QString::fromUtf8(target_prof.c_str()))
            : QString::fromUtf8(
                  "Do you want to delete profile '%1'?\n"
                  "(Dedicated personalized user model and history files for this profile will also be deleted.)")
                  .arg(QString::fromUtf8(target_prof.c_str()));
  if (QMessageBox::Ok !=
      QMessageBox::question(this, windowTitle(), prompt,
                            QMessageBox::Ok | QMessageBox::Cancel,
                            QMessageBox::Cancel)) {
    return;
  }

  // Delete dedicated data files and clean up app_profile_map using GruConfigManager::DeleteProfile
  GruPenaltyConfig temp_cfg = cached_gru_config_;
  temp_cfg.gru_user_model_path =
      gruUserModelPathLineEdit->text().trimmed().toStdString();
  temp_cfg.app_profile_map =
      appProfileMapLineEdit->text().trimmed().toStdString();
  config::GruConfigManager::DeleteProfile(&temp_cfg, target_prof,
                                          /*remove_data_files=*/true);
  appProfileMapLineEdit->setText(
      QString::fromUtf8(temp_cfg.app_profile_map.c_str()));

  working_profiles_.erase(target_prof);
  working_deleted_profiles_.insert(target_prof);

  gruProfileComboBox->blockSignals(true);
  gruProfileComboBox->removeItem(prof_idx);
  int default_idx = 0;
  for (int i = 0; i < gruProfileComboBox->count(); ++i) {
    if (gruProfileComboBox->itemData(i).toString().toStdString() == "default") {
      default_idx = i;
      break;
    }
  }
  gruProfileComboBox->setCurrentIndex(default_idx);
  gruProfileComboBox->blockSignals(false);

  current_dialog_profile_ = "default";
  deleteGruProfileButton->setEnabled(false);
  PopulateWidgetsFromProfile(working_profiles_["default"]);
  EnableApplyButton();
}

void ConfigDialog::SaveWidgetsToProfile(GruPenaltyConfig* p) const {
  if (p == nullptr) return;
  p->gru_score_weight = gruScoreWeightSpinBox->value();
  p->enable_resegmentation = enableGruResegmentationCheckBox->isChecked();
  p->resegment_threshold = gruResegmentThresholdSpinBox->value();
  p->katakana_multiplier = gruKatakanaMultiplierSpinBox->value();
  p->hiragana_multiplier = gruHiraganaMultiplierSpinBox->value();
  p->hiragana_length_power = gruHiraganaLengthPowerSpinBox->value();
  p->alphabet_multiplier = gruAlphabetMultiplierSpinBox->value();
  p->emoji_penalty = gruEmojiPenaltySpinBox->value();
  p->enable_english_fallback = enableEnglishFallbackCheckBox->isChecked();
  p->english_fallback_cost_threshold = englishFallbackCostThresholdSpinBox->value();
  p->enable_online_learning = enableGruOnlineLearningCheckBox->isChecked();
  p->online_learning_mode =
      (gruOnlineLearningModeComboBox->currentIndex() == 1) ? "full" : "output_only";
  p->online_learning_rate = gruOnlineLearningRateSpinBox->value();
  p->online_learning_weight_decay = gruOnlineLearningWeightDecaySpinBox->value();
  p->enable_instant_mode = enableInstantModeCheckBox->isChecked();
  p->instant_learning_rate = instantLearningRateSpinBox->value();
  p->instant_max_weight_delta = instantMaxWeightDeltaSpinBox->value();

  p->user_english_words.clear();
  const std::string words_str =
      userEnglishWordsPlainTextEdit->toPlainText().toStdString();
  for (absl::string_view w : absl::StrSplit(words_str, absl::ByAnyChar(",\r\n;"))) {
    absl::string_view trimmed = absl::StripAsciiWhitespace(w);
    if (!trimmed.empty()) {
      p->user_english_words.push_back(std::string(trimmed));
    }
  }
}

void ConfigDialog::PopulateWidgetsFromProfile(const GruPenaltyConfig& p) {
  gruScoreWeightSpinBox->blockSignals(true);
  enableGruResegmentationCheckBox->blockSignals(true);
  gruResegmentThresholdSpinBox->blockSignals(true);
  gruKatakanaMultiplierSpinBox->blockSignals(true);
  gruHiraganaMultiplierSpinBox->blockSignals(true);
  gruHiraganaLengthPowerSpinBox->blockSignals(true);
  gruAlphabetMultiplierSpinBox->blockSignals(true);
  gruEmojiPenaltySpinBox->blockSignals(true);
  enableEnglishFallbackCheckBox->blockSignals(true);
  englishFallbackCostThresholdSpinBox->blockSignals(true);
  userEnglishWordsPlainTextEdit->blockSignals(true);
  enableGruOnlineLearningCheckBox->blockSignals(true);
  gruOnlineLearningModeComboBox->blockSignals(true);
  gruOnlineLearningRateSpinBox->blockSignals(true);
  gruOnlineLearningWeightDecaySpinBox->blockSignals(true);
  enableInstantModeCheckBox->blockSignals(true);
  instantLearningRateSpinBox->blockSignals(true);
  instantMaxWeightDeltaSpinBox->blockSignals(true);

  gruScoreWeightSpinBox->setValue(p.gru_score_weight);
  enableGruResegmentationCheckBox->setChecked(p.enable_resegmentation);
  gruResegmentThresholdSpinBox->setValue(p.resegment_threshold);
  gruKatakanaMultiplierSpinBox->setValue(p.katakana_multiplier);
  gruHiraganaMultiplierSpinBox->setValue(p.hiragana_multiplier);
  gruHiraganaLengthPowerSpinBox->setValue(p.hiragana_length_power);
  gruAlphabetMultiplierSpinBox->setValue(p.alphabet_multiplier);
  gruEmojiPenaltySpinBox->setValue(p.emoji_penalty);

  enableEnglishFallbackCheckBox->setChecked(p.enable_english_fallback);
  englishFallbackCostThresholdSpinBox->setValue(p.english_fallback_cost_threshold);
  enableLiveConversionEnglishFallbackCheckBox->setEnabled(p.enable_english_fallback);
  englishFallbackCostThresholdSpinBox->setEnabled(p.enable_english_fallback);
  englishFallbackMinWordLengthSpinBox->setEnabled(p.enable_english_fallback);
  userEnglishWordsPlainTextEdit->setEnabled(p.enable_english_fallback);

  const std::string words_joined = absl::StrJoin(p.user_english_words, "\n");
  userEnglishWordsPlainTextEdit->setPlainText(
      QString::fromUtf8(words_joined.c_str()));

  enableGruOnlineLearningCheckBox->setChecked(p.enable_online_learning);
  const int mode_index =
      (p.online_learning_mode == "full" || p.online_learning_mode == "bptt" ||
       p.online_learning_mode == "full_bptt")
          ? 1
          : 0;
  gruOnlineLearningModeComboBox->setCurrentIndex(mode_index);
  gruOnlineLearningRateSpinBox->setValue(p.online_learning_rate);
  gruOnlineLearningWeightDecaySpinBox->setValue(p.online_learning_weight_decay);

  enableInstantModeCheckBox->setChecked(p.enable_instant_mode);
  instantLearningRateSpinBox->setValue(p.instant_learning_rate);
  instantMaxWeightDeltaSpinBox->setValue(p.instant_max_weight_delta);
  instantLearningRateSpinBox->setEnabled(p.enable_instant_mode);
  instantMaxWeightDeltaSpinBox->setEnabled(p.enable_instant_mode);

  const std::string custom_user_model =
      gruUserModelPathLineEdit->text().trimmed().toStdString();
  const std::string prof_to_resolve =
      p.current_profile.empty() ? "default" : p.current_profile;
  gruUserModelPathLineEdit->setPlaceholderText(
      QString::fromUtf8(config::GruConfigManager::ResolveProfileUserModelFilePath(
                            prof_to_resolve, custom_user_model)
                            .c_str()));

  gruScoreWeightSpinBox->blockSignals(false);
  enableGruResegmentationCheckBox->blockSignals(false);
  gruResegmentThresholdSpinBox->blockSignals(false);
  gruKatakanaMultiplierSpinBox->blockSignals(false);
  gruHiraganaMultiplierSpinBox->blockSignals(false);
  gruHiraganaLengthPowerSpinBox->blockSignals(false);
  gruAlphabetMultiplierSpinBox->blockSignals(false);
  gruEmojiPenaltySpinBox->blockSignals(false);
  enableEnglishFallbackCheckBox->blockSignals(false);
  englishFallbackCostThresholdSpinBox->blockSignals(false);
  userEnglishWordsPlainTextEdit->blockSignals(false);
  enableGruOnlineLearningCheckBox->blockSignals(false);
  gruOnlineLearningModeComboBox->blockSignals(false);
  gruOnlineLearningRateSpinBox->blockSignals(false);
  gruOnlineLearningWeightDecaySpinBox->blockSignals(false);
  enableInstantModeCheckBox->blockSignals(false);
  instantLearningRateSpinBox->blockSignals(false);
  instantMaxWeightDeltaSpinBox->blockSignals(false);
}

void ConfigDialog::LoadGruConfigToUi(const GruPenaltyConfig& gru_config) {
  cached_gru_config_ = gru_config;
  working_profiles_.clear();
  working_deleted_profiles_ = gru_config.deleted_profiles;

  // 1. Initialize default profile working copy from base config
  working_profiles_["default"] = gru_config;
  working_profiles_["default"].current_profile = "default";

  // 2. Initialize available profiles (builtin presets not deleted + custom profiles)
  const std::vector<std::string> avail_profiles =
      config::GruConfigManager::GetAvailableProfiles(gru_config);
  std::set<std::string> known_profiles;
  for (const auto& prof_name : avail_profiles) {
    known_profiles.insert(prof_name);
    if (prof_name != "default") {
      working_profiles_[prof_name] =
          config::GruConfigManager::GetEffectiveProfileConfig(gru_config, prof_name);
    }
  }

  std::string active_prof =
      gru_config.current_profile.empty() ? "default" : gru_config.current_profile;
  if (known_profiles.find(active_prof) == known_profiles.end()) {
    active_prof = "default";
  }
  current_dialog_profile_ = active_prof;
  deleteGruProfileButton->setEnabled(active_prof != "default");

  gruProfileComboBox->blockSignals(true);
  gruProfileComboBox->clear();
  const bool is_ja = (QLocale::system().language() == QLocale::Japanese);
  int selected_idx = 0;
  int current_idx = 0;
  for (const auto& prof_name : known_profiles) {
    QString label;
    if (prof_name == "default") {
      label = is_ja ? QString::fromUtf8("標準 (default)")
                    : QString::fromUtf8("Default (default)");
    } else if (prof_name == "business") {
      label = is_ja ? QString::fromUtf8("ビジネス (business)")
                    : QString::fromUtf8("Business (business)");
    } else if (prof_name == "coding") {
      label = is_ja ? QString::fromUtf8("コーディング (coding)")
                    : QString::fromUtf8("Coding (coding)");
    } else if (prof_name == "casual") {
      label = is_ja ? QString::fromUtf8("日常・SNS (casual)")
                    : QString::fromUtf8("Casual / SNS (casual)");
    } else if (prof_name == "creative") {
      label = is_ja ? QString::fromUtf8("創作・文芸 (creative)")
                    : QString::fromUtf8("Creative (creative)");
    } else if (prof_name == "instant") {
      label = is_ja ? QString::fromUtf8("インスタント (instant)")
                    : QString::fromUtf8("Instant (instant)");
    } else {
      label = QString::fromUtf8(prof_name.c_str());
    }
    gruProfileComboBox->addItem(label, QString::fromUtf8(prof_name.c_str()));
    if (prof_name == active_prof) {
      selected_idx = current_idx;
    }
    current_idx++;
  }
  gruProfileComboBox->setCurrentIndex(selected_idx);
  gruProfileComboBox->blockSignals(false);

  // Global settings
  enableGruRewriterCheckBox->setChecked(gru_config.enable_gru_rewriter);
  enableLiveConversionCheckBox->setChecked(gru_config.enable_live_conversion);
  showGruScoreCheckBox->setChecked(gru_config.show_gru_score);
  showSegmentSeparatorInConversionCheckBox->setChecked(
      gru_config.show_segment_separator_in_conversion);
  showSegmentSeparatorInLiveConversionCheckBox->setChecked(
      gru_config.show_segment_separator_in_live_conversion);
  enableLiveConversionPrefixFreezingCheckBox->setChecked(
      gru_config.enable_live_conversion_prefix_freezing);
  enableLiveConversionAutoCommitCheckBox->setChecked(
      gru_config.enable_live_conversion_auto_commit);
  liveConversionLengthThresholdSpinBox->setValue(
      gru_config.live_conversion_length_threshold);
  liveConversionExcludedHiraganaCharactersSpinBox->setValue(
      gru_config.live_conversion_excluded_hiragana_characters);

  gruModelPathLineEdit->setText(
      QString::fromUtf8(gru_config.gru_model_path.c_str()));
  gruChardataPathLineEdit->setText(
      QString::fromUtf8(gru_config.gru_chardata_path.c_str()));
  gruUserModelPathLineEdit->setText(
      QString::fromUtf8(gru_config.gru_user_model_path.c_str()));

  enableLiveConversionEnglishFallbackCheckBox->setChecked(
      gru_config.enable_live_conversion_english_fallback);
  englishFallbackMinWordLengthSpinBox->setValue(
      gru_config.english_fallback_min_word_length);
  gruOnlineLearningSaveIntervalSpinBox->setValue(
      std::max<int>(1, gru_config.online_learning_save_interval));
  enableConversionHistoryLogCheckBox->setChecked(
      gru_config.enable_conversion_history_log);

  enableAppProfileSwitchingCheckBox->setChecked(
      gru_config.enable_app_profile_switching);
  appProfileMapLineEdit->setText(
      QString::fromUtf8(gru_config.app_profile_map.c_str()));
  appProfileMapLineEdit->setEnabled(gru_config.enable_app_profile_switching);

  // Populate active profile's specific parameters
  PopulateWidgetsFromProfile(working_profiles_[active_prof]);
  RefreshConversionStatsUi();
}

GruPenaltyConfig ConfigDialog::CollectGruConfigFromUi() const {
  GruPenaltyConfig cfg = cached_gru_config_;

  cfg.enable_gru_rewriter = enableGruRewriterCheckBox->isChecked();
  cfg.enable_live_conversion = enableLiveConversionCheckBox->isChecked();
  cfg.show_gru_score = showGruScoreCheckBox->isChecked();
  cfg.show_segment_separator_in_conversion =
      showSegmentSeparatorInConversionCheckBox->isChecked();
  cfg.show_segment_separator_in_live_conversion =
      showSegmentSeparatorInLiveConversionCheckBox->isChecked();
  cfg.enable_live_conversion_prefix_freezing =
      enableLiveConversionPrefixFreezingCheckBox->isChecked();
  cfg.enable_live_conversion_auto_commit =
      enableLiveConversionAutoCommitCheckBox->isChecked();
  cfg.live_conversion_length_threshold =
      liveConversionLengthThresholdSpinBox->value();
  cfg.live_conversion_excluded_hiragana_characters =
      liveConversionExcludedHiraganaCharactersSpinBox->value();

  cfg.gru_model_path =
      gruModelPathLineEdit->text().trimmed().toStdString();
  cfg.gru_chardata_path =
      gruChardataPathLineEdit->text().trimmed().toStdString();
  cfg.gru_user_model_path =
      gruUserModelPathLineEdit->text().trimmed().toStdString();

  cfg.enable_live_conversion_english_fallback =
      enableLiveConversionEnglishFallbackCheckBox->isChecked();
  cfg.english_fallback_min_word_length =
      englishFallbackMinWordLengthSpinBox->value();
  cfg.online_learning_save_interval =
      gruOnlineLearningSaveIntervalSpinBox->value();
  cfg.enable_conversion_history_log =
      enableConversionHistoryLogCheckBox->isChecked();

  // Profile selection
  const int prof_idx = gruProfileComboBox->currentIndex();
  const std::string selected_prof =
      (prof_idx >= 0)
          ? gruProfileComboBox->itemData(prof_idx).toString().toStdString()
          : "default";
  cfg.current_profile = selected_prof;
  cfg.enable_app_profile_switching =
      enableAppProfileSwitchingCheckBox->isChecked();
  cfg.app_profile_map = appProfileMapLineEdit->text().trimmed().toStdString();

  // Make a working copy of working_profiles_ and save currently visible widgets into it
  auto wp = working_profiles_;
  auto it_active = wp.find(current_dialog_profile_);
  if (it_active != wp.end()) {
    SaveWidgetsToProfile(&it_active->second);
  }

  // If default was modified, update base fields of cfg
  auto it_def = wp.find("default");
  if (it_def != wp.end()) {
    const auto& def = it_def->second;
    cfg.gru_score_weight = def.gru_score_weight;
    cfg.enable_resegmentation = def.enable_resegmentation;
    cfg.resegment_threshold = def.resegment_threshold;
    cfg.katakana_multiplier = def.katakana_multiplier;
    cfg.hiragana_multiplier = def.hiragana_multiplier;
    cfg.hiragana_length_power = def.hiragana_length_power;
    cfg.alphabet_multiplier = def.alphabet_multiplier;
    cfg.emoji_penalty = def.emoji_penalty;
    cfg.enable_english_fallback = def.enable_english_fallback;
    cfg.english_fallback_cost_threshold = def.english_fallback_cost_threshold;
    cfg.user_english_words = def.user_english_words;
    cfg.enable_online_learning = def.enable_online_learning;
    cfg.online_learning_mode = def.online_learning_mode;
    cfg.online_learning_rate = def.online_learning_rate;
    cfg.online_learning_weight_decay = def.online_learning_weight_decay;
    cfg.enable_instant_mode = def.enable_instant_mode;
    cfg.instant_learning_rate = def.instant_learning_rate;
    cfg.instant_max_weight_delta = def.instant_max_weight_delta;
  }

  // Update cfg.profiles with non-default profiles and record deleted profiles
  cfg.profiles.clear();
  cfg.deleted_profiles = working_deleted_profiles_;
  for (const auto& kv : wp) {
    if (kv.first == "default") continue;
    cfg.profiles[kv.first] = kv.second;
  }

  return cfg;
}

bool ConfigDialog::SaveGruConfigFromUi() {
  const GruPenaltyConfig cfg = CollectGruConfigFromUi();
  const bool ok = config::GruConfigManager::SaveConfig(cfg, "");
  const std::string saved_path =
      config::GruConfigManager::ResolveConfigFilePath("",
                                                      /*allow_in_test=*/true);
  if (!saved_path.empty()) {
    gru_config_path_ = saved_path;
    gru_config_mtime_ = config::GruConfigManager::GetFileMtime(saved_path);
    gruConfigFilePathLabel->setText(QString::fromUtf8(saved_path.c_str()));
  }
  return ok;
}

void ConfigDialog::ReloadGruConfigFromDisk() {
  const std::string path =
      config::GruConfigManager::ResolveConfigFilePath("",
                                                      /*allow_in_test=*/true);
  const GruPenaltyConfig cfg = config::GruConfigManager::LoadConfig(path);
  gru_config_path_ = path;
  gru_config_mtime_ = config::GruConfigManager::GetFileMtime(path);
  if (!path.empty()) {
    gruConfigFilePathLabel->setText(QString::fromUtf8(path.c_str()));
  }
  LoadGruConfigToUi(cfg);
}

void ConfigDialog::CheckGruConfigFileModified() {
  if (gru_config_path_.empty()) {
    return;
  }
  const int64_t current_mtime =
      config::GruConfigManager::GetFileMtime(gru_config_path_);
  if (current_mtime > 0 && current_mtime != gru_config_mtime_) {
    const bool apply_was_enabled =
        configDialogButtonBox->button(QDialogButtonBox::Apply)->isEnabled();
    ReloadGruConfigFromDisk();
    configDialogButtonBox->button(QDialogButtonBox::Apply)
        ->setEnabled(apply_was_enabled);
  }
}

void ConfigDialog::BrowseGruModelPath() {
  const QString initial =
      gruModelPathLineEdit->text().isEmpty()
          ? gruModelPathLineEdit->placeholderText()
          : gruModelPathLineEdit->text();
  const QString selected = QFileDialog::getOpenFileName(
      this, tr("Select GRU Model File (grumodel.bin)"), initial,
      tr("Binary Models (*.bin);;All Files (*)"));
  if (!selected.isEmpty()) {
    gruModelPathLineEdit->setText(selected);
    EnableApplyButton();
  }
}

void ConfigDialog::BrowseGruChardataPath() {
  const QString initial =
      gruChardataPathLineEdit->text().isEmpty()
          ? gruChardataPathLineEdit->placeholderText()
          : gruChardataPathLineEdit->text();
  const QString selected = QFileDialog::getOpenFileName(
      this, tr("Select Char Data File (chardata.bin)"), initial,
      tr("Binary Data (*.bin);;All Files (*)"));
  if (!selected.isEmpty()) {
    gruChardataPathLineEdit->setText(selected);
    EnableApplyButton();
  }
}

void ConfigDialog::BrowseGruUserModelPath() {
  const QString initial =
      gruUserModelPathLineEdit->text().isEmpty()
          ? gruUserModelPathLineEdit->placeholderText()
          : gruUserModelPathLineEdit->text();
  const QString selected = QFileDialog::getSaveFileName(
      this, tr("Select User Model File (grumodel_user.bin)"), initial,
      tr("Binary Models (*.bin);;All Files (*)"), nullptr,
      QFileDialog::DontConfirmOverwrite);
  if (!selected.isEmpty()) {
    gruUserModelPathLineEdit->setText(selected);
    EnableApplyButton();
  }
}

void ConfigDialog::ResetGruUserModel() {
  const int prof_idx = gruProfileComboBox->currentIndex();
  const std::string active_prof =
      (prof_idx >= 0)
          ? gruProfileComboBox->itemData(prof_idx).toString().toStdString()
          : "default";
  const bool is_ja = (QLocale::system().language() == QLocale::Japanese);
  const QString prompt =
      is_ja ? QString::fromUtf8(
                  "選択中プロファイル「%1」の個人用GRUモデルを削除し、"
                  "初期モデルへ戻しますか？")
                  .arg(QString::fromUtf8(active_prof.c_str()))
            : tr("Do you want to delete the personalized GRU user model for "
                 "profile '%1' and revert to the base model?")
                  .arg(QString::fromUtf8(active_prof.c_str()));
  if (QMessageBox::Ok !=
      QMessageBox::question(this, windowTitle(), prompt,
                            QMessageBox::Ok | QMessageBox::Cancel,
                            QMessageBox::Cancel)) {
    return;
  }

  const std::string custom_path =
      gruUserModelPathLineEdit->text().trimmed().toStdString();
  const std::string path =
      config::GruConfigManager::ResolveProfileUserModelFilePath(active_prof,
                                                                custom_path);
  if (!path.empty()) {
    FileUtil::UnlinkIfExists(path).IgnoreError();
  }
  const QString done_msg =
      is_ja ? QString::fromUtf8("個人学習モデルをリセットしました:\n%1")
                  .arg(QString::fromUtf8(path.c_str()))
            : tr("Personalized GRU user model has been reset:\n%1")
                  .arg(QString::fromUtf8(path.c_str()));
  QMessageBox::information(this, windowTitle(), done_msg);
}

void ConfigDialog::LaunchAdministrationDialog() {
#ifdef _WIN32
  client_->LaunchTool("administration_dialog", "");
#endif  // _WIN32
}

void ConfigDialog::RefreshConversionStatsUi() {
  const config::ConversionStats stats =
      config::ConversionStatsManager::LoadStats();

  const bool is_ja = QLocale().language() == QLocale::Japanese;

  // Live conversion stats
  const uint64_t live_total =
      stats.live.commit_count + stats.live.fallback_to_space_count;
  QString live_str;
  if (live_total == 0) {
    live_str = is_ja
                   ? QString::fromUtf8("-- % (確定: 0回 / スペース移行: 0回)")
                   : QString::fromUtf8("-- % (Commits: 0, Fallback to Space: 0)");
  } else {
    const double live_acc = stats.live.AccuracyRate() * 100.0;
    if (is_ja) {
      live_str = QString::fromUtf8("%1 % (確定: %2回 / スペース移行: %3回)")
                     .arg(live_acc, 0, 'f', 1)
                     .arg(stats.live.commit_count)
                     .arg(stats.live.fallback_to_space_count);
    } else {
      live_str = QString::fromUtf8("%1 % (Commits: %2, Fallback to Space: %3)")
                     .arg(live_acc, 0, 'f', 1)
                     .arg(stats.live.commit_count)
                     .arg(stats.live.fallback_to_space_count);
    }
  }
  liveConversionAccuracyValueLabel->setText(live_str);

  // Space conversion stats
  QString space_str;
  if (stats.space.commit_count == 0) {
    space_str = is_ja ? QString::fromUtf8(
                            "-- % (確定単位: 0/0回, 文節単位: 0/0文節)")
                      : QString::fromUtf8(
                            "-- % (Commits: 0/0, Segments: 0/0)");
  } else {
    const double space_commit_acc = stats.space.CommitAccuracyRate() * 100.0;
    const double space_seg_acc = stats.space.SegmentAccuracyRate() * 100.0;
    if (is_ja) {
      space_str =
          QString::fromUtf8(
              "%1 % (確定単位: %2/%3回, 文節単位: %4/%5文節 [ %6 % ])")
              .arg(space_commit_acc, 0, 'f', 1)
              .arg(stats.space.top1_commit_count)
              .arg(stats.space.commit_count)
              .arg(stats.space.top1_segments)
              .arg(stats.space.total_segments)
              .arg(space_seg_acc, 0, 'f', 1);
    } else {
      space_str =
          QString::fromUtf8(
              "%1 % (Commits: %2/%3, Segments: %4/%5 [ %6 % ])")
              .arg(space_commit_acc, 0, 'f', 1)
              .arg(stats.space.top1_commit_count)
              .arg(stats.space.commit_count)
              .arg(stats.space.top1_segments)
              .arg(stats.space.total_segments)
              .arg(space_seg_acc, 0, 'f', 1);
    }
  }
  spaceConversionAccuracyValueLabel->setText(space_str);

  // Overall stats
  QString overall_str;
  if (stats.TotalCommits() == 0) {
    overall_str = is_ja
                      ? QString::fromUtf8("総確定: 0回 / 総文字数: 0文字")
                      : QString::fromUtf8("Total: 0 commits, 0 characters");
  } else {
    const double overall_acc = stats.OverallAccuracyRate() * 100.0;
    if (is_ja) {
      overall_str = QString::fromUtf8(
                        "総確定: %1回 / 総文字数: %2文字 (無修正確定率: %3 %)")
                        .arg(stats.TotalCommits())
                        .arg(stats.TotalCharacters())
                        .arg(overall_acc, 0, 'f', 1);
    } else {
      overall_str = QString::fromUtf8(
                        "Total: %1 commits, %2 chars (Top-1 Rate: %3 %)")
                        .arg(stats.TotalCommits())
                        .arg(stats.TotalCharacters())
                        .arg(overall_acc, 0, 'f', 1);
    }
  }
  overallStatsValueLabel->setText(overall_str);
}

void ConfigDialog::ResetConversionStats() {
  const bool is_ja = QLocale().language() == QLocale::Japanese;
  const QString prompt =
      is_ja ? QString::fromUtf8(
                  "変換正確率の統計を初期化（リセット）しますか？")
            : tr("Are you sure you want to reset conversion accuracy statistics?");
  if (QMessageBox::Ok !=
      QMessageBox::question(this, windowTitle(), prompt,
                            QMessageBox::Ok | QMessageBox::Cancel,
                            QMessageBox::Cancel)) {
    return;
  }

  config::ConversionStatsManager::ResetStats();
  RefreshConversionStatsUi();

  const QString done_msg =
      is_ja ? QString::fromUtf8("変換正確率の統計をリセットしました。")
            : tr("Conversion accuracy statistics have been reset.");
  QMessageBox::information(this, windowTitle(), done_msg);
}

void ConfigDialog::EnableApplyButton() {
  configDialogButtonBox->button(QDialogButtonBox::Apply)->setEnabled(true);
}

// Catch MouseButtonRelease event to toggle the CheckBoxes
bool ConfigDialog::eventFilter(QObject* obj, QEvent* event) {
  if (event->type() == QEvent::MouseButtonRelease) {
    if (obj == usageStatsMessage) {
#ifndef CHANNEL_DEV
      usageStatsCheckBox->toggle();
#endif  // CHANNEL_DEV
    } else if (obj == incognitoModeMessage) {
      incognitoModeCheckBox->toggle();
    }
  }
  return QObject::eventFilter(obj, event);
}

}  // namespace gui
}  // namespace mozc
