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

#ifndef MOZC_REWRITER_ENGLISH_FALLBACK_REWRITER_H_
#define MOZC_REWRITER_ENGLISH_FALLBACK_REWRITER_H_

#include <cstdint>
#include <string>

#include "absl/strings/string_view.h"
#include "config/gru_config.h"
#include "converter/segments.h"
#include "dictionary/english_dictionary.h"
#include "dictionary/pos_matcher.h"
#include "request/conversion_request.h"
#include "rewriter/rewriter_interface.h"

namespace mozc {

// A rewriter that detects English words from raw input when abnormal high-cost
// segments occur during Romaji input (e.g. "めrげする" -> "mergesuru"), and
// merges them into a clean English candidate (e.g. "merge" + "する").
class EnglishFallbackRewriter : public RewriterInterface {
 public:
  explicit EnglishFallbackRewriter(const dictionary::PosMatcher& pos_matcher);

  // Constructor for testing with an injected dictionary.
  EnglishFallbackRewriter(const dictionary::PosMatcher& pos_matcher,
                          dictionary::EnglishDictionary dictionary);

  ~EnglishFallbackRewriter() override = default;

  int capability(const ConversionRequest& request) const override;

  bool Rewrite(const ConversionRequest& request,
               Segments* segments) const override;

 private:
  void AddEnglishCandidates(absl::string_view word, absl::string_view key,
                            Segment* segment) const;

  uint16_t unknown_id_ = 0;
  dictionary::EnglishDictionary dictionary_;
};

}  // namespace mozc

#endif  // MOZC_REWRITER_ENGLISH_FALLBACK_REWRITER_H_
