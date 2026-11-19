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

#include "win32/tip/tip_thread_context.h"

#include <windows.h>

#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/strings/str_format.h"
#include "base/win32/wide_char.h"
#include "base/win32/win_util.h"
#include "config/gru_config.h"
#include "protocol/commands.pb.h"
#include "win32/tip/tip_input_mode_manager.h"

namespace mozc {
namespace win32 {
namespace tsf {

namespace {

TipInputModeManager::Config GetConfig() {
  TipInputModeManager::Config config;
  config.use_global_mode = WinUtil::IsPerUserInputSettingsEnabled();
  return config;
}

}  // namespace

TipThreadContext::TipThreadContext() : input_mode_manager_(GetConfig()) {}

void TipThreadContext::IncrementFocusRevision() {
  if (focus_revision_ < std::numeric_limits<int32_t>::max()) {
    focus_revision_++;
  } else {
    focus_revision_ = 0;
  }
}

// static
const std::string& TipThreadContext::GetHostApplicationName() {
  static const absl::NoDestructor<std::string> app_name([]() -> std::string {
    wchar_t module_path[4096] = {};
    const DWORD len =
        ::GetModuleFileNameW(nullptr, module_path, std::size(module_path));
    if (len == 0 || len >= std::size(module_path)) {
      return "";
    }
    const std::string utf8_path =
        WideToUtf8(std::wstring_view(module_path, len));
    return config::GruConfigManager::ExtractAppNameFromExecutablePath(
        utf8_path);
  }());
  return *app_name;
}

// static
void TipThreadContext::FillApplicationContext(commands::Context* context,
                                              uintptr_t window_handle_value) {
  if (context == nullptr) {
    return;
  }
  const std::string& app_name = GetHostApplicationName();
  if (!app_name.empty()) {
    context->set_application_name(app_name);
  }

  HWND hwnd = reinterpret_cast<HWND>(window_handle_value);
  if (hwnd == nullptr || !::IsWindow(hwnd)) {
    hwnd = ::GetFocus();
  }
  if (hwnd == nullptr || !::IsWindow(hwnd)) {
    hwnd = ::GetForegroundWindow();
  }
  HWND root_hwnd = nullptr;
  DWORD window_pid = 0;
  std::string current_win_id;
  if (hwnd != nullptr && ::IsWindow(hwnd)) {
    root_hwnd = ::GetAncestor(hwnd, GA_ROOTOWNER);
    if (root_hwnd == nullptr || !::IsWindow(root_hwnd)) {
      root_hwnd = hwnd;
    }
    ::GetWindowThreadProcessId(root_hwnd, &window_pid);
    if (window_pid == 0) {
      window_pid = ::GetCurrentProcessId();
    }
    current_win_id = absl::StrFormat(
        "win:0x%llx:%lu",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(root_hwnd)),
        static_cast<unsigned long>(window_pid));
    context->set_window_id(current_win_id);
  }

  struct SeenWindow {
    HWND hwnd;
    DWORD pid;
    std::string id;
  };
  static std::mutex* const seen_mutex = new std::mutex();
  static std::vector<SeenWindow>* const seen_windows =
      new std::vector<SeenWindow>();
  std::lock_guard<std::mutex> lock(*seen_mutex);
  bool found_current = false;
  for (auto it = seen_windows->begin(); it != seen_windows->end();) {
    DWORD alive_pid = 0;
    const bool alive =
        ::IsWindow(it->hwnd) &&
        ::GetWindowThreadProcessId(it->hwnd, &alive_pid) != 0 &&
        alive_pid == it->pid;
    if (!alive) {
      context->add_experimental_features("close_window:" + it->id);
      it = seen_windows->erase(it);
      continue;
    }
    if (root_hwnd != nullptr && it->hwnd == root_hwnd &&
        it->pid == window_pid) {
      found_current = true;
    }
    ++it;
  }
  if (root_hwnd != nullptr && !current_win_id.empty() && !found_current) {
    seen_windows->push_back(SeenWindow{root_hwnd, window_pid, current_win_id});
  }
}

}  // namespace tsf
}  // namespace win32
}  // namespace mozc
