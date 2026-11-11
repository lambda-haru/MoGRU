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

#ifndef MOZC_DICTIONARY_ENGLISH_DICTIONARY_H_
#define MOZC_DICTIONARY_ENGLISH_DICTIONARY_H_

#include <cstddef>
#include <istream>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "base/container/trie.h"

namespace mozc::dictionary {

// A dictionary class that manages English words using Trie for fast
// prefix search and longest match.
class EnglishDictionary {
 public:
  EnglishDictionary() = default;
  ~EnglishDictionary() = default;

  EnglishDictionary(EnglishDictionary&&) noexcept = default;
  EnglishDictionary& operator=(EnglishDictionary&&) noexcept = default;
  EnglishDictionary(const EnglishDictionary&) = delete;
  EnglishDictionary& operator=(const EnglishDictionary&) = delete;

  // Loads words from an input stream (one word per line).
  bool LoadFromStream(std::istream &is);

  // Loads words from an in-memory string/string_view (one word per line).
  bool LoadFromString(absl::string_view data);

  // Loads words from a file path.
  bool LoadFromFile(absl::string_view filepath);

  // Adds a single English word. Automatically converted to lower case in trie,
  // while storing original casing as value.
  void AddWord(absl::string_view word);

  // Clears all entries from the dictionary.
  void Clear();

  // Returns true if the dictionary has no words.
  bool empty() const { return word_count_ == 0; }

  // Returns the number of registered words.
  size_t size() const { return word_count_; }

  // Returns true if `word` exists in the dictionary.
  bool HasWord(absl::string_view word) const;

  // Finds the longest matching English word prefix in `text`.
  // Returns the length of the matched prefix.
  // If `matched_original_word` is non-null, sets it to the original word value stored in the trie.
  // If no word matches, or the matched length is less than `min_len`, returns 0.
  // Example: text="mergesuru", min_len=3 -> returns 5 ("merge").
  size_t LongestPrefixMatch(absl::string_view text, size_t min_len = 1,
                            std::string* matched_original_word = nullptr) const;

  // Returns up to `limit` words that start with `prefix`.
  std::vector<std::string> PredictWords(absl::string_view prefix,
                                        size_t limit = 10) const;

 private:
  mozc::Trie<std::string> trie_;
  size_t word_count_ = 0;
};

}  // namespace mozc::dictionary

#endif  // MOZC_DICTIONARY_ENGLISH_DICTIONARY_H_
