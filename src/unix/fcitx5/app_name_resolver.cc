// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#include "unix/fcitx5/app_name_resolver.h"

#include <string>

#include "absl/strings/ascii.h"
#include "absl/strings/string_view.h"
#include "config/gru_config.h"

namespace fcitx {

std::string NormalizeUsableAppName(absl::string_view raw_name) {
  const absl::string_view trimmed = absl::StripAsciiWhitespace(raw_name);
  if (trimmed.empty() ||
      mozc::config::GruConfigManager::IsCompositorOrProxyAppName(trimmed)) {
    return "";
  }
  return std::string(trimmed);
}

std::string ResolveApplicationName(absl::string_view ic_program,
                                   absl::string_view gnome_focused_app) {
  std::string resolved = NormalizeUsableAppName(ic_program);
  if (!resolved.empty()) {
    return resolved;
  }
  return NormalizeUsableAppName(gnome_focused_app);
}

bool IsMoGRUActiveInputMethod(absl::string_view im_name) {
  return im_name == "mozc" || im_name == "mogru" || im_name == "mozc-gru";
}

}  // namespace fcitx
