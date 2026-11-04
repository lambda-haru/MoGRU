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

#include <sstream>
#include <string>
#include <vector>

#include "base/file_util.h"
#include "testing/gunit.h"
#include "testing/mozctest.h"

namespace mozc::dictionary {
namespace {

TEST(EnglishDictionaryTest, BasicAddAndLookup) {
  EnglishDictionary dict;
  EXPECT_TRUE(dict.empty());
  EXPECT_EQ(dict.size(), 0);

  dict.AddWord("merge");
  dict.AddWord("commit");
  dict.AddWord("repository");

  EXPECT_FALSE(dict.empty());
  EXPECT_EQ(dict.size(), 3);

  EXPECT_TRUE(dict.HasWord("merge"));
  EXPECT_TRUE(dict.HasWord("MERGE"));
  EXPECT_TRUE(dict.HasWord("Commit"));
  EXPECT_TRUE(dict.HasWord("repository"));
  EXPECT_FALSE(dict.HasWord("unknown"));

  // Longest prefix match
  EXPECT_EQ(dict.LongestPrefixMatch("mergesuru"), 5);
  EXPECT_EQ(dict.LongestPrefixMatch("repositorywotsukuru"), 10);
  EXPECT_EQ(dict.LongestPrefixMatch("commitshita"), 6);
  EXPECT_EQ(dict.LongestPrefixMatch("taberu"), 0);

  // min_len threshold
  EXPECT_EQ(dict.LongestPrefixMatch("mergesuru", 6), 0);  // "merge" is len 5 < 6
  EXPECT_EQ(dict.LongestPrefixMatch("mergesuru", 5), 5);
}

TEST(EnglishDictionaryTest, StreamLoad) {
  EnglishDictionary dict;
  std::istringstream iss(
      "# Comment line\n"
      "git\n"
      "push\n"
      "pull\n"
      "fetch\n");

  EXPECT_TRUE(dict.LoadFromStream(iss));
  EXPECT_EQ(dict.size(), 4);
  EXPECT_TRUE(dict.HasWord("git"));
  EXPECT_TRUE(dict.HasWord("push"));
  EXPECT_TRUE(dict.HasWord("pull"));
  EXPECT_TRUE(dict.HasWord("fetch"));
}

TEST(EnglishDictionaryTest, LoadFromString) {
  EnglishDictionary dict;
  constexpr absl::string_view data =
      "# Comment line\n"
      "apple\n"
      "banana\n"
      "\n"
      "cherry\n"
      "  date  \n";

  EXPECT_TRUE(dict.LoadFromString(data));
  EXPECT_EQ(dict.size(), 4);
  EXPECT_TRUE(dict.HasWord("apple"));
  EXPECT_TRUE(dict.HasWord("banana"));
  EXPECT_TRUE(dict.HasWord("cherry"));
  EXPECT_TRUE(dict.HasWord("date"));
}

TEST(EnglishDictionaryTest, Google10000EnglishFileLoad) {
  EnglishDictionary dict;
  const std::string dict_path =
      mozc::testing::GetSourcePath({"data", "english", "google-10000-english.txt"});

  ASSERT_TRUE(dict.LoadFromFile(dict_path));
  EXPECT_GE(dict.size(), 9900);

  // Typical programming words present in google-10000-english
  EXPECT_TRUE(dict.HasWord("merge"));
  EXPECT_TRUE(dict.HasWord("commit"));
  EXPECT_TRUE(dict.HasWord("repository"));
  EXPECT_TRUE(dict.HasWord("branch"));
  EXPECT_TRUE(dict.HasWord("status"));
  EXPECT_TRUE(dict.HasWord("build"));
  EXPECT_TRUE(dict.HasWord("test"));

  // Test longest prefix matching with Japanese suffixes
  EXPECT_EQ(dict.LongestPrefixMatch("mergesuru", 3), 5);  // "merge"
  EXPECT_EQ(dict.LongestPrefixMatch("repositorywotsukuru", 3), 10);  // "repository"
  EXPECT_EQ(dict.LongestPrefixMatch("commitshita", 3), 6);  // "commit"
  EXPECT_EQ(dict.LongestPrefixMatch("statuswo", 3), 6);  // "status"
  EXPECT_EQ(dict.LongestPrefixMatch("branchwo", 3), 6);  // "branch"

  // Short words should be filtered by min_len
  EXPECT_EQ(dict.LongestPrefixMatch("in", 3), 0);
  EXPECT_EQ(dict.LongestPrefixMatch("to", 3), 0);
}

}  // namespace
}  // namespace mozc::dictionary
