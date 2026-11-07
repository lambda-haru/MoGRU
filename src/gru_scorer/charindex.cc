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

#include <fstream>
#include <memory>
#include <string>
#include "gru_scorer/charindex.hh"

using namespace charindex;

bool CharIndex::LoadFile(const std::string path)
{
    constexpr size_t expectedSize = 65536 * sizeof(uint16_t) + 4096 * sizeof(Utf8Entry);

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    if (file.tellg() != expectedSize) return false;

    file.seekg(0, std::ios::beg);

    constexpr size_t num_uint32 = expectedSize / sizeof(uint32_t);
    buffer_ = std::make_unique_for_overwrite<uint32_t[]>(num_uint32);

    if (!file.read(reinterpret_cast<char*>(buffer_.get()), expectedSize))
    {
        buffer_.reset();
        return false;
    }

    auto* raw_bytes = reinterpret_cast<uint8_t*>(buffer_.get());
    auto* mutable_char2index = reinterpret_cast<uint16_t*>(raw_bytes);

    // Normalize Japanese punctuation marks:
    // Map fullwidth comma '，' (U+FF0C), halfwidth comma '､' (U+FF64), and ASCII comma ',' (U+002C) to '、' (U+3001)
    // Map fullwidth full stop '．' (U+FF0E), halfwidth full stop '｡' (U+FF61), and ASCII full stop '.' (U+002E) to '。' (U+3002)
    mutable_char2index[0xFF0C] = mutable_char2index[0x3001];
    mutable_char2index[0xFF64] = mutable_char2index[0x3001];
    mutable_char2index[0x002C] = mutable_char2index[0x3001];
    mutable_char2index[0xFF0E] = mutable_char2index[0x3002];
    mutable_char2index[0xFF61] = mutable_char2index[0x3002];
    mutable_char2index[0x002E] = mutable_char2index[0x3002];

    char2Index_ = mutable_char2index;
    index2Char_ = reinterpret_cast<const Utf8Entry*>(raw_bytes + 65536 * sizeof(uint16_t));

    return true;
}
