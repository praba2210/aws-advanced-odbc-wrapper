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

// What the wrapper writes into the application's buffers: its own strings, and in the Unicode driver the base driver's
// when the application's SQLWCHAR and the base driver's differ in size (the wrapper converts between them).

#include "../../driver/util/windows_headers.h"

#include <sqlext.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "../../driver/driver.h"
#include "../../driver/error.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/default_plugin.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"
#include "../../driver/util/rds_strings.h"
#include "guarded_buffer.h"

namespace {
    // The base driver: its string (UTF-8), the size of its SQLWCHAR and the BufferLengths it was given. Like psqlODBC,
    // each function writes the longest prefix of the string that fits with a terminator, in its characters (UTF-16
    // units, code points for a 4-byte SQLWCHAR, bytes in ANSI) and not splitting a surrogate pair, returns the full
    // length in them, and returns SQL_SUCCESS_WITH_INFO when it cut the string.
    struct FakeBaseDriver {
        std::string text;
        size_t char_bytes = sizeof(SQLTCHAR);
        SQLINTEGER buffer_length = 0;  // the last call's
        std::vector<SQLINTEGER> buffer_lengths;
        int calls = 0;
        bool fail = false;  // the functions return SQL_ERROR
        // What SQLSetConnectAttr got for a string attribute: its text, read in the base driver's characters.
        std::vector<std::pair<SQLINTEGER, std::string>> string_attrs;
    };
    FakeBaseDriver base_driver;

    // The string in the base driver's characters.
    std::vector<uint32_t> BaseChars() {
        std::vector<uint32_t> chars;
#ifdef UNICODE
        const icu::UnicodeString text = icu::UnicodeString::fromUTF8(base_driver.text);
        const bool four_byte = base_driver.char_bytes == 2 * sizeof(SQLTCHAR);
        for (int32_t i = 0; i < text.length(); i = four_byte ? text.moveIndex32(i, 1) : i + 1) {
            chars.push_back(four_byte ? static_cast<uint32_t>(text.char32At(i)) : text.charAt(i));
        }
#else
        for (const char c : base_driver.text) {
            chars.push_back(static_cast<unsigned char>(c));
        }
#endif
        return chars;
    }

    // Writes the string into out, a buffer of buffer_chars of the base driver's characters.
    SQLRETURN WriteBaseText(SQLPOINTER out, SQLINTEGER buffer_chars) {
        if (base_driver.fail) {
            return SQL_ERROR;
        }
        auto* units = static_cast<SQLTCHAR*>(out);
        if (units == nullptr || buffer_chars <= 0) {
            return units == nullptr ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO;
        }
        const std::vector<uint32_t> chars = BaseChars();
        const size_t step = base_driver.char_bytes / sizeof(SQLTCHAR);
        size_t count = std::min(chars.size(), static_cast<size_t>(buffer_chars) - 1);
        // The first half of a UTF-16 surrogate pair
        if (step == 1 && count > 0 && count < chars.size() && (chars[count - 1] & 0xFC00U) == 0xD800U) {
            --count;
        }
        std::fill_n(units, (count + 1) * step, static_cast<SQLTCHAR>(0));
        for (size_t i = 0; i < count; ++i) {
            units[i * step] = static_cast<SQLTCHAR>(chars[i]);
            if (step == 2) {
                units[(i * step) + 1] = static_cast<SQLTCHAR>(chars[i] >> 16U);
            }
        }
        return count < chars.size() ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    }

    // BufferLength and the returned length count bytes.
    SQLRETURN WriteBaseTextInBytes(SQLPOINTER out, SQLINTEGER buffer_length, SQLINTEGER* length) {
        ++base_driver.calls;
        base_driver.buffer_length = buffer_length;
        base_driver.buffer_lengths.push_back(buffer_length);
        if (length != nullptr) {
            *length = static_cast<SQLINTEGER>(BaseChars().size() * base_driver.char_bytes);
        }
        return WriteBaseText(out, buffer_length / static_cast<SQLINTEGER>(base_driver.char_bytes));
    }

    // BufferLength and the returned length count characters.
    template <typename Length>
    SQLRETURN WriteBaseTextInChars(SQLTCHAR* out, SQLINTEGER buffer_length, Length* length) {
        ++base_driver.calls;
        base_driver.buffer_length = buffer_length;
        base_driver.buffer_lengths.push_back(buffer_length);
        if (length != nullptr) {
            *length = static_cast<Length>(BaseChars().size());
        }
        return WriteBaseText(out, buffer_length);
    }

    SQLRETURN FakeDescribeCol(SQLHSTMT /*stmt*/, SQLUSMALLINT /*column*/, SQLTCHAR* name, SQLSMALLINT buffer_length,
                              SQLSMALLINT* name_length, SQLSMALLINT* /*type*/, SQLULEN* /*size*/,
                              SQLSMALLINT* /*digits*/, SQLSMALLINT* /*nullable*/) {
        return WriteBaseTextInChars(name, buffer_length, name_length);
    }

    SQLRETURN FakeGetCursorName(SQLHSTMT /*stmt*/, SQLTCHAR* name, SQLSMALLINT buffer_length, SQLSMALLINT* length) {
        return WriteBaseTextInChars(name, buffer_length, length);
    }

    SQLRETURN FakeGetDescRec(SQLHDESC /*desc*/, SQLSMALLINT /*rec*/, SQLTCHAR* name, SQLSMALLINT buffer_length,
                             SQLSMALLINT* length, SQLSMALLINT* /*type*/, SQLSMALLINT* /*subtype*/, SQLLEN* /*len*/,
                             SQLSMALLINT* /*precision*/, SQLSMALLINT* /*scale*/, SQLSMALLINT* /*nullable*/) {
        return WriteBaseTextInChars(name, buffer_length, length);
    }

    SQLRETURN FakeNativeSql(SQLHDBC /*dbc*/, SQLTCHAR* /*in*/, SQLINTEGER /*in_length*/, SQLTCHAR* out,
                            SQLINTEGER buffer_length, SQLINTEGER* length) {
        return WriteBaseTextInChars(out, buffer_length, length);
    }

    // BufferLength and the returned length count bytes, in an SQLSMALLINT.
    SQLRETURN WriteBaseTextInShortBytes(SQLPOINTER out, SQLSMALLINT buffer_length, SQLSMALLINT* length) {
        SQLINTEGER long_length = 0;
        const SQLRETURN ret = WriteBaseTextInBytes(out, buffer_length, &long_length);
        if (length != nullptr) {
            *length = static_cast<SQLSMALLINT>(long_length);
        }
        return ret;
    }

    SQLRETURN FakeGetInfo(SQLHDBC /*dbc*/, SQLUSMALLINT /*info_type*/, SQLPOINTER value, SQLSMALLINT buffer_length,
                          SQLSMALLINT* length) {
        return WriteBaseTextInShortBytes(value, buffer_length, length);
    }

    SQLRETURN FakeGetDiagField(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT /*rec*/,
                               SQLSMALLINT /*identifier*/, SQLPOINTER info, SQLSMALLINT buffer_length,
                               SQLSMALLINT* length) {
        return WriteBaseTextInShortBytes(info, buffer_length, length);
    }

    SQLRETURN FakeColAttribute(SQLHSTMT /*stmt*/, SQLUSMALLINT /*column*/, SQLUSMALLINT /*field*/, SQLPOINTER character,
                               SQLSMALLINT buffer_length, SQLSMALLINT* length, SQLLEN* /*numeric*/) {
        return WriteBaseTextInShortBytes(character, buffer_length, length);
    }

    SQLRETURN FakeGetConnectAttr(SQLHDBC /*dbc*/, SQLINTEGER /*attribute*/, SQLPOINTER value, SQLINTEGER buffer_length,
                                 SQLINTEGER* length) {
        return WriteBaseTextInBytes(value, buffer_length, length);
    }

    SQLRETURN FakeGetDescField(SQLHDESC /*desc*/, SQLSMALLINT /*rec*/, SQLSMALLINT /*field*/, SQLPOINTER value,
                               SQLINTEGER buffer_length, SQLINTEGER* length) {
        return WriteBaseTextInBytes(value, buffer_length, length);
    }

    SQLRETURN FakeGetDiagRec(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT /*rec*/,
                             SQLTCHAR* /*state*/, SQLINTEGER* /*native*/, SQLTCHAR* message, SQLSMALLINT buffer_length,
                             SQLSMALLINT* length) {
        ++base_driver.calls;
        base_driver.buffer_length = buffer_length;
        base_driver.buffer_lengths.push_back(buffer_length);
        if (length != nullptr) {
            *length = static_cast<SQLSMALLINT>(BaseChars().size());
        }
        return WriteBaseText(message, buffer_length);
    }

    SQLRETURN FakeSetConnectAttr(SQLHDBC /*dbc*/, SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER length) {
        if (OdbcHelper::IsStringConnectAttr(attribute) && value != nullptr) {
            const auto* units = static_cast<const SQLTCHAR*>(value);
            const size_t step = base_driver.char_bytes / sizeof(SQLTCHAR);
            const size_t max_chars = length >= 0 ? static_cast<size_t>(length) / base_driver.char_bytes : SIZE_MAX;
            std::string text;
            for (size_t i = 0; i < max_chars && units[i * step] != 0; ++i) {
                text.push_back(static_cast<char>(units[i * step]));
            }
            base_driver.string_attrs.emplace_back(attribute, text);
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSetDescField(SQLHDESC /*desc*/, SQLSMALLINT /*rec*/, SQLSMALLINT /*field*/, SQLPOINTER /*value*/,
                               SQLINTEGER /*buffer_length*/) {
        return base_driver.fail ? SQL_ERROR : SQL_SUCCESS;
    }

    SQLRETURN FakeSetDescRec(SQLHDESC /*desc*/, SQLSMALLINT /*rec*/, SQLSMALLINT /*type*/, SQLSMALLINT /*subtype*/,
                             SQLLEN /*length*/, SQLSMALLINT /*precision*/, SQLSMALLINT /*scale*/, SQLPOINTER /*data*/,
                             SQLLEN* /*string_length*/, SQLLEN* /*indicator*/) {
        return base_driver.fail ? SQL_ERROR : SQL_SUCCESS;
    }

    SQLRETURN FakeCopyDesc(SQLHDESC /*source*/, SQLHDESC /*target*/) {
        return base_driver.fail ? SQL_ERROR : SQL_SUCCESS;
    }

    // A base driver handle for SQLAllocHandle: never dereferenced.
    int fake_base_child = 0;

    SQLRETURN FakeAllocHandle(SQLSMALLINT /*handle_type*/, SQLHANDLE /*input*/, SQLHANDLE* output) {
        if (output != nullptr) {
            *output = &fake_base_child;
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeFreeHandle(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/) {
        return SQL_SUCCESS;
    }

    SQLRETURN FakeDriverConnect(SQLHDBC /*dbc*/, SQLHWND /*window*/, SQLTCHAR* /*in*/, SQLSMALLINT /*in_length*/,
                                SQLTCHAR* /*out*/, SQLSMALLINT /*buffer_length*/, SQLSMALLINT* /*out_length*/,
                                SQLUSMALLINT /*completion*/) {
        return SQL_SUCCESS;
    }

    class FakeBaseDriverLoader : public RdsLibLoader {
    public:
        RdsPlatform::FuncHandle GetFunction(const std::string& name) override {
            if (name == RdsFuncNames::DescribeCol) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeDescribeCol);
            }
            if (name == RdsFuncNames::GetCursorName) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetCursorName);
            }
            if (name == RdsFuncNames::GetDescRec) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetDescRec);
            }
            if (name == RdsFuncNames::NativeSql) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeNativeSql);
            }
            if (name == RdsFuncNames::GetConnectAttr) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetConnectAttr);
            }
            if (name == RdsFuncNames::GetDescField) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetDescField);
            }
            if (name == RdsFuncNames::GetDiagRec) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetDiagRec);
            }
            if (name == RdsFuncNames::SetConnectAttr) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSetConnectAttr);
            }
            if (name == RdsFuncNames::GetInfo) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetInfo);
            }
            if (name == RdsFuncNames::GetDiagField) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetDiagField);
            }
            if (name == RdsFuncNames::ColAttribute) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeColAttribute);
            }
            if (name == RdsFuncNames::SetDescField) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSetDescField);
            }
            if (name == RdsFuncNames::SetDescRec) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSetDescRec);
            }
            if (name == RdsFuncNames::CopyDesc) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeCopyDesc);
            }
            if (name == RdsFuncNames::AllocHandle) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeAllocHandle);
            }
            if (name == RdsFuncNames::FreeHandle) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeFreeHandle);
            }
            if (name == RdsFuncNames::DriverConnect) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeDriverConnect);
            }
            return nullptr;
        }
    };

    // The plugin service of a connected handle, for its OdbcHelper.
    class HelperOnlyPluginService : public PluginService {
    public:
        explicit HelperOnlyPluginService(const ENV* env) : odbc_helper_(std::make_shared<OdbcHelper>(nullptr, env)) {}

        std::shared_ptr<OdbcHelper> GetOdbcHelper() override { return odbc_helper_; }

    private:
        std::shared_ptr<OdbcHelper> odbc_helper_;
    };

    constexpr uint32_t GUARD = 0x5A5A5A5A;

    // A string the wrapper wrote for a 2-byte app, as UTF-8.
    std::string AppText(SQLTCHAR* buffer) {
#ifdef UNICODE
        return ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(buffer));
#else
        return reinterpret_cast<const char*>(buffer);
#endif
    }

    // Text as a 2-byte app passes it, with NULs after it.
    std::vector<SQLTCHAR> AppString(const std::string& text) {
#ifdef UNICODE
        const std::vector<uint16_t> utf16 = ConvertUTF8ToUTF16(text);
        std::vector<SQLTCHAR> out(utf16.begin(), utf16.end());
#else
        std::vector<SQLTCHAR> out(text.begin(), text.end());
#endif
        out.resize(out.size() + 4, 0);
        return out;
    }

    // The ASCII text of a string the wrapper wrote, up to its terminator.
    template <typename Char>
    std::string Text(const Char* buffer) {
        std::string text;
        for (; *buffer != 0; ++buffer) {
            text.push_back(static_cast<char>(*buffer));
        }
        return text;
    }
}  // namespace

// Connected handles over the fake base driver. The statement has no base driver statement, so the wrapper answers for
// it, until a test gives it one.
class OutputBufferTest : public testing::Test {
protected:
    ENV env_;
    DBC dbc_{};
    STMT stmt_{};
    DESC desc_{};
    int fake_handle_ = 0;

    void SetUp() override {
        base_driver = FakeBaseDriver{};
        env_.driver_lib_loader = std::make_shared<FakeBaseDriverLoader>();
        dbc_.env = &env_;
        dbc_.wrapped_dbc = &fake_handle_;
        dbc_.plugin_service = std::make_shared<HelperOnlyPluginService>(&env_);
        stmt_.dbc = &dbc_;
        desc_.dbc = &dbc_;
        desc_.wrapped_desc = &fake_handle_;
    }

    // Connects the handle to the fake base driver through the default plugin, which applies the attributes set before.
    SQLRETURN ConnectToBaseDriver() {
        dbc_.wrapped_dbc = &fake_handle_;
        dbc_.conn_attr[KEY_MONITORING_CONN_UUID] = "output-buffer-test";  // no dialect to update
        DefaultPlugin plugin(&dbc_);
        return plugin.Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    }

    // The text the base driver got for a string attribute at connect.
    static std::string ReplayedString(SQLINTEGER attribute) {
        for (const auto& [key, text] : base_driver.string_attrs) {
            if (key == attribute) {
                return text;
            }
        }
        return "<not set>";
    }

#ifdef UNICODE
    // An app with a 4-byte SQLWCHAR over a 2-byte base driver.
    void UseFourByteApp() {
        env_.use_4_bytes_user_app = true;
    }

    // A 2-byte app over a base driver with a 4-byte SQLWCHAR.
    void UseFourByteDriver() {
        dbc_.plugin_service->GetOdbcHelper()->SetUse4BytesBaseDriver(true);
        base_driver.char_bytes = 2 * sizeof(SQLTCHAR);
    }
#endif
};

#ifdef UNICODE
// SQLGetConnectAttr and SQLGetDescField count bytes. The base driver gets a BufferLength for as many of its own
// characters as the app's buffer holds, so a string that doesn't fit comes back cut with SQL_SUCCESS_WITH_INFO, as the
// base driver returns it. Given the app's byte count, a 2-byte driver had room for twice as many characters and
// returned SQL_SUCCESS for a string the conversion then cut.
TEST_F(OutputBufferTest, GetConnectAttr_FourByteAppOverATwoByteDriverReportsTheCut) {
    UseFourByteApp();
    base_driver.text = "abcdef";
    std::vector<uint32_t> value(8, GUARD);  // the app's buffer: 3 characters and the terminator
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(4 * sizeof(uint32_t)), &length));
    EXPECT_EQ(static_cast<SQLINTEGER>(4 * sizeof(SQLTCHAR)), base_driver.buffer_lengths.front());
    EXPECT_EQ("abc", Text(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(6 * sizeof(uint32_t)), length);
    EXPECT_EQ(4, std::count(value.begin() + 4, value.end(), GUARD));
}

TEST_F(OutputBufferTest, GetDescField_FourByteAppOverATwoByteDriverReportsTheCut) {
    UseFourByteApp();
    base_driver.text = "abcdef";
    std::vector<uint32_t> value(8, GUARD);
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDescField(&desc_, 1, SQL_DESC_NAME, value.data(),
        static_cast<SQLINTEGER>(4 * sizeof(uint32_t)), &length));
    EXPECT_EQ(static_cast<SQLINTEGER>(4 * sizeof(SQLTCHAR)), base_driver.buffer_lengths.front());
    EXPECT_EQ("abc", Text(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(6 * sizeof(uint32_t)), length);
    EXPECT_EQ(4, std::count(value.begin() + 4, value.end(), GUARD));
}

// The other way round, a 4-byte driver given the app's byte count had room for half the app's characters: a string
// that fits the app's buffer came back cut.
TEST_F(OutputBufferTest, GetConnectAttr_TwoByteAppOverAFourByteDriverGetsTheWholeString) {
    UseFourByteDriver();
    base_driver.text = "abcdef";
    std::vector<SQLTCHAR> value(8, 0x5A);  // the app's buffer: 7 characters and the terminator
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * 2 * sizeof(SQLTCHAR)), base_driver.buffer_length);
    EXPECT_EQ("abcdef", Text(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(6 * sizeof(SQLTCHAR)), length);
}

TEST_F(OutputBufferTest, GetDescField_TwoByteAppOverAFourByteDriverGetsTheWholeString) {
    UseFourByteDriver();
    base_driver.text = "abcdef";
    std::vector<SQLTCHAR> value(8, 0x5A);
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDescField(&desc_, 1, SQL_DESC_NAME, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * 2 * sizeof(SQLTCHAR)), base_driver.buffer_length);
    EXPECT_EQ("abcdef", Text(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(6 * sizeof(SQLTCHAR)), length);
}
#endif

// SQLGetDiagRec's BufferLength counts characters and can't be negative: SQL_ERROR, nothing is written, and the base
// driver isn't asked (unixODBC passes the request for a driver's record through without checking it). The Unicode
// driver sized its buffer for the base driver's message from it and threw std::length_error out of the C API.
TEST_F(OutputBufferTest, DiagRec_NegativeBufferLengthIsAnError) {
    base_driver.text = "base driver message";
    std::vector<SQLTCHAR> state(2 * MAX_SQL_STATE_LEN, 0x5A);
    std::vector<SQLTCHAR> message(32, 0x5A);
    SQLINTEGER native = 77;
    SQLSMALLINT length = 77;
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state.data(), &native, message.data(), -1,
        &length, false));
    EXPECT_EQ(0, base_driver.calls);

    // The wrapper's own record too.
    dbc_.err = std::make_unique<ErrInfo>("wrapper message", ERR_GENERAL_ERROR);
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state.data(), &native, message.data(), -1,
        &length, false));
    EXPECT_EQ(2 * MAX_SQL_STATE_LEN, std::count(state.begin(), state.end(), 0x5A));
    EXPECT_EQ(32, std::count(message.begin(), message.end(), 0x5A));
    EXPECT_EQ(77, native);
    EXPECT_EQ(77, length);

    // An invalid handle stays SQL_INVALID_HANDLE.
    EXPECT_EQ(SQL_INVALID_HANDLE, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, nullptr, 1, state.data(), &native, message.data(),
        -1, &length, false));
}

// The wrapper's message is cut to a terminated prefix of BufferLength characters, with SQL_SUCCESS_WITH_INFO and the
// full length: also at an exact fit, where the terminator takes the last character's place.
TEST_F(OutputBufferTest, DiagRec_TruncatedMessageIsATerminatedPrefix) {
    dbc_.err = std::make_unique<ErrInfo>("SQLExecDirect - Connection not open", ERR_CONNECTION_NOT_OPEN);
    const auto full = static_cast<SQLSMALLINT>(dbc_.err->error_msg.size());
    std::vector<SQLTCHAR> message(8 + 8, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr, message.data(), 8,
        &length, false));
    EXPECT_EQ("SQLExec", Text(message.data()));
    EXPECT_EQ(full, length);
    EXPECT_EQ(8, std::count(message.begin() + 8, message.end(), 0x5A));

    dbc_.err = std::make_unique<ErrInfo>("abcd", ERR_GENERAL_ERROR);
    std::vector<SQLTCHAR> exact(4 + 4, 0x5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr, exact.data(), 4,
        &length, false));
    EXPECT_EQ("abc", Text(exact.data()));
    EXPECT_EQ(4, length);
}

// The message's length counts the app's characters, also when the app asks for the length only (no buffer, or a
// BufferLength of 0): "café" is 4 SQLWCHARs in the Unicode driver, not its 5 UTF-8 bytes.
TEST_F(OutputBufferTest, DiagRec_LengthCountsTheAppCharacters) {
    dbc_.err = std::make_unique<ErrInfo>("caf\xC3\xA9", ERR_GENERAL_ERROR);
#ifdef UNICODE
    constexpr SQLSMALLINT CHARS = 4;
#else
    constexpr SQLSMALLINT CHARS = 5;
#endif
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr, nullptr, 16, &length, false));
    EXPECT_EQ(CHARS, length);

    length = 0;
    std::vector<SQLTCHAR> message(8, 0x5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr, message.data(), 0,
        &length, false));
    EXPECT_EQ(CHARS, length);
    EXPECT_EQ(8, std::count(message.begin(), message.end(), 0x5A));

    length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr, message.data(), 8, &length,
        false));
    EXPECT_EQ(CHARS, length);
    EXPECT_EQ("caf\xC3\xA9", AppText(message.data()));
}

#ifdef UNICODE
// An app with a 4-byte SQLWCHAR gets 4-byte characters within its BufferLength characters, and lengths in code points:
// U+1F600 is one character (two UTF-16 units, four UTF-8 bytes). The wrapper expanded the message's full UTF-16 length
// from the app's buffer, reading past what it had written and, for a long message, past the buffer.
TEST_F(OutputBufferTest, DiagRec_FourByteAppGetsCodePoints) {
    UseFourByteApp();
    dbc_.err = std::make_unique<ErrInfo>("a\xF0\x9F\x98\x80", ERR_GENERAL_ERROR);
    std::vector<uint32_t> message(3 + 4, GUARD);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr,
        reinterpret_cast<SQLTCHAR*>(message.data()), 3, &length, false));
    EXPECT_EQ(2, length);
    EXPECT_EQ(static_cast<uint32_t>('a'), message[0]);
    EXPECT_EQ(0x1F600U, message[1]);
    EXPECT_EQ(0U, message[2]);
    EXPECT_EQ(4, std::count(message.begin() + 3, message.end(), GUARD));

    length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr, nullptr, 0, &length,
        false));
    EXPECT_EQ(2, length);

    // A message more than twice as long as the buffer: its first characters and a terminator, nothing past the buffer.
    dbc_.err = std::make_unique<ErrInfo>("SQLExecDirect - Connection not open", ERR_CONNECTION_NOT_OPEN);
    std::vector<uint32_t> short_message(8 + 32, GUARD);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr,
        reinterpret_cast<SQLTCHAR*>(short_message.data()), 8, &length, false));
    EXPECT_EQ("SQLExec", Text(short_message.data()));
    EXPECT_EQ(static_cast<SQLSMALLINT>(dbc_.err->error_msg.size()), length);
    EXPECT_EQ(32, std::count(short_message.begin() + 8, short_message.end(), GUARD));
}
#endif

// With no record, the message is an empty string in the app's characters (4 bytes of zeros for an app with a 4-byte
// SQLWCHAR, where the wrapper wrote 2), and a BufferLength of 0 gets nothing.
TEST_F(OutputBufferTest, DiagRec_NoRecordWritesAnEmptyMessageInTheBuffer) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    std::vector<uint32_t> message(4, GUARD);
    SQLSMALLINT length = 77;
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr,
        reinterpret_cast<SQLTCHAR*>(message.data()), 0, &length, false));
    EXPECT_EQ(4, std::count(message.begin(), message.end(), GUARD));
#ifdef UNICODE
    UseFourByteApp();
#endif
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, nullptr, nullptr,
        reinterpret_cast<SQLTCHAR*>(message.data()), 2, &length, false));
#ifdef UNICODE
    EXPECT_EQ(0U, message[0]);
#else
    EXPECT_EQ(0, reinterpret_cast<const SQLTCHAR*>(message.data())[0]);
#endif
    EXPECT_EQ(0, length);
}

// SQLGetCursorName's BufferLength counts characters and can't be negative: HY090, as the driver manager returns, and
// nothing is written. For the wrapper's own name, the Unicode driver wrote a terminator two characters before the
// buffer and the ANSI driver wrote the whole name.
TEST_F(OutputBufferTest, GetCursorName_NegativeBufferLengthIsHY090) {
    struct {
        SQLTCHAR before[8];
        SQLTCHAR name[16];
    } out{};
    std::fill_n(&out.before[0], sizeof(out) / sizeof(SQLTCHAR), static_cast<SQLTCHAR>(0x5A));
    SQLSMALLINT length = 77;
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetCursorName(&stmt_, out.name, -1, &length));
    EXPECT_EQ(static_cast<std::ptrdiff_t>(sizeof(out) / sizeof(SQLTCHAR)),
        std::count(&out.before[0], &out.before[0] + sizeof(out) / sizeof(SQLTCHAR), static_cast<SQLTCHAR>(0x5A)));
    EXPECT_EQ(77, length);
    ASSERT_TRUE(stmt_.err);
    EXPECT_EQ("HY090", stmt_.err->sqlstate);
}

// The length of the wrapper's own name counts characters: the Unicode driver reported bytes, twice the count, and for
// a non-ASCII name twice its UTF-8 bytes. A name that doesn't fit is cut to a terminated prefix.
TEST_F(OutputBufferTest, GetCursorName_WrapperNameLengthCountsCharacters) {
    std::vector<SQLTCHAR> name(16, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetCursorName(&stmt_, name.data(), static_cast<SQLSMALLINT>(name.size()), &length));
    EXPECT_EQ("CUR_0", AppText(name.data()));
    EXPECT_EQ(5, length);

    std::vector<SQLTCHAR> short_name(3 + 4, 0x5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetCursorName(&stmt_, short_name.data(), 3, &length));
    EXPECT_EQ("CU", AppText(short_name.data()));
    EXPECT_EQ(5, length);
    EXPECT_EQ(4, std::count(short_name.begin() + 3, short_name.end(), 0x5A));

    stmt_.cursor_name = "caf\xC3\xA9";
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetCursorName(&stmt_, nullptr, 0, &length));
#ifdef UNICODE
    EXPECT_EQ(4, length);
#else
    EXPECT_EQ(5, length);
#endif
}

#ifdef UNICODE
// An app with a 4-byte SQLWCHAR has BufferLength 4-byte characters for the name. The wrapper used half of them.
TEST_F(OutputBufferTest, GetCursorName_FourByteAppGetsTheWholeBuffer) {
    UseFourByteApp();
    std::vector<uint32_t> name(6 + 4, GUARD);  // room for "CUR_0" and the terminator
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetCursorName(&stmt_, reinterpret_cast<SQLTCHAR*>(name.data()), 6, &length));
    EXPECT_EQ("CUR_0", Text(name.data()));
    EXPECT_EQ(5, length);
    EXPECT_EQ(4, std::count(name.begin() + 6, name.end(), GUARD));
}
#endif

// Before a connect, SQLGetConnectAttr answers a string attribute from what SQLSetConnectAttr stored: the app's own
// pointer and length (SQL_NTS, or bytes). It writes the app's characters in BufferLength bytes, cut to a terminated
// prefix, and sets the length in bytes. The Unicode driver wrote the string's first byte as narrow text, neither
// driver set *StringLengthPtr, and a string whose length was the size of an integer was written as one.
TEST_F(OutputBufferTest, GetConnectAttr_BeforeConnectWritesTheStoredString) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    std::vector<SQLTCHAR> catalog = AppString("postgres");
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, catalog.data(), SQL_NTS));

    std::vector<SQLTCHAR> value(16, 0x5A);
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ("postgres", AppText(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * sizeof(SQLTCHAR)), length);

    // Room for 3 characters and the terminator.
    std::vector<SQLTCHAR> short_value(4 + 4, 0x5A);
    length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, short_value.data(),
        static_cast<SQLINTEGER>(4 * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ("pos", AppText(short_value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * sizeof(SQLTCHAR)), length);
    EXPECT_EQ(4, std::count(short_value.begin() + 4, short_value.end(), 0x5A));

    // The length only.
    length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, nullptr, 0, &length));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * sizeof(SQLTCHAR)), length);

    // A string of 2 characters has a length of 2 or 4 bytes, the size of an integer: it is still a string.
    std::vector<SQLTCHAR> short_catalog = AppString("db");
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, short_catalog.data(),
        static_cast<SQLINTEGER>(2 * sizeof(SQLTCHAR))));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ("db", AppText(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(2 * sizeof(SQLTCHAR)), length);

    // A length in bytes, with no NUL after the string: nothing past it is read.
    std::vector<SQLTCHAR> unterminated = AppString("postgresXYZ");
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, unterminated.data(),
        static_cast<SQLINTEGER>(8 * sizeof(SQLTCHAR))));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ("postgres", AppText(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * sizeof(SQLTCHAR)), length);
}

// A negative BufferLength for a string attribute is HY090, and nothing is written. The wrapper gave snprintf the
// length as a size_t and wrote the whole string.
TEST_F(OutputBufferTest, GetConnectAttr_BeforeConnectNegativeBufferLengthIsHY090) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    std::vector<SQLTCHAR> catalog = AppString("postgres");
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, catalog.data(), SQL_NTS));

    std::vector<SQLTCHAR> value(16, 0x5A);
    SQLINTEGER length = 77;
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(), -1, &length));
    EXPECT_EQ(16, std::count(value.begin(), value.end(), 0x5A));
    EXPECT_EQ(77, length);
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ("HY090", dbc_.err->sqlstate);
}

#ifdef UNICODE
// An app with a 4-byte SQLWCHAR stored a 4-byte string and gets 4-byte characters back, with the length in bytes.
TEST_F(OutputBufferTest, GetConnectAttr_BeforeConnectFourByteApp) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    UseFourByteApp();
    std::vector<uint32_t> catalog = {'p', 'o', 's', 't', 'g', 'r', 'e', 's', 0};
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, catalog.data(), SQL_NTS));

    std::vector<uint32_t> value(16, GUARD);
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(uint32_t)), &length));
    EXPECT_EQ("postgres", Text(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * sizeof(uint32_t)), length);

    std::vector<uint32_t> short_value(4 + 4, GUARD);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, short_value.data(),
        static_cast<SQLINTEGER>(4 * sizeof(uint32_t)), &length));
    EXPECT_EQ("pos", Text(short_value.data()));
    EXPECT_EQ(4, std::count(short_value.begin() + 4, short_value.end(), GUARD));
}
#endif

// SQLNativeSql, SQLDescribeCol, SQLGetCursorName and SQLGetDescRec count BufferLength in characters, and it can't be
// negative: HY090, as the driver manager returns for the first three, and the base driver isn't called. In the Unicode
// driver's conversion its buffer for the base driver was sized from it, which threw std::length_error out of the C API.
// (The fake base driver ignores the statement text, so SQLNativeSql gets none.)
TEST_F(OutputBufferTest, NegativeCharacterBufferLengthIsHY090) {
#ifdef UNICODE
    UseFourByteApp();
#endif
    OdbcHelper::SetWrappedStmt(&stmt_, &fake_handle_);
    base_driver.text = "abcdef";
    std::vector<uint32_t> out(8, GUARD);
    auto* const out_chars = reinterpret_cast<SQLTCHAR*>(out.data());
    SQLSMALLINT length = 77;
    SQLINTEGER sql_length = 77;

    EXPECT_EQ(SQL_ERROR, RDS_SQLNativeSql(&dbc_, nullptr, SQL_NTS, out_chars, -1, &sql_length));
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ("HY090", dbc_.err->sqlstate);

    EXPECT_EQ(SQL_ERROR, RDS_SQLDescribeCol(&stmt_, 1, out_chars, -1, &length, nullptr, nullptr, nullptr, nullptr));
    ASSERT_TRUE(stmt_.err);
    EXPECT_EQ("HY090", stmt_.err->sqlstate);

    stmt_.err.reset();
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetCursorName(&stmt_, out_chars, -1, &length));
    ASSERT_TRUE(stmt_.err);
    EXPECT_EQ("HY090", stmt_.err->sqlstate);

    EXPECT_EQ(SQL_ERROR, RDS_SQLGetDescRec(&desc_, 1, out_chars, -1, &length, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr));
    ASSERT_TRUE(desc_.err);
    EXPECT_EQ("HY090", desc_.err->sqlstate);

    EXPECT_EQ(0, base_driver.calls);
    EXPECT_EQ(8, std::count(out.begin(), out.end(), GUARD));
    EXPECT_EQ(77, length);
    EXPECT_EQ(77, sql_length);
}

#ifdef UNICODE
// An app with a 4-byte SQLWCHAR has BufferLength 4-byte characters. The conversion from the 2-byte base driver's
// characters gave it half of them: a name the base driver returned whole, with SQL_SUCCESS, came back cut.
TEST_F(OutputBufferTest, CharacterBuffersOfAFourByteAppAreUsedWhole) {
    UseFourByteApp();
    OdbcHelper::SetWrappedStmt(&stmt_, &fake_handle_);
    base_driver.text = "abcdef";
    constexpr SQLSMALLINT CHARS = 8;  // room for "abcdef" and the terminator
    SQLSMALLINT length = 0;
    SQLINTEGER sql_length = 0;

    std::vector<uint32_t> out(CHARS + 4, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLNativeSql(&dbc_, nullptr, SQL_NTS, reinterpret_cast<SQLTCHAR*>(out.data()), CHARS,
        &sql_length));
    EXPECT_EQ("abcdef", Text(out.data()));
    EXPECT_EQ(CHARS, base_driver.buffer_length);
    EXPECT_EQ(4, std::count(out.begin() + CHARS, out.end(), GUARD));

    out.assign(CHARS + 4, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLDescribeCol(&stmt_, 1, reinterpret_cast<SQLTCHAR*>(out.data()), CHARS, &length,
        nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ("abcdef", Text(out.data()));
    EXPECT_EQ(4, std::count(out.begin() + CHARS, out.end(), GUARD));

    out.assign(CHARS + 4, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetCursorName(&stmt_, reinterpret_cast<SQLTCHAR*>(out.data()), CHARS, &length));
    EXPECT_EQ("abcdef", Text(out.data()));
    EXPECT_EQ(4, std::count(out.begin() + CHARS, out.end(), GUARD));

    out.assign(CHARS + 4, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDescRec(&desc_, 1, reinterpret_cast<SQLTCHAR*>(out.data()), CHARS, &length,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ("abcdef", Text(out.data()));
    EXPECT_EQ(4, std::count(out.begin() + CHARS, out.end(), GUARD));

    // A name that doesn't fit is cut where the app's buffer ends, with the base driver's SQL_SUCCESS_WITH_INFO.
    out.assign(CHARS + 4, GUARD);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLDescribeCol(&stmt_, 1, reinterpret_cast<SQLTCHAR*>(out.data()), 4, &length,
        nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ("abc", Text(out.data()));
    EXPECT_EQ(CHARS, std::count(out.begin() + 4, out.end(), GUARD));
}
#endif

// Before a connect, an integer attribute can be set with an SQL_IS_* length. SQLGetConnectAttr writes it as that type
// (an SQLUINTEGER, SQLUSMALLINT or SQLPOINTER); it read the integer as the address of a string.
TEST_F(OutputBufferTest, GetConnectAttr_BeforeConnectIntegerWithAnIsLength) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_LOGIN_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(7)), SQL_IS_UINTEGER));
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(9)), SQL_IS_INTEGER));
    int window = 0;
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_QUIET_MODE, &window, SQL_IS_POINTER));
    constexpr SQLINTEGER DRIVER_ATTRIBUTE = 30001;  // a driver-defined attribute
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, DRIVER_ATTRIBUTE,
        reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(5)), SQL_IS_USMALLINT));

    SQLUINTEGER timeout = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_LOGIN_TIMEOUT, &timeout, 0, nullptr));
    EXPECT_EQ(7U, timeout);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_TIMEOUT, &timeout, 0, nullptr));
    EXPECT_EQ(9U, timeout);
    SQLPOINTER handle = nullptr;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_QUIET_MODE, &handle, SQL_IS_POINTER, nullptr));
    EXPECT_EQ(static_cast<SQLPOINTER>(&window), handle);
    struct {
        SQLUSMALLINT value;
        SQLUSMALLINT after;
    } small{0, 0x5A5A};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, DRIVER_ATTRIBUTE, &small.value, 0, nullptr));
    EXPECT_EQ(5, small.value);
    EXPECT_EQ(0x5A5A, small.after);
}

// A string attribute set before a connect is copied, so the app's buffer needn't outlive SQLSetConnectAttr. The
// wrapper kept the app's pointer and read whatever its buffer held later.
TEST_F(OutputBufferTest, GetConnectAttr_BeforeConnectStringIsACopy) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    std::vector<SQLTCHAR> catalog = AppString("postgres");
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, catalog.data(), SQL_NTS));
    std::fill(catalog.begin(), catalog.begin() + 8, static_cast<SQLTCHAR>('X'));

    std::vector<SQLTCHAR> value(16, 0x5A);
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ("postgres", AppText(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * sizeof(SQLTCHAR)), length);
}

// At connect, the base driver gets the string attributes set before it from the wrapper's copy.
TEST_F(OutputBufferTest, Connect_AppliesTheCopyOfAStringAttribute) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    std::vector<SQLTCHAR> catalog = AppString("postgres");
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, catalog.data(), SQL_NTS));
    std::fill(catalog.begin(), catalog.begin() + 8, static_cast<SQLTCHAR>('X'));

    EXPECT_EQ(SQL_SUCCESS, ConnectToBaseDriver());
    EXPECT_EQ("postgres", ReplayedString(SQL_ATTR_CURRENT_CATALOG));
}

#ifdef UNICODE
// An app with a 4-byte SQLWCHAR over a 2-byte base driver: the base driver gets the attribute in its own characters.
// It got the app's 4-byte string, which it read as "p".
TEST_F(OutputBufferTest, Connect_AppliesAStringAttributeInTheBaseDriverCharacters) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    UseFourByteApp();
    std::vector<uint32_t> catalog = {'p', 'o', 's', 't', 'g', 'r', 'e', 's', 0};
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, catalog.data(), SQL_NTS));

    EXPECT_EQ(SQL_SUCCESS, ConnectToBaseDriver());
    EXPECT_EQ("postgres", ReplayedString(SQL_ATTR_CURRENT_CATALOG));
}
#endif

// When the wrapper cuts its own cursor name it says so with a 01004 record, as SQLGetInfo and SQLDriverConnect do; a
// name that fits leaves none.
TEST_F(OutputBufferTest, GetCursorName_CutWrapperNameHasA01004Record) {
    std::vector<SQLTCHAR> name(3 + 4, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetCursorName(&stmt_, name.data(), 3, &length));
    ASSERT_TRUE(stmt_.err);
    EXPECT_EQ("01004", stmt_.err->sqlstate);
    EXPECT_EQ("SQLGetCursorName - The cursor name was truncated: the buffer needs 6 characters",
        std::string(stmt_.err->error_msg));

    std::vector<SQLTCHAR> roomy(16, 0x5A);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetCursorName(&stmt_, roomy.data(), static_cast<SQLSMALLINT>(roomy.size()), &length));
    EXPECT_FALSE(stmt_.err);
}

// The same for a string attribute SQLGetConnectAttr answers before a connect. The value is cut to the app's whole
// characters, so a BufferLength that isn't a whole number of them is cut too.
TEST_F(OutputBufferTest, GetConnectAttr_BeforeConnectCutValueHasA01004Record) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    std::vector<SQLTCHAR> catalog = AppString("postgres");
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, catalog.data(), SQL_NTS));

    std::vector<SQLTCHAR> value(16, 0x5A);
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(4 * sizeof(SQLTCHAR)), &length));
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ("01004", dbc_.err->sqlstate);
    EXPECT_EQ("SQLGetConnectAttr - The value was truncated: the buffer needs " + std::to_string(9 * sizeof(SQLTCHAR))
        + " bytes", std::string(dbc_.err->error_msg));

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_FALSE(dbc_.err);

#ifdef UNICODE
    // 17 bytes hold 8 SQLWCHARs: room for 7 and the terminator.
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(8 * sizeof(SQLTCHAR) + 1), &length));
    EXPECT_EQ("postgre", AppText(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * sizeof(SQLTCHAR)), length);
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ("01004", dbc_.err->sqlstate);
#endif
}

#ifdef UNICODE
namespace {
    // "a", U+1F600 and "b": 3 code points, 4 UTF-16 units.
    constexpr const char* OUTSIDE_BMP = "a\xF0\x9F\x98\x80" "b";

    std::vector<uint32_t> CodePoints(const uint32_t* buffer) {
        std::vector<uint32_t> points;
        for (; *buffer != 0; ++buffer) {
            points.push_back(*buffer);
        }
        return points;
    }

    std::vector<uint16_t> Utf16Units(const SQLTCHAR* buffer) {
        std::vector<uint16_t> units;
        for (; *buffer != 0; ++buffer) {
            units.push_back(*buffer);
        }
        return units;
    }
}  // namespace

// In conversion mode the app gets lengths in its own characters, counted from the whole string: code points for an app
// with a 4-byte SQLWCHAR over a 2-byte base driver, whose lengths are UTF-16 units. A string outside the BMP was
// reported one character too long per surrogate pair, also when the app asked only for the length.
TEST_F(OutputBufferTest, CharacterLengthsOfAFourByteAppCountCodePoints) {
    UseFourByteApp();
    OdbcHelper::SetWrappedStmt(&stmt_, &fake_handle_);
    base_driver.text = OUTSIDE_BMP;
    const std::vector<uint32_t> expected = {'a', 0x1F600, 'b'};
    std::vector<uint32_t> out(8, GUARD);
    auto* const out_chars = reinterpret_cast<SQLTCHAR*>(out.data());
    SQLSMALLINT length = 0;
    SQLINTEGER sql_length = 0;

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLNativeSql(&dbc_, nullptr, SQL_NTS, out_chars, 8, &sql_length));
    EXPECT_EQ(expected, CodePoints(out.data()));
    EXPECT_EQ(3, sql_length);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLNativeSql(&dbc_, nullptr, SQL_NTS, nullptr, 0, &sql_length));
    EXPECT_EQ(3, sql_length);

    out.assign(8, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLDescribeCol(&stmt_, 1, out_chars, 8, &length, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(expected, CodePoints(out.data()));
    EXPECT_EQ(3, length);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLDescribeCol(&stmt_, 1, nullptr, 0, &length, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(3, length);

    out.assign(8, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetCursorName(&stmt_, out_chars, 8, &length));
    EXPECT_EQ(expected, CodePoints(out.data()));
    EXPECT_EQ(3, length);

    out.assign(8, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDescRec(&desc_, 1, out_chars, 8, &length, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr));
    EXPECT_EQ(expected, CodePoints(out.data()));
    EXPECT_EQ(3, length);

    // The base driver's record.
    out.assign(8, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_STMT, &stmt_, 1, nullptr, nullptr, out_chars, 8, &length,
        false));
    EXPECT_EQ(expected, CodePoints(out.data()));
    EXPECT_EQ(3, length);
}

// A string that doesn't fit is cut where the app's buffer ends, in its characters, with its whole length; when only the
// conversion cut it (the base driver's last call returned it whole), the wrapper posts the 01004. Two-byte characters
// hold fewer code points than a buffer of 4-byte ones: the base driver's cut gave the app fewer than it had room for.
TEST_F(OutputBufferTest, CutStringOfAFourByteAppFillsItsBuffer) {
    UseFourByteApp();
    OdbcHelper::SetWrappedStmt(&stmt_, &fake_handle_);
    base_driver.text = "a\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80" "b";  // 5 code points, 8 UTF-16 units
    std::vector<uint32_t> out(4 + 4, GUARD);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLDescribeCol(&stmt_, 1, reinterpret_cast<SQLTCHAR*>(out.data()), 4, &length,
        nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ((std::vector<uint32_t>{'a', 0x1F600, 0x1F600}), CodePoints(out.data()));
    EXPECT_EQ(5, length);
    EXPECT_EQ(4, std::count(out.begin() + 4, out.end(), GUARD));
    ASSERT_TRUE(stmt_.err);
    EXPECT_EQ("01004", stmt_.err->sqlstate);
    EXPECT_EQ("SQLDescribeCol - The column name was truncated: the buffer needs 6 characters",
        std::string(stmt_.err->error_msg));
}

// A 2-byte app over a base driver with a 4-byte SQLWCHAR gets UTF-16 lengths: U+1F600 is two units. A string that fits
// the base driver's characters but not the app's is cut with SQL_SUCCESS_WITH_INFO and a 01004 record; it came back
// cut with SQL_SUCCESS.
TEST_F(OutputBufferTest, CharacterLengthsOfATwoByteAppOverAFourByteDriverCountUtf16Units) {
    UseFourByteDriver();
    OdbcHelper::SetWrappedStmt(&stmt_, &fake_handle_);
    base_driver.text = OUTSIDE_BMP;
    std::vector<SQLTCHAR> out(8 + 4, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLDescribeCol(&stmt_, 1, out.data(), 8, &length, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ((std::vector<uint16_t>{'a', 0xD83D, 0xDE00, 'b'}), Utf16Units(out.data()));
    EXPECT_EQ(4, length);

    // Room for 2 units and the terminator: "a", and not the first half of the pair.
    out.assign(8 + 4, 0x5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLDescribeCol(&stmt_, 1, out.data(), 3, &length, nullptr, nullptr, nullptr,
        nullptr));
    EXPECT_EQ((std::vector<uint16_t>{'a'}), Utf16Units(out.data()));
    EXPECT_EQ(4, length);
    ASSERT_TRUE(stmt_.err);
    EXPECT_EQ("01004", stmt_.err->sqlstate);
}

// The same for the functions that count bytes: 4 bytes per code point for a 4-byte app, 2 per UTF-16 unit for a 2-byte
// one. The base driver's byte count was doubled or halved instead.
TEST_F(OutputBufferTest, ByteLengthsCountTheAppCharacters) {
    base_driver.text = OUTSIDE_BMP;
    UseFourByteApp();
    std::vector<uint32_t> value(8, GUARD);
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(uint32_t)), &length));
    EXPECT_EQ((std::vector<uint32_t>{'a', 0x1F600, 'b'}), CodePoints(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(3 * sizeof(uint32_t)), length);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDescField(&desc_, 1, SQL_DESC_NAME, nullptr, 0, &length));
    EXPECT_EQ(static_cast<SQLINTEGER>(3 * sizeof(uint32_t)), length);

    env_.use_4_bytes_user_app = false;
    UseFourByteDriver();
    std::vector<SQLTCHAR> narrow(8, 0x5A);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDescField(&desc_, 1, SQL_DESC_NAME, narrow.data(),
        static_cast<SQLINTEGER>(narrow.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ((std::vector<uint16_t>{'a', 0xD83D, 0xDE00, 'b'}), Utf16Units(narrow.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(4 * sizeof(SQLTCHAR)), length);
}
#endif

#ifdef UNICODE
// SQLGetInfo, SQLGetDiagField (a base driver's record) and SQLColAttribute count bytes of the app's characters, from
// the whole string, also when the app asks only for the length: 4 per code point for a 4-byte app, 2 per UTF-16 unit
// for a 2-byte app over a 4-byte base driver. The base driver's byte count was doubled or halved, and a length-only
// call went to the base driver unconverted.
TEST_F(OutputBufferTest, InfoDiagFieldAndColAttributeLengthsCountTheAppCharacters) {
    OdbcHelper::SetWrappedStmt(&stmt_, &fake_handle_);
    base_driver.text = OUTSIDE_BMP;
    UseFourByteApp();
    std::vector<uint32_t> wide(8, GUARD);
    SQLSMALLINT length = 0;
    const std::vector<uint32_t> expected = {'a', 0x1F600, 'b'};
    constexpr auto WIDE_BYTES = static_cast<SQLSMALLINT>(8 * sizeof(uint32_t));
    constexpr auto WIDE_LENGTH = static_cast<SQLSMALLINT>(3 * sizeof(uint32_t));

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, wide.data(), WIDE_BYTES, &length));
    EXPECT_EQ(expected, CodePoints(wide.data()));
    EXPECT_EQ(WIDE_LENGTH, length);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, nullptr, 0, &length));
    EXPECT_EQ(WIDE_LENGTH, length);

    wide.assign(8, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, &dbc_, 1, SQL_DIAG_MESSAGE_TEXT, wide.data(), WIDE_BYTES,
        &length));
    EXPECT_EQ(expected, CodePoints(wide.data()));
    EXPECT_EQ(WIDE_LENGTH, length);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, &dbc_, 1, SQL_DIAG_MESSAGE_TEXT, nullptr, 0, &length));
    EXPECT_EQ(WIDE_LENGTH, length);

    wide.assign(8, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLColAttribute(&stmt_, 1, SQL_DESC_NAME, wide.data(), WIDE_BYTES, &length, nullptr));
    EXPECT_EQ(expected, CodePoints(wide.data()));
    EXPECT_EQ(WIDE_LENGTH, length);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLColAttribute(&stmt_, 1, SQL_DESC_NAME, nullptr, 0, &length, nullptr));
    EXPECT_EQ(WIDE_LENGTH, length);

    // "😀😀" fits 12 bytes, 3 of the app's characters: 2 code points and the terminator. A 2-byte base driver given
    // 3 of its characters cut it (4 UTF-16 units), and the length was its 8 bytes doubled.
    base_driver.text = "\xF0\x9F\x98\x80\xF0\x9F\x98\x80";
    const std::vector<uint32_t> two_emoji = {0x1F600, 0x1F600};
    constexpr auto TWELVE = static_cast<SQLSMALLINT>(3 * sizeof(uint32_t));
    constexpr auto EIGHT = static_cast<SQLSMALLINT>(2 * sizeof(uint32_t));
    wide.assign(8, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, wide.data(), TWELVE, &length));
    EXPECT_EQ(two_emoji, CodePoints(wide.data()));
    EXPECT_EQ(EIGHT, length);
    wide.assign(8, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, &dbc_, 1, SQL_DIAG_MESSAGE_TEXT, wide.data(), TWELVE,
        &length));
    EXPECT_EQ(two_emoji, CodePoints(wide.data()));
    EXPECT_EQ(EIGHT, length);
    wide.assign(8, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLColAttribute(&stmt_, 1, SQL_DESC_NAME, wide.data(), TWELVE, &length, nullptr));
    EXPECT_EQ(two_emoji, CodePoints(wide.data()));
    EXPECT_EQ(EIGHT, length);
    EXPECT_EQ(5, std::count(wide.begin() + 3, wide.end(), GUARD));

    base_driver.text = OUTSIDE_BMP;
    env_.use_4_bytes_user_app = false;
    UseFourByteDriver();
    const std::vector<uint16_t> expected_utf16 = {'a', 0xD83D, 0xDE00, 'b'};
    constexpr auto NARROW_LENGTH = static_cast<SQLSMALLINT>(4 * sizeof(SQLTCHAR));
    std::vector<SQLTCHAR> narrow(8, 0x5A);
    const auto narrow_bytes = static_cast<SQLSMALLINT>(narrow.size() * sizeof(SQLTCHAR));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, narrow.data(), narrow_bytes, &length));
    EXPECT_EQ(expected_utf16, Utf16Units(narrow.data()));
    EXPECT_EQ(NARROW_LENGTH, length);

    narrow.assign(8, 0x5A);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, &dbc_, 1, SQL_DIAG_MESSAGE_TEXT, narrow.data(),
        narrow_bytes, &length));
    EXPECT_EQ(expected_utf16, Utf16Units(narrow.data()));
    EXPECT_EQ(NARROW_LENGTH, length);

    narrow.assign(8, 0x5A);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLColAttribute(&stmt_, 1, SQL_DESC_NAME, narrow.data(), narrow_bytes, &length, nullptr));
    EXPECT_EQ(expected_utf16, Utf16Units(narrow.data()));
    EXPECT_EQ(NARROW_LENGTH, length);
}

// A string that fits the base driver's characters but not the app's (U+1F600 needs two UTF-16 units) is cut by the
// conversion: SQL_SUCCESS_WITH_INFO, with the wrapper's 01004 for SQLGetInfo and SQLColAttribute and no record for
// SQLGetDiagField, which posts none about itself.
TEST_F(OutputBufferTest, InfoAndColAttributeConversionCutHasA01004Record) {
    OdbcHelper::SetWrappedStmt(&stmt_, &fake_handle_);
    base_driver.text = "a\xF0\x9F\x98\x80";  // 2 code points, 3 UTF-16 units
    UseFourByteDriver();
    std::vector<SQLTCHAR> out(3 + 4, 0x5A);  // 6 bytes: room for 2 units and the terminator, "a"
    const auto out_bytes = static_cast<SQLSMALLINT>(3 * sizeof(SQLTCHAR));
    const std::string needed = std::to_string(4 * sizeof(SQLTCHAR));
    SQLSMALLINT length = 0;

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, out.data(), out_bytes, &length));
    EXPECT_EQ((std::vector<uint16_t>{'a'}), Utf16Units(out.data()));
    EXPECT_EQ(static_cast<SQLSMALLINT>(3 * sizeof(SQLTCHAR)), length);
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ("01004", dbc_.err->sqlstate);
    EXPECT_EQ("SQLGetInfo - The value was truncated: the buffer needs " + needed + " bytes",
        std::string(dbc_.err->error_msg));

    out.assign(3 + 4, 0x5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLColAttribute(&stmt_, 1, SQL_DESC_NAME, out.data(), out_bytes, &length,
        nullptr));
    EXPECT_EQ((std::vector<uint16_t>{'a'}), Utf16Units(out.data()));
    ASSERT_TRUE(stmt_.err);
    EXPECT_EQ("01004", stmt_.err->sqlstate);
    EXPECT_EQ("SQLColAttribute - The value was truncated: the buffer needs " + needed + " bytes",
        std::string(stmt_.err->error_msg));

    dbc_.err.reset();
    out.assign(3 + 4, 0x5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagField(SQL_HANDLE_DBC, &dbc_, 1, SQL_DIAG_MESSAGE_TEXT, out.data(),
        out_bytes, &length));
    EXPECT_EQ((std::vector<uint16_t>{'a'}), Utf16Units(out.data()));
    EXPECT_EQ(static_cast<SQLSMALLINT>(3 * sizeof(SQLTCHAR)), length);
    EXPECT_FALSE(dbc_.err);
}

// The base driver's BufferLength is a whole number of its characters, also at the SQLSMALLINT limit: a 2-byte app's
// 32766 bytes hold 16383 characters, more than a 4-byte base driver's 32767 bytes do, so it gets 32764 (8191 of its
// 4-byte characters). It got 32767, an odd byte count, which the ODBC spec makes HY090 for a Unicode value.
TEST_F(OutputBufferTest, BaseDriverBufferLengthIsWholeCharactersAtTheLimit) {
    base_driver.text = "abc";
    UseFourByteDriver();
    std::vector<SQLTCHAR> out(32766 / sizeof(SQLTCHAR), 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, &dbc_, 1, SQL_DIAG_MESSAGE_TEXT, out.data(), 32766,
        &length));
    EXPECT_EQ(32764, base_driver.buffer_lengths.front());
    EXPECT_EQ("abc", AppText(out.data()));
    base_driver.buffer_lengths.clear();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, out.data(), 32766, &length));
    EXPECT_EQ(32764, base_driver.buffer_lengths.front());
}
#endif

#ifdef UNICODE
// A direct caller with a 4-byte SQLWCHAR (no driver manager) can set a string attribute before its first connect, which
// is where the driver tells such an app apart. With a length, the value is kept as the app's bytes and read in the
// app's characters when it's used: before the connect in SQLGetConnectAttr, and at the connect for the base driver. It
// was copied as 2-byte characters when it was set, which kept "p".
TEST_F(OutputBufferTest, StringAttributeSetBeforeTheAppIsKnownToBeFourByte) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    ASSERT_FALSE(env_.use_4_bytes_user_app);
    std::vector<uint32_t> catalog = {'p', 'o', 's', 't', 'g', 'r', 'e', 's', 0};
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, catalog.data(),
        static_cast<SQLINTEGER>(8 * sizeof(uint32_t))));
    std::fill(catalog.begin(), catalog.end() - 1, 'X');

    UseFourByteApp();  // what the first connect finds out
    std::vector<uint32_t> value(16, GUARD);
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(uint32_t)), &length));
    EXPECT_EQ("postgres", Text(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(8 * sizeof(uint32_t)), length);

    EXPECT_EQ(SQL_SUCCESS, ConnectToBaseDriver());
    EXPECT_EQ("postgres", ReplayedString(SQL_ATTR_CURRENT_CATALOG));
}
#endif

// SQL_ASYNC_DBC_FUNCTIONS and SQL_ASYNC_NOTIFICATION, which the wrapper answers before a connect, are SQLUINTEGERs: all
// 4 bytes are written, and the length is their size. Only 2 were, so an SQLUINTEGER the app hadn't zeroed read
// 0x5A5A0001. SQL_MAX_CONCURRENT_ACTIVITIES stays an SQLUSMALLINT.
TEST_F(OutputBufferTest, GetInfoAsyncInfoTypesAreSqlUintegers) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    for (const SQLUSMALLINT info_type : {SQLUSMALLINT{SQL_ASYNC_DBC_FUNCTIONS}, SQLUSMALLINT{SQL_ASYNC_NOTIFICATION}}) {
        SCOPED_TRACE(info_type);
        SQLUINTEGER value = GUARD;
        SQLSMALLINT length = 0;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, info_type, &value, static_cast<SQLSMALLINT>(sizeof(value)), &length));
        EXPECT_EQ(1U, value);
        EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(SQLUINTEGER)), length);
    }

    struct {
        SQLUSMALLINT value;
        SQLUSMALLINT after;
    } activities{0x5A5A, 0x5A5A};
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_MAX_CONCURRENT_ACTIVITIES, &activities.value,
        static_cast<SQLSMALLINT>(sizeof(activities.value)), &length));
    EXPECT_EQ(0, activities.value);
    EXPECT_EQ(0x5A5A, activities.after);
    EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(SQLUSMALLINT)), length);
}

// A call the wrapper answers itself clears the handle's diagnostics, as every ODBC call does: after its own
// SQL_DRIVER_NAME on a connected handle, SQLGetDiagRec and SQLGetDiagField report no record, without asking the base
// driver, whose record is from an earlier call. The next call the base driver runs makes its record current again.
// Only a direct caller sees this: after an SQL_SUCCESS, unixODBC answers SQL_NO_DATA itself.
TEST_F(OutputBufferTest, WrapperAnsweredGetInfoLeavesNoBaseDriverRecord) {
    base_driver.text = "base driver record";
    std::vector<SQLTCHAR> name(128, 0);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_DRIVER_NAME, name.data(),
        static_cast<SQLSMALLINT>(name.size() * sizeof(SQLTCHAR)), nullptr));

    std::vector<SQLTCHAR> state(2 * MAX_SQL_STATE_LEN, 0);
    std::vector<SQLTCHAR> message(64, 0);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state.data(), nullptr, message.data(),
        static_cast<SQLSMALLINT>(message.size()), &length, false));
    SQLINTEGER number = 77;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, &dbc_, 0, SQL_DIAG_NUMBER, &number, 0, nullptr));
    EXPECT_EQ(0, number);
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagField(SQL_HANDLE_DBC, &dbc_, 1, SQL_DIAG_MESSAGE_TEXT, message.data(),
        static_cast<SQLSMALLINT>(message.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(0, base_driver.calls);

    std::vector<SQLTCHAR> catalog(32, 0);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_TRACEFILE, catalog.data(),
        static_cast<SQLINTEGER>(catalog.size() * sizeof(SQLTCHAR)), nullptr));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state.data(), nullptr, message.data(),
        static_cast<SQLSMALLINT>(message.size()), &length, false));
    EXPECT_EQ("base driver record", AppText(message.data()));
}

// The same for SQLGetEnvAttr, which the wrapper answers from what SQLSetEnvAttr stored.
TEST_F(OutputBufferTest, WrapperAnsweredGetEnvAttrLeavesNoBaseDriverRecord) {
    int fake_env = 0;
    env_.wrapped_env = &fake_env;
    env_.attr_map.insert_or_assign(SQL_ATTR_ODBC_VERSION,
        std::make_pair(reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_OV_ODBC3)), 0));
    base_driver.text = "base driver record";
    SQLUINTEGER version = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetEnvAttr(&env_, SQL_ATTR_ODBC_VERSION, &version, 0, nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_OV_ODBC3), version);

    std::vector<SQLTCHAR> message(64, 0);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagRec(SQL_HANDLE_ENV, &env_, 1, nullptr, nullptr, message.data(),
        static_cast<SQLSMALLINT>(message.size()), &length, false));
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagField(SQL_HANDLE_ENV, &env_, 1, SQL_DIAG_MESSAGE_TEXT, message.data(),
        static_cast<SQLSMALLINT>(message.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(0, base_driver.calls);
    env_.wrapped_env = SQL_NULL_HENV;
}

#ifdef UNICODE
// The same with SQL_NTS. The end of the app's string takes a character size, so it's copied in the one known when it's
// set, 2 bytes: up to the zero upper half of the 'p'. The copy is padded to whole 4-byte characters and a 4-byte
// terminator, and keeps SQL_NTS, so reading it as 4-byte characters, before the connect and at it, gives "p" and stays
// inside it. The copy was 6 bytes with a length of 2: SQLGetConnectAttr read 0 characters, and the connect's 4-byte
// read went past it (a heap over-read under ASan).
TEST_F(OutputBufferTest, NtsStringAttributeSetBeforeTheAppIsKnownToBeFourByteStaysInItsCopy) {
    dbc_.wrapped_dbc = SQL_NULL_HDBC;
    ASSERT_FALSE(env_.use_4_bytes_user_app);
    std::vector<uint32_t> catalog = {'p', 'o', 's', 't', 'g', 'r', 'e', 's', 0};
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, catalog.data(), SQL_NTS));

    UseFourByteApp();  // what the first connect finds out
    std::vector<uint32_t> value(16, GUARD);
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(uint32_t)), &length));
    EXPECT_EQ("p", Text(value.data()));
    EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(uint32_t)), length);

    EXPECT_EQ(SQL_SUCCESS, ConnectToBaseDriver());
    EXPECT_EQ("p", ReplayedString(SQL_ATTR_CURRENT_CATALOG));
}
#endif

// Each descriptor function clears the descriptor's record when it starts, as the other handles' functions do, so the
// wrapper's record from an earlier call (a 01004, an HY090) can't stand in for a later call's. Nothing cleared it, so
// it stayed for the descriptor's lifetime.
TEST_F(OutputBufferTest, DescriptorFunctionsClearTheDescriptorRecord) {
    base_driver.text = "x";
    const auto post_stale_record = [](DESC& desc) {
        desc.err = std::make_unique<ErrInfo>("an earlier call's record", WARN_STRING_DATA_RIGHT_TRUNCATED);
    };
    std::vector<SQLTCHAR> buffer(16, 0);
    const auto buffer_bytes = static_cast<SQLINTEGER>(buffer.size() * sizeof(SQLTCHAR));
    SQLINTEGER length = 0;
    SQLSMALLINT short_length = 0;

    post_stale_record(desc_);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDescField(&desc_, 1, SQL_DESC_NAME, buffer.data(), buffer_bytes, &length));
    EXPECT_FALSE(desc_.err);

    post_stale_record(desc_);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDescRec(&desc_, 1, buffer.data(), static_cast<SQLSMALLINT>(buffer.size()),
        &short_length, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    EXPECT_FALSE(desc_.err);

    post_stale_record(desc_);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(&desc_, 1, SQL_DESC_NAME, buffer.data(), SQL_NTS));
    EXPECT_FALSE(desc_.err);

    post_stale_record(desc_);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescRec(&desc_, 1, SQL_C_CHAR, 0, 0, 0, 0, nullptr, nullptr, nullptr));
    EXPECT_FALSE(desc_.err);

    // SQLCopyDesc posts on its target.
    DESC target{};
    target.dbc = &dbc_;
    target.wrapped_desc = &fake_handle_;
    post_stale_record(target);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLCopyDesc(&desc_, &target));
    EXPECT_FALSE(target.err);
    target.wrapped_desc = SQL_NULL_HDESC;
}

#ifdef UNICODE
// The case that shows it: a 4-byte app's SQLGetDescRec whose name only the conversion cuts gets the wrapper's 01004, as
// intended; a later SQLGetDescField fails in the base driver, and SQLGetDiagRec returns the base driver's record for
// it. It returned the 01004.
TEST_F(OutputBufferTest, DescriptorConversionCutDoesNotMaskALaterError) {
    UseFourByteApp();
    base_driver.text = "a\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80" "b";  // 5 code points, 8 UTF-16 units
    std::vector<uint32_t> name(4 + 4, GUARD);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDescRec(&desc_, 1, reinterpret_cast<SQLTCHAR*>(name.data()), 4, &length,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
    ASSERT_TRUE(desc_.err);
    EXPECT_EQ("01004", desc_.err->sqlstate);

    base_driver.fail = true;
    std::vector<uint32_t> value(8, GUARD);
    SQLINTEGER value_length = 0;
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetDescField(&desc_, 99, SQL_DESC_NAME, value.data(),
        static_cast<SQLINTEGER>(value.size() * sizeof(uint32_t)), &value_length));
    base_driver.fail = false;

    base_driver.text = "base driver error";
    std::vector<uint32_t> message(32, GUARD);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DESC, &desc_, 1, nullptr, nullptr,
        reinterpret_cast<SQLTCHAR*>(message.data()), static_cast<SQLSMALLINT>(message.size()), &length, false));
    EXPECT_EQ("base driver error", Text(message.data()));
}
#endif

// SQLConnect and SQLDriverConnect on a handle that is already connected return SQL_ERROR with 08002 (connection name in
// use), and SQLGetEnvAttr for an attribute the environment doesn't hold says why: HY092 for an identifier that isn't an
// ODBC environment attribute, HY010 for an unset SQL_ATTR_ODBC_VERSION, which has no default. All three returned
// SQL_ERROR without a record, and SQLGetDiagRec then went on to the base driver's record from an earlier call. A driver
// manager answers these itself; a direct caller reaches the wrapper.
TEST_F(OutputBufferTest, WrapperErrorsWithoutARecordNowPostOne) {
    dbc_.conn_status = CONN_CONNECTED;
    std::vector<SQLTCHAR> conn_str = AppString("DSN=output-buffer-test");
    EXPECT_EQ(SQL_ERROR, RDS_SQLDriverConnect(&dbc_, nullptr, conn_str.data(), SQL_NTS, nullptr, 0, nullptr,
        SQL_DRIVER_NOPROMPT));
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ("08002", dbc_.err->sqlstate);

    dbc_.err.reset();
    std::vector<SQLTCHAR> dsn = AppString("output-buffer-test");
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(&dbc_, dsn.data(), SQL_NTS, nullptr, 0, nullptr, 0));
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ("08002", dbc_.err->sqlstate);
    dbc_.conn_status = CONN_NOT_CONNECTED;

    SQLUINTEGER value = 0;
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetEnvAttr(&env_, 30001, &value, 0, nullptr));
    ASSERT_TRUE(env_.err);
    EXPECT_EQ("HY092", env_.err->sqlstate);
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetEnvAttr(&env_, SQL_ATTR_ODBC_VERSION, &value, 0, nullptr));
    ASSERT_TRUE(env_.err);
    EXPECT_EQ("HY010", env_.err->sqlstate);
}

// SQLGetEnvAttr gives the ODBC default of an environment attribute that has one when it isn't set: SQL_TRUE for
// SQL_ATTR_OUTPUT_NTS, SQL_CP_STRICT_MATCH for SQL_ATTR_CP_MATCH and SQL_CP_OFF for SQL_ATTR_CONNECTION_POOLING. It
// returned SQL_ERROR with HYC00, which says the driver doesn't support the attribute.
TEST_F(OutputBufferTest, GetEnvAttrGivesTheOdbcDefaultOfAnUnsetAttribute) {
    struct Case {
        SQLINTEGER attribute;
        SQLUINTEGER expected;
    };
    for (const Case& c : {Case{SQL_ATTR_OUTPUT_NTS, SQL_TRUE}, Case{SQL_ATTR_CP_MATCH, SQL_CP_STRICT_MATCH},
                          Case{SQL_ATTR_CONNECTION_POOLING, SQL_CP_OFF}}) {
        SQLUINTEGER value = 0x5A5A5A5A;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetEnvAttr(&env_, c.attribute, &value, 0, nullptr)) << c.attribute;
        EXPECT_EQ(c.expected, value) << c.attribute;
        EXPECT_FALSE(env_.err) << c.attribute;
    }
}

// SQLAllocHandle writes a connection's base_diag_stale without the connection's lock, and SQLGetInfo, SQLGetDiagRec and
// SQLGetDiagField use it under the lock on another thread: the flag is atomic, so that isn't a data race. It was a plain
// bool.
TEST_F(OutputBufferTest, BaseDiagStaleIsAtomic) {
    EXPECT_TRUE((std::is_same_v<decltype(DBC::base_diag_stale), std::atomic<bool>>));
    EXPECT_TRUE((std::is_same_v<decltype(ENV::base_diag_stale), std::atomic<bool>>));
}

#ifdef UNICODE
// Review P1: on a connected handle in conversion mode, SQLSetConnectAttr reads a string value's StringLength as bytes
// (b0b7d05). The reviewer's case: a 4-byte app's "db" in an 8-byte buffer with nothing after it, StringLength 8.
// The base driver gets "db" in its own characters, and nothing past the 8 bytes is read, here or in the copy
// SQLSetConnectAttr keeps: in a death test's child the 8 bytes end against an unreadable page (without ASan, the bytes
// after a heap block are zero and stop the old read).
TEST_F(OutputBufferTest, ConnectedSetConnectAttrReadsAFourByteAppsByteLength) {
    UseFourByteApp();
#ifndef _WIN32
    {
        const uint32_t db[] = {'d', 'b'};
        const GuardedBuffer guarded(db, sizeof(db));
        ASSERT_NE(nullptr, guarded.Get());
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        EXPECT_EXIT((NoCoreFile(), RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, guarded.Get(),
                                                         static_cast<SQLINTEGER>(sizeof(db))),
                     _exit(0)),
                    testing::ExitedWithCode(0), "");
    }
#endif
    auto value = std::make_unique<uint32_t[]>(2);
    value[0] = 'd';
    value[1] = 'b';
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_CURRENT_CATALOG, value.get(),
        static_cast<SQLINTEGER>(2 * sizeof(uint32_t))));
    EXPECT_EQ("db", ReplayedString(SQL_ATTR_CURRENT_CATALOG));
}
#endif

// SQLGetEnvAttr reads a value stored with an SQL_IS_* length back as an integer, as SQLGetConnectAttr does, and a
// standard environment attribute (they are all integers) as one whatever its length. Such a value went to
// snprintf("%s") as the address of a string: SQL_ATTR_ODBC_VERSION set with SQL_IS_INTEGER, as nanodbc and many samples
// do, crashed (ASan: SEGV). Only a caller without a driver manager reaches this; both answer SQLGetEnvAttr themselves.
TEST_F(OutputBufferTest, GetEnvAttrReadsIntegersStoredWithAnIsLength) {
    const auto as_pointer = [](uintptr_t value) { return reinterpret_cast<SQLPOINTER>(value); };
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetEnvAttr(&env_, SQL_ATTR_ODBC_VERSION, as_pointer(SQL_OV_ODBC3), SQL_IS_INTEGER));
    SQLUINTEGER value = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetEnvAttr(&env_, SQL_ATTR_ODBC_VERSION, &value, 0, nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_OV_ODBC3), value);

    // A standard attribute is an integer whatever length it was set with.
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetEnvAttr(&env_, SQL_ATTR_OUTPUT_NTS, as_pointer(SQL_TRUE), SQL_IS_POINTER));
    value = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetEnvAttr(&env_, SQL_ATTR_OUTPUT_NTS, &value, 0, nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_TRUE), value);

    // An attribute the wrapper doesn't know, stored as an integer: the type its length gives.
    constexpr SQLINTEGER DRIVER_ATTRIBUTE = 30002;
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetEnvAttr(&env_, DRIVER_ATTRIBUTE, as_pointer(7), SQL_IS_UINTEGER));
    value = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetEnvAttr(&env_, DRIVER_ATTRIBUTE, &value, 0, nullptr));
    EXPECT_EQ(7U, value);

    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetEnvAttr(&env_, DRIVER_ATTRIBUTE, as_pointer(5), SQL_IS_SMALLINT));
    struct {
        SQLUSMALLINT value;
        SQLUSMALLINT after;
    } small{0, 0x5A5A};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetEnvAttr(&env_, DRIVER_ATTRIBUTE, &small.value, 0, nullptr));
    EXPECT_EQ(5, small.value);
    EXPECT_EQ(0x5A5A, small.after);

    // A string the app stored with SQL_NTS still comes back as one.
    static const char text[] = "abc";
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetEnvAttr(&env_, DRIVER_ATTRIBUTE, const_cast<char*>(text), SQL_NTS));
    char out[8] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetEnvAttr(&env_, DRIVER_ATTRIBUTE, out, static_cast<SQLINTEGER>(sizeof(out)), nullptr));
    EXPECT_STREQ("abc", out);
}

// SQLAllocHandle for a statement or a descriptor adds it to the connection's list under the connection's lock, as
// SQLFreeHandle removes it, so threads that share one connection, each with its own statements and descriptors, can
// allocate and free them at once. Without the lock this is a data race on the lists, which TSan reports and which can
// corrupt them. Short (4 threads, 100 rounds each) and with a fixed amount of work, for CI; meant to be run under TSan.
TEST_F(OutputBufferTest, ConcurrentAllocAndFreeOnOneConnection) {
    constexpr int THREADS = 4;
    constexpr int ROUNDS = 100;
    std::vector<std::thread> threads;
    threads.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([this] {
            for (int round = 0; round < ROUNDS; ++round) {
                SQLHSTMT stmt = nullptr;
                if (RDS_AllocStmt(&dbc_, &stmt) == SQL_SUCCESS) {
                    RDS_FreeStmt(stmt, SQL_DROP);
                }
                SQLHANDLE desc = nullptr;
                if (RDS_AllocDesc(&dbc_, &desc) == SQL_SUCCESS) {
                    RDS_FreeDesc(desc);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const std::lock_guard<std::recursive_mutex> lock_guard(dbc_.lock);
    EXPECT_TRUE(dbc_.stmt_list.empty());
    EXPECT_TRUE(dbc_.desc_list.empty());
}
