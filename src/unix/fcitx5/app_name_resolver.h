// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#ifndef MOZC_UNIX_FCITX5_APP_NAME_RESOLVER_H_
#define MOZC_UNIX_FCITX5_APP_NAME_RESOLVER_H_

#include <string>

#include "absl/strings/string_view.h"

namespace fcitx {

// Trims |raw_name| and returns empty string if empty or a compositor/proxy name.
std::string NormalizeUsableAppName(absl::string_view raw_name);

// Resolves the effective application name for MoGRU context/profile switching.
// Prefers |ic_program| unless it is empty or a Wayland/XWayland compositor or
// IM portal proxy name, in which case it falls back to |gnome_focused_app|
// supplied by the GNOME Shell extension.
std::string ResolveApplicationName(absl::string_view ic_program,
                                   absl::string_view gnome_focused_app);

// Returns true if |im_name| identifies an active MoGRU / Mozc input method.
bool IsMoGRUActiveInputMethod(absl::string_view im_name);

}  // namespace fcitx

#endif  // MOZC_UNIX_FCITX5_APP_NAME_RESOLVER_H_
