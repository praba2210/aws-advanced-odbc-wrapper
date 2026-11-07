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

#ifndef RDS_STRINGS_H_
#define RDS_STRINGS_H_

#include "windows_headers.h"

#ifdef WIN32
    #include <tchar.h>
#else // Unix Platforms
    #define TEXT(x) x
#endif // WIN32

#include <sql.h>

#include <algorithm>
#include <regex>
#include <sstream>
#include <string.h>
#include <string_view>
#include <vector>

#include "unicode/ucasemap.h"
#include "unicode/utypes.h"

#include "logger_wrapper.h"

static constexpr uint32_t SQLTCHAR_HALF_BITS = 16;

inline size_t GetLenOfSqltcharArray(SQLTCHAR *in, SQLLEN buffer_len, bool use_4_bytes) {
    if (buffer_len > 0) {
        if (!use_4_bytes || in == nullptr) {
            return static_cast<size_t>(buffer_len) + 1;
        }

        const int32_t num_codepoints = static_cast<int32_t>(buffer_len);
        std::vector<UChar32> utf32_buf(num_codepoints);
        for (int32_t i = 0; i < num_codepoints; i++) {
            const ptrdiff_t offset = static_cast<ptrdiff_t>(i) * 2;
            utf32_buf[i] = static_cast<UChar32>(static_cast<uint32_t>(in[offset])
                         | (static_cast<uint32_t>(in[offset + 1]) << SQLTCHAR_HALF_BITS));
        }

        UErrorCode err = U_ZERO_ERROR;
        int32_t utf16_len = 0;
        u_strFromUTF32(nullptr, 0, &utf16_len, utf32_buf.data(), num_codepoints, &err);
        if (err != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(err)) {
            LOG(ERROR) << "ICU preflight conversion failed: " << u_errorName(err);
            return (static_cast<size_t>(buffer_len) * 2) + 1;
        }

        return static_cast<size_t>(utf16_len) + 1;
    }

    if (buffer_len == SQL_NTS) {
        if (in == nullptr) {
            return 0;
        }
        if (!use_4_bytes) {
            return u_strlen(reinterpret_cast<const UChar *>(in)) + 1;
        }

        std::vector<UChar32> utf32_buf;
        size_t num_codepoints = 0;
        while (true) {
            const UChar32 cp = static_cast<UChar32>(static_cast<uint32_t>(in[num_codepoints * 2])
                       | (static_cast<uint32_t>(in[(num_codepoints * 2) + 1]) << SQLTCHAR_HALF_BITS));
            if (cp == 0) {
                break;
            }
            utf32_buf.push_back(cp);
            num_codepoints++;
        }

        UErrorCode err = U_ZERO_ERROR;
        int32_t utf16_len = 0;
        u_strFromUTF32(nullptr, 0, &utf16_len, utf32_buf.data(), static_cast<int32_t>(num_codepoints), &err);
        if (err != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(err)) {
            LOG(ERROR) << "ICU preflight conversion failed: " << u_errorName(err);
            return (num_codepoints * 2) + 1;
        }

        return static_cast<size_t>(utf16_len) + 1;
    }

    return 0;
}

#ifdef UNICODE
#include "unicode/unistr.h"
inline size_t UShortStrlen(const uint16_t* str, const bool use_4_byte = false) {
    size_t length = 0;
    if (!str) {
        return length;
    }

    if (use_4_byte) {
        while (str[length * 2] != 0 || str[(length * 2) + 1] != 0) {
            length++;
        }
    } else {
        while (str[length] != 0) {
            length++;
        }
    }
    return length;
}

inline std::wstring ConvertUTF8ToWString(std::string input) {
    const icu::StringPiece string_piece(input.c_str(), static_cast<int32_t>(input.length()));
    const icu::UnicodeString string_utf16 = icu::UnicodeString::fromUTF8(string_piece);

    int32_t size;
    UErrorCode error = U_ZERO_ERROR;
    u_strToWCS(nullptr, 0, &size, string_utf16.getBuffer(), string_utf16.length(), &error);

    error = U_ZERO_ERROR; // Reset error
    std::wstring wstr(size, 0);
    u_strToWCS(wstr.data(), static_cast<int32_t>(wstr.size()), nullptr, string_utf16.getBuffer(), string_utf16.length(), &error);

    if (U_FAILURE(error)) {
        LOG(ERROR) << "ConvertUTF8ToWString conversion failed: " << u_errorName(error);
        return {};
    }

    return wstr;
}

inline std::vector<uint16_t> ConvertUTF8ToUTF16(std::string input) {
    const icu::StringPiece string_piece(input.c_str(), static_cast<int32_t>(input.length()));
    icu::UnicodeString string_utf16 = icu::UnicodeString::fromUTF8(string_piece);
    uint16_t *ushort_string = reinterpret_cast<uint16_t*>(const_cast<char16_t*>(string_utf16.getTerminatedBuffer()));
    const size_t size = UShortStrlen(ushort_string);
    std::vector<uint16_t> ushort_vec(ushort_string, ushort_string + size);
    // Insert null terminator because vector.data() returns NULL when empty
    ushort_vec.push_back(0);
    return ushort_vec;
}

// Assumes that the passed in vec is null terminated and was produced by ConvertUTF8ToUTF16
inline int CopyUTF16StringToBuffer(uint16_t* buf, size_t buf_len, std::vector<uint16_t> vec) {
    const int32_t str_len = static_cast<int32_t>(vec.empty() ? 0 : vec.size() - 1);
    if (buf_len == 0) {
        return str_len;
    }
    const icu::UnicodeString ustr(reinterpret_cast<const char16_t*>(vec.data()), str_len);
    UErrorCode err = U_ZERO_ERROR;
    ustr.extract(reinterpret_cast<char16_t*>(buf), static_cast<int32_t>(buf_len), err);
    if (U_FAILURE(err)) {
        buf[buf_len - 1] = 0;
    }
    return str_len;
}

inline int CopyUTF8ToUTF16Buffer(uint16_t* buf, size_t buf_len, std::string str) {
    return CopyUTF16StringToBuffer(buf, buf_len, ConvertUTF8ToUTF16(str));
}

// Writes UTF-8 text into an app's buffer of buffer_chars characters, in the app's characters: UTF-16 units, or code
// points for an app with a 4-byte SQLWCHAR. Returns the text's full length in those characters, which is what a W
// function reports. A text that doesn't fit is cut to the longest prefix that leaves room for the terminator and
// doesn't end in the first half of a surrogate pair. Writes nothing when out is null or buffer_chars is 0.
inline size_t CopyUTF8ToAppChars(std::string_view utf8, void* out, size_t buffer_chars, bool user_4_byte) {
    const icu::UnicodeString text =
        icu::UnicodeString::fromUTF8(icu::StringPiece(utf8.data(), static_cast<int32_t>(utf8.size())));
    const auto full = static_cast<size_t>(user_4_byte ? text.countChar32() : text.length());
    if (out == nullptr || buffer_chars == 0) {
        return full;
    }
    const size_t room = std::min(full, buffer_chars - 1);
    if (user_4_byte) {
        auto* const dst = static_cast<UChar32*>(out);
        int32_t index = 0;
        for (size_t n = 0; n < room; ++n) {
            dst[n] = text.char32At(index);
            index = text.moveIndex32(index, 1);
        }
        dst[room] = 0;
    } else {
        size_t copied = room;
        if (copied > 0 && copied < full && U16_IS_LEAD(text.charAt(static_cast<int32_t>(copied - 1)))) {
            copied--;
        }
        auto* const dst = static_cast<char16_t*>(out);
        text.extract(0, static_cast<int32_t>(copied), dst);
        dst[copied] = 0;
    }
    return full;
}

// The input string buffer is assumed to be null terminated
inline std::string ConvertUTF16ToUTF8(uint16_t *buffer_utf16) {
    const icu::UnicodeString unicode_str(reinterpret_cast<const char16_t*>(buffer_utf16));
    std::string buffer_utf8;
    unicode_str.toUTF8String(buffer_utf8);
    return buffer_utf8;
}

// Expand UTF16 (2-byte) into UTF32 (4-byte)
inline size_t ConvertUTF16ToUTF32(const SQLTCHAR* src, SQLTCHAR* dst, const size_t src_len, const size_t dst_len) {
    if (src == nullptr || dst == nullptr || dst_len < 2) {
        return 0;
    }

    const int32_t capacity = static_cast<int32_t>((dst_len - 2) / 2);
    const icu::UnicodeString ustr(reinterpret_cast<const char16_t*>(src), static_cast<int32_t>(src_len));
    UErrorCode err = U_ZERO_ERROR;
    const int32_t written = ustr.toUTF32(reinterpret_cast<UChar32*>(dst), capacity, err);
    const bool conversion_ok = U_SUCCESS(err) != 0 || err == U_BUFFER_OVERFLOW_ERROR;
    const int32_t actual = conversion_ok ? std::min(written, capacity) : 0;
    reinterpret_cast<UChar32*>(dst)[actual] = 0;
    return static_cast<size_t>(actual);
}

inline void ExpandUTF16ToUTF32InPlace(SQLTCHAR* buf, size_t src_chars, size_t buf_slots) {
    if (buf == nullptr || src_chars == 0 || buf_slots < 2) {
        return;
    }
    const int32_t capacity = static_cast<int32_t>((buf_slots - 2) / 2);
    // UnicodeString copies the source data internally, so writing to buf is safe
    const icu::UnicodeString ustr(reinterpret_cast<const char16_t*>(buf), static_cast<int32_t>(src_chars));
    UErrorCode err = U_ZERO_ERROR;
    const int32_t written = ustr.toUTF32(reinterpret_cast<UChar32*>(buf), capacity, err);
    const bool conversion_ok = U_SUCCESS(err) != 0 || err == U_BUFFER_OVERFLOW_ERROR;
    const int32_t actual = conversion_ok ? std::min(written, capacity) : 0;
    reinterpret_cast<UChar32*>(buf)[actual] = 0;
}

// A 4-byte base driver's UTF-32 text in buf to UTF-16, in place: at most max_chars code points, to the first NUL, and
// as many of them whole as buf_slots SQLTCHARs hold with a NUL. A code point takes the two SQLTCHARs of its UTF-32 or
// fewer, so all of them fit unless the text has no NUL in buf. An invalid code point becomes U+FFFD. Returns the
// UTF-16 units written.
inline size_t ConvertUTF32ToUTF16InPlace(SQLTCHAR* buf, size_t max_chars, size_t buf_slots) {
    if (buf == nullptr || buf_slots == 0) {
        return 0;
    }
    constexpr uint32_t MAX_CODE_POINT = 0x10FFFF;
    constexpr uint32_t REPLACEMENT_CHARACTER = 0xFFFD;
    const size_t chars = std::min(max_chars, buf_slots / 2);
    std::vector<SQLTCHAR> utf16;
    utf16.reserve(chars * 2);
    for (size_t i = 0; i < chars; ++i) {
        uint32_t code_point = 0;
        std::memcpy(&code_point, buf + (i * 2), sizeof(code_point));
        if (code_point == 0) {
            break;
        }
        if (code_point > MAX_CODE_POINT || U_IS_SURROGATE(code_point)) {
            code_point = REPLACEMENT_CHARACTER;
        }
        const size_t units = U16_LENGTH(code_point);
        if (utf16.size() + units > buf_slots - 1) {
            break;
        }
        if (units == 1) {
            utf16.push_back(static_cast<SQLTCHAR>(code_point));
        } else {
            utf16.push_back(static_cast<SQLTCHAR>(U16_LEAD(code_point)));
            utf16.push_back(static_cast<SQLTCHAR>(U16_TRAIL(code_point)));
        }
    }
    std::copy(utf16.begin(), utf16.end(), buf);
    buf[utf16.size()] = 0;
    return utf16.size();
}

inline std::string Convert4ByteSqlWChar(
    const SQLTCHAR *   InputStr,
    SQLINTEGER         BufferLength
    )
{
    if (!InputStr) {
        return "";
    }
    std::vector<UChar32> utf32_buf;
    int i = 0;
    while (true) {
        if (BufferLength > 0 && (i / 2) >= BufferLength) {
            break;
        }
        const UChar32 cp = static_cast<UChar32>(static_cast<uint32_t>(InputStr[i])
                   | (static_cast<uint32_t>(InputStr[i + 1]) << SQLTCHAR_HALF_BITS));
        if (cp == 0) {
            break;
        }
        utf32_buf.push_back(cp);
        i += 2;
    }
    const icu::UnicodeString ustr = icu::UnicodeString::fromUTF32(utf32_buf.data(), static_cast<int32_t>(utf32_buf.size()));
    std::string result;
    ustr.toUTF8String(result);
    return result;
}

inline std::string ConvertUserAppToUTF8(bool user_4_byte, SQLTCHAR* in, SQLINTEGER in_length) {
    if (!in) {
        return "";
    }

    if (user_4_byte) {
        // An explicit length (in code points) needn't be followed by a NUL, so never read past it. Convert4ByteSqlWChar
        // stops at a NUL and reads to it for a length below 1, which serves SQL_NTS but not 0.
        return in_length == 0 ? "" : Convert4ByteSqlWChar(in, in_length);
    }
    if (in_length >= 0) {
        // An explicit length (in characters) needn't be followed by a NUL, so never read past it.
        const uint16_t* in_utf16 = reinterpret_cast<const uint16_t*>(in);
        int32_t length = 0;
        while (length < in_length && in_utf16[length] != 0) {
            length++;
        }
        const icu::UnicodeString unicode_str(reinterpret_cast<const char16_t*>(in_utf16), length);
        std::string buffer_utf8;
        unicode_str.toUTF8String(buffer_utf8);
        return buffer_utf8;
    }
    return ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(in));
}

inline void ConvertUTF8ToDriver(bool driver_4_byte, std::string input, SQLTCHAR* out, SQLSMALLINT out_length) {
    if (out_length <= 0) {
        return;
    }
    if (driver_4_byte) {
        if (out_length < 2) {
            out[0] = 0;
            return;
        }
        const icu::UnicodeString ustr = icu::UnicodeString::fromUTF8(icu::StringPiece(input.c_str(), static_cast<int32_t>(input.length())));
        const int32_t capacity = (static_cast<int32_t>(out_length) - 2) / 2;
        UErrorCode err = U_ZERO_ERROR;
        const int32_t written = ustr.toUTF32(reinterpret_cast<UChar32*>(out), capacity, err);
        const bool conversion_ok = U_SUCCESS(err) != 0 || err == U_BUFFER_OVERFLOW_ERROR;
        const int32_t actual = conversion_ok ? std::min(written, capacity) : 0;
        reinterpret_cast<UChar32*>(out)[actual] = 0;
    } else {
        CopyUTF8ToUTF16Buffer(reinterpret_cast<uint16_t*>(out), out_length, input);
    }
}

inline std::vector<SQLTCHAR> ConvertUserAppInputToBaseDriver(bool user_4_byte, bool driver_4_byte, SQLTCHAR* in, SQLINTEGER in_length) {
    // nullptr is valid ODBC input
    if (in == nullptr) {
        return {};
    }

    const std::string utf8 = ConvertUserAppToUTF8(user_4_byte, in, in_length);
    if (driver_4_byte) {
        const std::vector<uint16_t> utf16 = ConvertUTF8ToUTF16(utf8);
        // utf8 holds in_length characters already. Cutting its UTF-16 to in_length units would cut a 4-byte app's
        // text outside the BMP, as in_length counts its code points.
        const size_t size = utf16.empty() ? 0 : utf16.size() - 1;

        const size_t size_converted = (size * 2) + 2; // Each char expands to 2 SQLTCHAR + null pair
        std::vector<SQLTCHAR> result(size_converted, 0);
        ConvertUTF16ToUTF32(reinterpret_cast<const SQLTCHAR*>(utf16.data()), result.data(), size, size_converted);
        return result;
    }

    std::vector<uint16_t> utf16 = ConvertUTF8ToUTF16(utf8);
    return {
        reinterpret_cast<SQLTCHAR*>(utf16.data()),
        reinterpret_cast<SQLTCHAR*>(utf16.data() + utf16.size())};
}
#endif

#ifdef UNICODE
    #define AS_SQLTCHAR(str) const_cast<SQLTCHAR *>(reinterpret_cast<const SQLTCHAR *>(ConvertUTF8ToUTF16(str).data()))
    #define AS_UTF8_CSTR(str) ConvertUTF16ToUTF8(reinterpret_cast<uint16_t *>(str)).data()
    #define RDS_TSTR(str) ConvertUTF8ToWString(str)
#else
    #define AS_SQLTCHAR(str) const_cast<SQLTCHAR *>(reinterpret_cast<const SQLTCHAR *>((str).data()))
    #define AS_UTF8_CSTR(str) reinterpret_cast<const char *>(str)
    #define RDS_TSTR(str) str
#endif

#define AS_CHAR(str) (reinterpret_cast<char *>(str))
#define AS_CONST_CHAR(str) (reinterpret_cast<const char *>(str))
#define AS_WCHAR(str) (reinterpret_cast<wchar_t *>(str))
#define AS_CONST_WCHAR(str) (reinterpret_cast<const wchar_t*>(str))

#ifdef WIN32
    #define STR_ICMP(str1, str2) strcmpi(str1, str2)
#else
    #define STR_ICMP(str1, str2) strcasecmp(str1, str2)
#endif

#include <cstring>

// Uppercases a-z and nothing else, whatever the locale: for connection string and DSN keys, the option values
// that name an enum, driver names and SQL keywords. RDS_STR_UPPER uses ICU's default locale, which under a
// Turkish one uppercases i to a dotted capital I, so a key such as Region or a value such as iam wouldn't match.
inline std::string ToAsciiUpper(std::string str) {
    for (char& c : str) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return str;
}

inline std::string RDS_STR_UPPER(std::string str) {
    if (!str.empty()) {
        const size_t buf_len = str.length() * 4;
        char *buf = new char[buf_len];
        UErrorCode ucasemap_status = U_ZERO_ERROR;
        UCaseMap *ucasemap = ucasemap_open(nullptr, 0, &ucasemap_status);
        if (U_FAILURE(ucasemap_status)) {
            LOG(ERROR) << std::format("Failed to convert string {} to uppercase when opening ucasemap: {}", str, u_errorName(ucasemap_status));
            delete[] buf;
            return str;
        }
        UErrorCode upper_status = U_ZERO_ERROR;
        ucasemap_utf8ToUpper(ucasemap, buf, static_cast<int32_t>(buf_len), str.c_str(), -1, &upper_status);
        if (U_FAILURE(upper_status)) {
            LOG(ERROR) << std::format("Failed to convert string {} to uppercase: {}\n", str, u_errorName(upper_status));
            ucasemap_close(ucasemap);
            delete[] buf;
            return str;
        }
        std::string upper(buf);
        ucasemap_close(ucasemap);
        delete[] buf;
        return upper;
    }
    return str;
}

// Lowercases A-Z and nothing else, whatever the locale: for host names, AWS Region names and other ASCII protocol
// text. std::tolower follows the C locale: under tr_TR.UTF-8 glibc leaves I as I, and under tr_TR.ISO-8859-9 it maps
// I to 0xFD (dotless i). RDS_STR_UPPER follows ICU's default locale.
inline std::string AsciiLower(std::string str) {
    for (char& c : str) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return str;
}

#define EMPTY_RDS_STR ""

inline std::string TrimStr(std::string &str) {
    str = str.erase(str.find_last_not_of(TEXT(' ')) + 1);
    str = str.erase(0, str.find_first_not_of(TEXT(' ')));
    return str;
}

inline std::vector<std::string> SplitStr(std::string &str, std::string &delimiter) {
    const std::regex pattern(delimiter);
    std::smatch match;
    std::string str_itr = str;
    std::vector<std::string> matches;
    while (std::regex_search(str_itr, match, pattern)) {
        matches.push_back(match.prefix().str());
        str_itr = match.suffix().str();
    }

    if (matches.empty()) {
        matches.push_back(str);
    }

    return matches;
}

inline void Convert4To2ByteString(bool use_4_bytes, SQLTCHAR *in, SQLTCHAR *out, size_t len) {
    if (in == nullptr || len == 0) {
        return;
    }

    if (!use_4_bytes) {
        if (out != nullptr) {
            std::copy(in, in + len, out);
            out[len - 1] = 0;
        }
        return;
    }

    UErrorCode err = U_ZERO_ERROR;
    SQLTCHAR *output = out == nullptr ? in : out;
    const int32_t output_size = static_cast<int32_t>(len - 1);
    int32_t written = 0;
    std::vector<SQLTCHAR> temp(len, 0);

    const int32_t max_src_codepoints = static_cast<int32_t>(len);
    std::vector<UChar32> utf32_buf(max_src_codepoints);
    int32_t num_codepoints = 0;
    for (int32_t i = 0; i < max_src_codepoints; i++) {
        const ptrdiff_t offset = static_cast<ptrdiff_t>(i) * 2;
        utf32_buf[i] = static_cast<UChar32>(static_cast<uint32_t>(in[offset])
                     | (static_cast<uint32_t>(in[offset + 1]) << SQLTCHAR_HALF_BITS));
        if (utf32_buf[i] == 0) {
            break;
        }
        num_codepoints++;
    }

    u_strFromUTF32(
        reinterpret_cast<UChar *>(temp.data()),
        output_size,
        &written,
        utf32_buf.data(),
        num_codepoints,
        &err
    );

    if (U_FAILURE(err) && err != U_BUFFER_OVERFLOW_ERROR) {
        LOG(ERROR) << "ICU conversion failed: " << u_errorName(err);
        output[0] = 0;
        return;
    }

    int32_t end_index = std::min(written, output_size);

    if (end_index > 0) {
        const UChar last = reinterpret_cast<UChar *>(temp.data())[end_index - 1];
        if (U16_IS_LEAD(last)) {
            end_index--;
        }
    }

    std::copy(temp.begin(), temp.begin() + end_index, output);
    output[end_index] = 0;
}

#endif // RDS_STRINGS_H_
