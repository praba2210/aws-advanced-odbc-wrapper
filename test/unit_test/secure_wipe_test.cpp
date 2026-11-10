// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "../../driver/util/secure_wipe.h"

namespace {
    const std::string TOKEN = "SignedTokenStart" + std::string(600, '7') + "SignedTokenEnd";
}  // namespace

// SecureWipe zeroes the bytes it is given and no others.
TEST(SecureWipeTest, WipesTheGivenBytesOnly) {
    std::vector<unsigned char> bytes(16, 0x5A);
    SecureWipe(bytes.data() + 4, 8);
    EXPECT_EQ(std::vector<unsigned char>(4, 0x5A), std::vector<unsigned char>(bytes.begin(), bytes.begin() + 4));
    EXPECT_EQ(std::vector<unsigned char>(8, 0), std::vector<unsigned char>(bytes.begin() + 4, bytes.begin() + 12));
    EXPECT_EQ(std::vector<unsigned char>(4, 0x5A), std::vector<unsigned char>(bytes.begin() + 12, bytes.end()));
    SecureWipe(nullptr, 8);  // nothing to do
}

// ScopedWipe wipes a string's or a vector's elements when it goes out of scope, whatever their size, and reads the
// container's size then: a vector resized after the ScopedWipe is wiped whole, an empty one is left alone.
TEST(SecureWipeTest, ScopedWipeWipesOnScopeExit) {
    std::string text = std::string("UID=admin;PWD=") + TOKEN + std::string(64, 'x');  // on the heap
    std::vector<uint16_t> wide(text.begin(), text.end());
    std::vector<uint32_t> resized;
    std::vector<uint32_t> empty;
    {
        const ScopedWipe wipe_text(text);
        const ScopedWipe wipe_wide(wide);
        const ScopedWipe wipe_resized(resized);
        const ScopedWipe wipe_empty(empty);
        resized.assign(text.begin(), text.end());
        EXPECT_NE(std::string::npos, text.find(TOKEN));
    }
    EXPECT_EQ(std::string(text.size(), '\0'), text);
    EXPECT_EQ(std::vector<uint16_t>(wide.size(), 0), wide);
    ASSERT_EQ(text.size(), resized.size());
    EXPECT_EQ(std::vector<uint32_t>(resized.size(), 0), resized);
    EXPECT_TRUE(empty.empty());
}

// SecureWipeString zeroes everything the string's buffer holds up to its capacity, an earlier and longer value too, and
// leaves the string empty with its buffer in place.
TEST(SecureWipeTest, WipeString_ZeroesTheWholeBuffer) {
    constexpr size_t LONG_VALUE = 64;
    constexpr size_t SHORT_VALUE = 10;
    constexpr size_t SMALL_STRING_MAX = 15;
    std::string text(LONG_VALUE, 'A');
    text.assign(SHORT_VALUE, 'B');  // the bytes past the new end still hold the earlier value
    const void* const buffer = text.data();
    const size_t capacity = text.capacity();
    ASSERT_GT(capacity, SMALL_STRING_MAX);  // on the heap, not in the small-string buffer
    ASSERT_EQ('A', std::string_view(text.data(), capacity)[LONG_VALUE - 1]);

    SecureWipeString(text);

    EXPECT_TRUE(text.empty());
    EXPECT_TRUE(buffer == text.data());
    EXPECT_EQ(capacity, text.capacity());
    EXPECT_EQ(std::vector<char>(capacity, '\0'), std::vector<char>(text.data(), text.data() + capacity));
}

TEST(SecureWipeTest, WipeString_EmptyAndSmallStrings) {
    std::string empty;
    SecureWipeString(empty);
    EXPECT_TRUE(empty.empty());
    std::string small = "abc";
    SecureWipeString(small);
    EXPECT_TRUE(small.empty());
}
