// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#include "unix/fcitx5/app_name_resolver.h"

#include <cstddef>
#include <string>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
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

std::string ResolveWindowId(bool gnome_service_available,
                            absl::string_view gnome_window_id,
                            bool allow_ic_fallback,
                            absl::string_view ic_uuid_id) {
  if (gnome_service_available) {
    return std::string(absl::StripAsciiWhitespace(gnome_window_id));
  }
  if (allow_ic_fallback) {
    return std::string(absl::StripAsciiWhitespace(ic_uuid_id));
  }
  return "";
}

std::string FormatInputContextWindowId(const unsigned char* uuid_bytes,
                                       size_t uuid_len) {
  if (uuid_bytes == nullptr || uuid_len == 0) {
    return "";
  }
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(uuid_len * 2);
  bool any_nonzero = false;
  for (size_t i = 0; i < uuid_len; ++i) {
    const unsigned char b = uuid_bytes[i];
    if (b != 0) {
      any_nonzero = true;
    }
    hex.push_back(kHexDigits[(b >> 4) & 0x0f]);
    hex.push_back(kHexDigits[b & 0x0f]);
  }
  if (!any_nonzero) {
    return "";
  }
  return absl::StrCat("ic:", hex);
}

bool IsMoGRUActiveInputMethod(absl::string_view im_name) {
  return im_name == "mozc" || im_name == "mogru" || im_name == "mozc-gru";
}

}  // namespace fcitx
