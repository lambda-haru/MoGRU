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

#include "dictionary/english_dictionary.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <istream>
#include <string>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/string_view.h"
#include "base/util.h"

namespace mozc::dictionary {

void EnglishDictionary::AddWord(absl::string_view word) {
  std::string lower = std::string(absl::StripAsciiWhitespace(word));
  if (lower.empty()) {
    return;
  }
  // Verify that all characters are alphabetical or valid punctuation (hyphen/apostrophe).
  for (char c : lower) {
    if (!std::isalpha(static_cast<unsigned char>(c)) && c != '-' && c != '\'') {
      return;
    }
  }
  Util::LowerString(&lower);
  std::string value = lower;
  trie_.AddEntry(lower, std::move(value));
  ++word_count_;
}

bool EnglishDictionary::LoadFromStream(std::istream &is) {
  std::string line;
  size_t count = 0;
  while (std::getline(is, line)) {
    absl::string_view trimmed = absl::StripAsciiWhitespace(line);
    if (!trimmed.empty() && !trimmed.starts_with('#')) {
      AddWord(trimmed);
      ++count;
    }
  }
  return count > 0;
}

bool EnglishDictionary::LoadFromFile(absl::string_view filepath) {
  std::ifstream ifs(std::string(filepath).c_str());
  if (!ifs.is_open()) {
    LOG(ERROR) << "Failed to open English dictionary file: " << filepath;
    return false;
  }
  return LoadFromStream(ifs);
}

bool EnglishDictionary::HasWord(absl::string_view word) const {
  std::string lower(word);
  Util::LowerString(&lower);
  std::string dummy;
  return trie_.LookUp(lower, &dummy);
}

size_t EnglishDictionary::LongestPrefixMatch(absl::string_view text,
                                            size_t min_len) const {
  if (text.size() < min_len) {
    return 0;
  }
  std::string lower(text);
  Util::LowerString(&lower);
  std::string matched_value;
  size_t matched_len = 0;
  if (trie_.LongestMatch(lower, &matched_value, &matched_len)) {
    if (matched_len >= min_len) {
      return matched_len;
    }
  }
  return 0;
}

std::vector<std::string> EnglishDictionary::PredictWords(
    absl::string_view prefix, size_t limit) const {
  std::string lower(prefix);
  Util::LowerString(&lower);
  std::vector<std::string> all_words;
  trie_.LookUpPredictiveAll(lower, &all_words);
  if (all_words.size() > limit) {
    all_words.resize(limit);
  }
  return all_words;
}

}  // namespace mozc::dictionary
