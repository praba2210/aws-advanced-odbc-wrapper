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

#include <gtest/gtest.h>

#include <string>

#include "scoped_ctype_locale.h"

#include "../../driver/util/rds_strings.h"

class RdsStringsTest : public testing::Test {};

TEST_F(RdsStringsTest, AsciiLower_FoldsOnlyAsciiLetters) {
    EXPECT_EQ("abc-xyz_019.on.aws", AsciiLower("AbC-XyZ_019.ON.AWS"));
    EXPECT_EQ("", AsciiLower(""));
    // UTF-8 bytes stay as they are: U+0130 (capital I with dot) and U+00DC (capital U with diaeresis).
    EXPECT_EQ("\xC4\xB0\xC3\x9C", AsciiLower("\xC4\xB0\xC3\x9C"));
}

// Under tr_TR.UTF-8, glibc's std::tolower leaves I as I (under tr_TR.ISO-8859-9 it maps it to 0xFD). Needs the
// tr_TR.UTF-8 locale.
TEST_F(RdsStringsTest, AsciiLower_TurkishCtype_StillFoldsI) {
    const ScopedCtypeLocale ctype("tr_TR.UTF-8");
    if (!ctype.IsSet()) {
        SKIP_OR_FAIL_WITHOUT_TURKISH_LOCALE();
    }
    EXPECT_EQ("il-central-1", AsciiLower("IL-CENTRAL-1"));
}

#ifdef UNICODE

TEST_F(RdsStringsTest, SimpleConversion) {
    SQLTCHAR in[] = {97, 0, 98, 0, 0, 0};
    SQLTCHAR out[3] = {0};

    Convert4To2ByteString(true, in, out, 3);

    EXPECT_EQ(out[0], 97);
    EXPECT_EQ(out[1], 98);
    EXPECT_EQ(out[2], 0);
}

TEST_F(RdsStringsTest, SurrogatePairConversion) {
    // Rocket emoji as utf-32: 0x0001F680
    // Each UChar32 occupies 2 SQLTCHAR elements (little-endian layout).
    // We need the output buffer large enough for the surrogate pair + null.
    SQLTCHAR in[] = {0xF680, 0x0001, 0x0000, 0x0000};
    SQLTCHAR out[4] = {0};

    Convert4To2ByteString(true, in, out, 4);

    // Surrogate pair: high=0xD83D, low=0xDE80
    EXPECT_EQ(out[0], 0xD83D);
    EXPECT_EQ(out[1], 0xDE80);
    EXPECT_EQ(out[2], 0x0000);
}

TEST_F(RdsStringsTest, ConvertInput) {
    SQLTCHAR in[] = {97, 0, 98, 0, 0, 0};

    Convert4To2ByteString(true, in, nullptr, 3);

    EXPECT_EQ(in[0], 97);
    EXPECT_EQ(in[1], 98);
    EXPECT_EQ(in[2], 0);
}

TEST_F(RdsStringsTest, SurrogatePairTruncatedWhenBufferTooSmall) {
    // Rocket emoji U+1F680 as UTF-32 in 2 SQLTCHAR elements (little-endian)
    // followed by 'a' (U+0061) as UTF-32 and null terminator.
    SQLTCHAR in[] = {0xF680, 0x0001, 0x0061, 0x0000, 0x0000, 0x0000};
    // Output buffer only has room for 2 code units + null.
    // The surrogate pair needs 2 code units, so it should fit, but 'a' won't.
    SQLTCHAR out[3] = {0};

    Convert4To2ByteString(true, in, out, 3);

    // Surrogate pair fits exactly in 2 slots
    EXPECT_EQ(out[0], 0xD83D);
    EXPECT_EQ(out[1], 0xDE80);
    EXPECT_EQ(out[2], 0x0000);
}

TEST_F(RdsStringsTest, UnpairedSurrogateDroppedWhenTruncated) {
    // Two supplementary characters: U+1F680 and U+1F4A9
    // Each needs a surrogate pair (2 UTF-16 code units).
    // With output_size=3, ICU would write 4 code units but we clamp to 3,
    // leaving an unpaired high surrogate. The fix should drop it back to 2.
    SQLTCHAR in[] = {0xF680, 0x0001, 0xF4A9, 0x0001, 0x0000, 0x0000};
    SQLTCHAR out[4] = {0};

    Convert4To2ByteString(true, in, out, 4);

    // Only the first complete surrogate pair should be in the output.
    // The second pair's high surrogate should be dropped to avoid corruption.
    EXPECT_EQ(out[0], 0xD83D);
    EXPECT_EQ(out[1], 0xDE80);
    EXPECT_EQ(out[2], 0x0000);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_4Byte_NTS) {
    // "ab" as UChar32: each codepoint takes 2 SQLTCHAR elements
    SQLTCHAR in[] = {97, 0, 98, 0, 0, 0};
    size_t len = GetLenOfSqltcharArray(in, SQL_NTS, true);
    EXPECT_EQ(len, 3u);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_4Byte_NTS_WithSupplementary) {
    // U+1F680 (rocket) as UChar32 in 2 SQLTCHAR elements then null
    SQLTCHAR in[] = {0xF680, 0x0001, 0x0000, 0x0000};
    size_t len = GetLenOfSqltcharArray(in, SQL_NTS, true);
    EXPECT_EQ(len, 3u);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_4Byte_NTS_MixedBmpAndSupplementary) {
    // 'a' (U+0061) as UChar32, then U+1F680 (rocket) as UChar32, then null
    SQLTCHAR in[] = {0x0061, 0x0000, 0xF680, 0x0001, 0x0000, 0x0000};
    size_t len = GetLenOfSqltcharArray(in, SQL_NTS, true);
    EXPECT_EQ(len, 4u);
}

TEST_F(RdsStringsTest, NullIn) {
    SQLTCHAR out[4] = {97, 97, 97, 97};
    Convert4To2ByteString(true, nullptr, out, 4);
    EXPECT_EQ(out[0], 97);
}

TEST_F(RdsStringsTest, Convert2ByteChars) {
    SQLTCHAR in[] = {97, 98, 99, 0};
    SQLTCHAR out[4] = {0};

    Convert4To2ByteString(false, in, out, 4);

    EXPECT_EQ(out[0], 97);
    EXPECT_EQ(out[1], 98);
    EXPECT_EQ(out[2], 99);
    EXPECT_EQ(out[3], 0);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_2Byte_NTS) {
    SQLTCHAR in[] = {97, 98, 99, 0};
    size_t len = GetLenOfSqltcharArray(in, SQL_NTS, false);
    // Should include null terminator: 3 chars + 1 null = 4
    EXPECT_EQ(len, 4u);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_ExplicitLength) {
    SQLTCHAR in[] = {97, 98, 99, 0};
    size_t len = GetLenOfSqltcharArray(in, 10, false);
    EXPECT_EQ(len, 11u);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_ZeroLength) {
    SQLTCHAR in[] = {97, 0};
    size_t len = GetLenOfSqltcharArray(in, 0, false);
    EXPECT_EQ(len, 0u);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_4Byte_ExplicitLength_BMP) {
    // "abc" as UChar32: 3 codepoints, each BMP, so UTF-16 length = 3 + 1 null = 4
    SQLTCHAR in[] = {0x0061, 0x0000, 0x0062, 0x0000, 0x0063, 0x0000};
    size_t len = GetLenOfSqltcharArray(in, 3, true);
    EXPECT_EQ(len, 4u);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_4Byte_ExplicitLength_Supplementary) {
    // U+1F680 (rocket) as UChar32: 1 codepoint, needs surrogate pair = 2 UTF-16 units + 1 null = 3
    SQLTCHAR in[] = {0xF680, 0x0001, 0x0000, 0x0000};
    size_t len = GetLenOfSqltcharArray(in, 1, true);
    EXPECT_EQ(len, 3u);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_4Byte_ExplicitLength_Mixed) {
    // 'a' (U+0061) then U+1F680: 2 codepoints, UTF-16 = 1 + 2 = 3 units + 1 null = 4
    SQLTCHAR in[] = {0x0061, 0x0000, 0xF680, 0x0001, 0x0000, 0x0000};
    size_t len = GetLenOfSqltcharArray(in, 2, true);
    EXPECT_EQ(len, 4u);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_4Byte_NTS_HighByteZero) {
    SQLTCHAR in[] = {0x00AB, 0x0000, 0x0000, 0x0000};
    size_t len = GetLenOfSqltcharArray(in, SQL_NTS, true);
    EXPECT_EQ(len, 2u);
}

TEST_F(RdsStringsTest, GetLenOfSqltcharArray_4Byte_NTS_MultipleHighByteZero) {
    SQLTCHAR in[] = {
        0x00AB, 0x0000,  // U+00AB as UChar32
        0x00FF, 0x0000,  // U+00FF as UChar32
        0x0041, 0x0000,  // U+0041 ('A') as UChar32
        0x0000, 0x0000   // null terminator as UChar32
    };
    size_t len = GetLenOfSqltcharArray(in, SQL_NTS, true);
    EXPECT_EQ(len, 4u);
}

// The length is in the app's characters: "café" is 5 UTF-8 bytes, 4 UTF-16 units and 4 code points; "a😀" is 5 bytes,
// 3 units and 2 code points.
TEST_F(RdsStringsTest, CopyUTF8ToAppChars_CountsAppCharacters) {
    EXPECT_EQ(4u, CopyUTF8ToAppChars("caf\xC3\xA9", nullptr, 0, false));
    EXPECT_EQ(4u, CopyUTF8ToAppChars("caf\xC3\xA9", nullptr, 0, true));
    EXPECT_EQ(3u, CopyUTF8ToAppChars("a\xF0\x9F\x98\x80", nullptr, 0, false));
    EXPECT_EQ(2u, CopyUTF8ToAppChars("a\xF0\x9F\x98\x80", nullptr, 0, true));
    EXPECT_EQ(0u, CopyUTF8ToAppChars("", nullptr, 0, false));

    uint16_t utf16[5] = {0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A};
    EXPECT_EQ(4u, CopyUTF8ToAppChars("caf\xC3\xA9", utf16, 5, false));
    EXPECT_EQ(0x00E9, utf16[3]);
    EXPECT_EQ(0, utf16[4]);
}

// A text that doesn't fit, or fits only without its terminator, is cut to a terminated prefix; the full length is
// returned either way.
TEST_F(RdsStringsTest, CopyUTF8ToAppChars_CutsToATerminatedPrefix) {
    const std::vector<uint32_t> abc = {'a', 'b', 'c', 0};
    for (const bool user_4_byte : {false, true}) {
        // The buffer's 4 characters, read in the app's width.
        uint32_t buf[4];
        const auto chars = [&buf, user_4_byte]() {
            std::vector<uint32_t> out;
            for (size_t i = 0; i < 4; ++i) {
                out.push_back(user_4_byte ? buf[i] : reinterpret_cast<const uint16_t*>(buf)[i]);
            }
            return out;
        };

        std::fill(buf, buf + 4, 0x5A5A5A5Au);
        EXPECT_EQ(5u, CopyUTF8ToAppChars("abcde", buf, 4, user_4_byte));
        EXPECT_EQ(abc, chars());

        // Room for every character but the terminator.
        std::fill(buf, buf + 4, 0x5A5A5A5Au);
        EXPECT_EQ(4u, CopyUTF8ToAppChars("abcd", buf, 4, user_4_byte));
        EXPECT_EQ(abc, chars());

        std::fill(buf, buf + 4, 0x5A5A5A5Au);
        EXPECT_EQ(3u, CopyUTF8ToAppChars("abc", buf, 4, user_4_byte));
        EXPECT_EQ(abc, chars());
    }
}

// A cut doesn't end in the first half of a surrogate pair: "ab😀" in 4 UTF-16 units leaves "ab".
TEST_F(RdsStringsTest, CopyUTF8ToAppChars_KeepsSurrogatePairsWhole) {
    uint16_t buf[4] = {0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A};
    EXPECT_EQ(4u, CopyUTF8ToAppChars("ab\xF0\x9F\x98\x80", buf, 4, false));
    EXPECT_EQ('a', buf[0]);
    EXPECT_EQ('b', buf[1]);
    EXPECT_EQ(0, buf[2]);

    uint16_t roomy[5] = {0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A, 0x5A5A};
    EXPECT_EQ(4u, CopyUTF8ToAppChars("ab\xF0\x9F\x98\x80", roomy, 5, false));
    EXPECT_EQ(0xD83D, roomy[2]);
    EXPECT_EQ(0xDE00, roomy[3]);
    EXPECT_EQ(0, roomy[4]);
}

// An app with a 4-byte SQLWCHAR gets one code point per character, a supplementary one too.
TEST_F(RdsStringsTest, CopyUTF8ToAppChars_FourByteWritesCodePoints) {
    uint32_t buf[3] = {0x5A5A5A5A, 0x5A5A5A5A, 0x5A5A5A5A};
    EXPECT_EQ(2u, CopyUTF8ToAppChars("a\xF0\x9F\x98\x80", buf, 3, true));
    EXPECT_EQ(0x61u, buf[0]);
    EXPECT_EQ(0x1F600u, buf[1]);
    EXPECT_EQ(0u, buf[2]);

    uint32_t tight[2] = {0x5A5A5A5A, 0x5A5A5A5A};
    EXPECT_EQ(2u, CopyUTF8ToAppChars("a\xF0\x9F\x98\x80", tight, 2, true));
    EXPECT_EQ(0x61u, tight[0]);
    EXPECT_EQ(0u, tight[1]);
}

TEST_F(RdsStringsTest, CopyUTF8ToAppChars_ZeroCapacityWritesNothing) {
    uint32_t buf[2] = {0x5A5A5A5A, 0x5A5A5A5A};
    EXPECT_EQ(3u, CopyUTF8ToAppChars("abc", buf, 0, false));
    EXPECT_EQ(3u, CopyUTF8ToAppChars("abc", buf, 0, true));
    EXPECT_EQ(0x5A5A5A5Au, buf[0]);
    EXPECT_EQ(0x5A5A5A5Au, buf[1]);
}

// ODBC W functions take a length in characters, and the text needn't be NUL-terminated. pyodbc, for example, passes
// a UTF-16 buffer that ends in a single NUL byte, so reading to a 16-bit NUL picks up whatever follows.
TEST_F(RdsStringsTest, ConvertUserAppToUTF8_2Byte_ExplicitLength_StopsAtLength) {
    SQLTCHAR in[] = {'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0x0400, 0};
    EXPECT_EQ("SELECT 1", ConvertUserAppToUTF8(false, in, 8));
}

TEST_F(RdsStringsTest, ConvertUserAppToUTF8_2Byte_ExplicitLength_StopsAtEmbeddedNul) {
    SQLTCHAR in[] = {'a', 'b', 0, 'x', 'y', 0};
    EXPECT_EQ("ab", ConvertUserAppToUTF8(false, in, 5));
}

TEST_F(RdsStringsTest, ConvertUserAppToUTF8_2Byte_ZeroLength_Empty) {
    SQLTCHAR in[] = {'a', 'b', 0};
    EXPECT_EQ("", ConvertUserAppToUTF8(false, in, 0));
}

TEST_F(RdsStringsTest, ConvertUserAppToUTF8_2Byte_Nts_ReadsToNul) {
    SQLTCHAR in[] = {'a', 'b', 0, 'x', 0};
    EXPECT_EQ("ab", ConvertUserAppToUTF8(false, in, SQL_NTS));
}

// An app with a 4-byte SQLWCHAR (iODBC) gives the length in code points, and the text needn't end in a NUL. Each
// buffer below holds the text, then a sentinel right after TextLength characters that the conversion mustn't reach.
TEST_F(RdsStringsTest, ConvertUserAppToUTF8_4Byte_ExplicitLength_StopsAtLength) {
    char32_t in[] = {U'C', U'O', U'M', U'M', U'I', U'T', U'X'};
    EXPECT_EQ("COMMIT", ConvertUserAppToUTF8(true, reinterpret_cast<SQLTCHAR*>(in), 6));
}

TEST_F(RdsStringsTest, ConvertUserAppToUTF8_4Byte_ExplicitLength_CountsCodePointsOutsideTheBmp) {
    char32_t in[] = {U'\U0001F600', U'\U0001F600', U'a', U'b', U'c'};
    EXPECT_EQ("\xF0\x9F\x98\x80\xF0\x9F\x98\x80", ConvertUserAppToUTF8(true, reinterpret_cast<SQLTCHAR*>(in), 2));
}

TEST_F(RdsStringsTest, ConvertUserAppToUTF8_4Byte_ZeroLength_Empty) {
    char32_t in[] = {U'a', U'b', 0};
    EXPECT_EQ("", ConvertUserAppToUTF8(true, reinterpret_cast<SQLTCHAR*>(in), 0));
}

TEST_F(RdsStringsTest, ConvertUserAppToUTF8_4Byte_ExplicitLength_StopsAtEmbeddedNul) {
    char32_t in[] = {U'a', U'b', 0, U'x', U'y'};
    EXPECT_EQ("ab", ConvertUserAppToUTF8(true, reinterpret_cast<SQLTCHAR*>(in), 5));
}

TEST_F(RdsStringsTest, ConvertUserAppToUTF8_4Byte_Nts_ReadsToNul) {
    char32_t in[] = {U'a', U'b', 0, U'x', 0};
    EXPECT_EQ("ab", ConvertUserAppToUTF8(true, reinterpret_cast<SQLTCHAR*>(in), SQL_NTS));
}

// What a 2-byte base driver gets for SQLPrepareW and the catalog functions.
TEST_F(RdsStringsTest, ConvertUserAppInputToBaseDriver_4ByteApp_ExplicitLength_StopsAtLength) {
    char32_t in[] = {U'C', U'O', U'M', U'M', U'I', U'T', U'X'};
    const std::vector<SQLTCHAR> out = ConvertUserAppInputToBaseDriver(true, false, reinterpret_cast<SQLTCHAR*>(in), 6);
    EXPECT_EQ((std::vector<SQLTCHAR>{'C', 'O', 'M', 'M', 'I', 'T', 0}), out);
}

// A 4-byte base driver gets all TextLength code points of a 4-byte app's text, also outside the BMP.
TEST_F(RdsStringsTest, ConvertUserAppInputToBaseDriver_4ByteAppAndDriver_KeepsTextOutsideTheBmp) {
    char32_t in[] = {U'\U0001F600', U'\U0001F600', U'a', U'b', U'c'};
    std::vector<SQLTCHAR> out = ConvertUserAppInputToBaseDriver(true, true, reinterpret_cast<SQLTCHAR*>(in), 2);
    EXPECT_EQ("\xF0\x9F\x98\x80\xF0\x9F\x98\x80", Convert4ByteSqlWChar(out.data(), SQL_NTS));
}

#endif // UNICODE
