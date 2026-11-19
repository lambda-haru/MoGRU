// Copyright 2026 Haruhiro Obora
// SPDX-License-Identifier: BSD-3-Clause

#ifndef MOZC_UNIX_FCITX5_APP_NAME_RESOLVER_H_
#define MOZC_UNIX_FCITX5_APP_NAME_RESOLVER_H_

#include <cstddef>
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

// Resolves the effective window identifier for per-window profile memory.
// When the GNOME Shell extension service is active, returns |gnome_window_id|
// (or empty when no application window is focused). Otherwise falls back to
// |ic_uuid_id| (e.g. "ic:<uuid>") only when |allow_ic_fallback| is true.
std::string ResolveWindowId(bool gnome_service_available,
                            absl::string_view gnome_window_id,
                            bool allow_ic_fallback,
                            absl::string_view ic_uuid_id);

// Formats a Fcitx5 InputContext UUID into an "ic:<hex>" window identifier.
std::string FormatInputContextWindowId(const unsigned char* uuid_bytes,
                                       size_t uuid_len);

// Returns true if |im_name| identifies an active MoGRU / Mozc input method.
bool IsMoGRUActiveInputMethod(absl::string_view im_name);

}  // namespace fcitx

#endif  // MOZC_UNIX_FCITX5_APP_NAME_RESOLVER_H_
