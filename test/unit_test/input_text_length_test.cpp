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

// The text the wrapper reads from the application and hands on: TextLength characters (code points for an app with
// a 4-byte SQLWCHAR), never past them, and nothing more for the base driver or the plugin chain.

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../../driver/driver.h"
#include "../../driver/error.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/base_plugin.h"
#include "../../driver/plugin/default_plugin.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/odbc_dsn_helper.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/rds_lib_loader.h"
#include "../../driver/util/rds_strings.h"
#include "common_mock_objects.h"
#include "guarded_buffer.h"

namespace {
    // One call the fake base driver got: the text it reads, as UTF-8, and the length it was given.
    struct BaseCall {
        std::string function;
        std::string text;
        SQLINTEGER length;
        // For SQLExecute and SQLExecDirect: the bound parameter as the base driver reads it then, and its indicator.
        std::string param_text;
        SQLLEN param_indicator = 0;
    };
    std::vector<BaseCall> g_base_calls;

    // The one parameter the fake base driver has bound, read when it executes.
    struct BoundParam {
        SQLPOINTER value = nullptr;
        SQLLEN buffer_length = 0;
        SQLLEN* indicator = nullptr;
        SQLSMALLINT io_type = SQL_PARAM_INPUT;
    };
    BoundParam g_param;
    // What the fake writes into an output parameter when it executes, in the base driver's width.
    std::string g_param_output;
    // What the fake's last execute returned: SQL_SUCCESS_WITH_INFO when it cut the output parameter (psqlODBC).
    SQLRETURN g_exec_ret = SQL_SUCCESS;
    // A parameter whose data comes at execution, bound with the app's own buffer (its token for SQLParamData), and
    // whether SQLParamData has asked for its data: an execute waits for it, and the SQLParamData after it runs.
    BoundParam g_data_at_exec_param;
    bool g_data_asked = false;

    // The one column the fake base driver has bound, and the value SQLFetch writes into it (in the base driver's width,
    // as psqlODBC: as much as the buffer holds with a NUL, and the whole value's length).
    struct BoundColumn {
        SQLPOINTER target = nullptr;
        SQLLEN buffer_length = 0;
        SQLLEN* indicator = nullptr;
    };
    BoundColumn g_column;
    std::string g_column_value;
    // Whether the fake base driver has a 4-byte SQLWCHAR (Unicode builds).
    bool g_base_four_byte = false;
    int g_base_stmt_stand_in = 0;

    // The text a base driver reads: length characters of its own width, up to a NUL (psqlODBC stops there too), or
    // to the NUL for SQL_NTS.
    std::string BaseText(SQLTCHAR* text, SQLINTEGER length) {
#if UNICODE
        return ConvertUserAppToUTF8(g_base_four_byte, text, length);
#else
        if (text == nullptr) {
            return "";
        }
        const char* chars = reinterpret_cast<const char*>(text);
        return length >= 0 ? std::string(chars, strnlen(chars, static_cast<size_t>(length))) : std::string(chars);
#endif
    }

    SQLINTEGER BaseChars(SQLINTEGER length);

    // Writes a value into a base driver's buffer as psqlODBC does (convert_text_field_to_sql_c): as many of its
    // characters as buffer_length bytes hold with a NUL, and the whole value's length. Returns whether it cut the value.
    template <typename Unit>
    bool WriteBaseValue(const Unit* value, size_t units, SQLPOINTER buffer, SQLLEN buffer_length, SQLLEN* indicator) {
        const size_t room = buffer_length > 0 ? static_cast<size_t>(buffer_length) / sizeof(Unit) : 0;
        const size_t written = room > 0 ? std::min(units, room - 1) : 0;
        if (room > 0) {
            const Unit nul = 0;
            std::memcpy(buffer, value, written * sizeof(Unit));
            std::memcpy(static_cast<char*>(buffer) + (written * sizeof(Unit)), &nul, sizeof(Unit));
        }
        *indicator = static_cast<SQLLEN>(units * sizeof(Unit));
        return written < units;
    }

    // An execute: reads the bound parameter, then writes an output parameter's value.
    BaseCall Executed(const std::string& function, const std::string& text, SQLINTEGER length) {
        BaseCall call{.function = function, .text = text, .length = length};
        g_exec_ret = SQL_SUCCESS;
        if (g_param.value == nullptr) {
            return call;
        }
        call.param_indicator = g_param.indicator != nullptr ? *g_param.indicator : SQL_NTS;
        if (g_param.io_type != SQL_PARAM_OUTPUT && call.param_indicator != SQL_NULL_DATA
            && call.param_indicator != SQL_DEFAULT_PARAM && call.param_indicator != SQL_COLUMN_IGNORE) {
            const SQLINTEGER chars = call.param_indicator == SQL_NTS
                ? SQL_NTS : BaseChars(static_cast<SQLINTEGER>(call.param_indicator));
            call.param_text = BaseText(static_cast<SQLTCHAR*>(g_param.value), chars);
        }
#if UNICODE
        if (g_param.io_type != SQL_PARAM_INPUT && !g_param_output.empty()) {
            bool cut = false;
            if (g_base_four_byte) {
                const std::wstring out = ConvertUTF8ToWString(g_param_output);
                cut = WriteBaseValue(out.data(), out.size(), g_param.value, g_param.buffer_length, g_param.indicator);
            } else {
                const std::vector<uint16_t> out = ConvertUTF8ToUTF16(g_param_output);
                cut = WriteBaseValue(out.data(), out.size() - 1, g_param.value, g_param.buffer_length,
                                     g_param.indicator);
            }
            g_exec_ret = cut ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
        }
#endif
        return call;
    }

    // With a data-at-execution parameter an execute waits for its data: SQL_NEED_DATA.
    bool NeedsData(const std::string& function, const std::string& text, SQLINTEGER length) {
        if (g_data_at_exec_param.value == nullptr) {
            return false;
        }
        g_base_calls.push_back({.function = function, .text = text, .length = length});
        g_data_asked = false;
        return true;
    }

    SQLRETURN FakeSQLExecDirect(SQLHSTMT /*stmt*/, SQLTCHAR* text, SQLINTEGER length) {
        if (NeedsData("SQLExecDirect", BaseText(text, length), length)) {
            return SQL_NEED_DATA;
        }
        g_base_calls.push_back(Executed("SQLExecDirect", BaseText(text, length), length));
        return g_exec_ret;
    }

    SQLRETURN FakeSQLExecute(SQLHSTMT /*stmt*/) {
        if (NeedsData("SQLExecute", "", 0)) {
            return SQL_NEED_DATA;
        }
        g_base_calls.push_back(Executed("SQLExecute", "", 0));
        return g_exec_ret;
    }

    // Asks for the data-at-execution parameter's data once, then runs the statement.
    SQLRETURN FakeSQLParamData(SQLHSTMT /*stmt*/, SQLPOINTER* value) {
        if (!g_data_asked) {
            g_data_asked = true;
            *value = g_data_at_exec_param.value;
            return SQL_NEED_DATA;
        }
        g_base_calls.push_back(Executed("SQLParamData", "", 0));
        return g_exec_ret;
    }

    SQLRETURN FakeSQLPrepare(SQLHSTMT /*stmt*/, SQLTCHAR* text, SQLINTEGER length) {
        g_base_calls.push_back({.function = "SQLPrepare", .text = BaseText(text, length), .length = length});
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSQLNativeSql(SQLHDBC /*dbc*/, SQLTCHAR* text, SQLINTEGER length, SQLTCHAR* /*out*/,
                               SQLINTEGER /*buffer_length*/, SQLINTEGER* /*out_length*/) {
        g_base_calls.push_back({.function = "SQLNativeSql", .text = BaseText(text, length), .length = length});
        return SQL_SUCCESS;
    }

    // Records the table name.
    SQLRETURN FakeSQLTables(SQLHSTMT /*stmt*/, SQLTCHAR* /*catalog*/, SQLSMALLINT /*catalog_length*/,
                            SQLTCHAR* /*schema*/, SQLSMALLINT /*schema_length*/, SQLTCHAR* table,
                            SQLSMALLINT table_length, SQLTCHAR* /*type*/, SQLSMALLINT /*type_length*/) {
        g_base_calls.push_back({.function = "SQLTables", .text = BaseText(table, table_length), .length = table_length});
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSQLSetCursorName(SQLHSTMT /*stmt*/, SQLTCHAR* name, SQLSMALLINT length) {
        g_base_calls.push_back({.function = "SQLSetCursorName", .text = BaseText(name, length), .length = length});
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSQLBindCol(SQLHSTMT /*stmt*/, SQLUSMALLINT /*column*/, SQLSMALLINT /*type*/, SQLPOINTER target,
                             SQLLEN buffer_length, SQLLEN* indicator) {
        g_column = {.target = target, .buffer_length = buffer_length, .indicator = indicator};
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSQLFetch(SQLHSTMT /*stmt*/) {
        bool cut = false;
#if UNICODE
        if (g_base_four_byte) {
            const std::wstring value = ConvertUTF8ToWString(g_column_value);
            cut = WriteBaseValue(value.data(), value.size(), g_column.target, g_column.buffer_length,
                                 g_column.indicator);
        } else {
            const std::vector<uint16_t> value = ConvertUTF8ToUTF16(g_column_value);
            cut = WriteBaseValue(value.data(), value.size() - 1, g_column.target, g_column.buffer_length,
                                 g_column.indicator);
        }
#else
        cut = WriteBaseValue(g_column_value.data(), g_column_value.size(), g_column.target, g_column.buffer_length,
                             g_column.indicator);
#endif
        return cut ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    }

    SQLRETURN FakeSQLAllocHandle(SQLSMALLINT /*type*/, SQLHANDLE /*input*/, SQLHANDLE* output) {
        *output = static_cast<SQLHANDLE>(&g_base_stmt_stand_in);
        return SQL_SUCCESS;
    }

    // String attribute and descriptor field lengths are in bytes.
    SQLINTEGER BaseChars(SQLINTEGER length) {
        const auto char_bytes = static_cast<SQLINTEGER>(g_base_four_byte ? sizeof(SQLTCHAR) * 2 : sizeof(SQLTCHAR));
        return length > 0 ? length / char_bytes : length;
    }

    SQLRETURN FakeSQLSetConnectAttr(SQLHDBC /*dbc*/, SQLINTEGER /*attribute*/, SQLPOINTER value, SQLINTEGER length) {
        const SQLINTEGER chars = BaseChars(length);
        g_base_calls.push_back(
            {.function = "SQLSetConnectAttr", .text = BaseText(static_cast<SQLTCHAR*>(value), chars), .length = length});
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSQLSetDescField(SQLHDESC /*desc*/, SQLSMALLINT /*record*/, SQLSMALLINT /*field*/, SQLPOINTER value,
                                  SQLINTEGER length) {
        const SQLINTEGER chars = BaseChars(length);
        g_base_calls.push_back(
            {.function = "SQLSetDescField", .text = BaseText(static_cast<SQLTCHAR*>(value), chars), .length = length});
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSQLPutData(SQLHSTMT /*stmt*/, SQLPOINTER data, SQLLEN length) {
        const SQLINTEGER chars = length == SQL_NTS ? SQL_NTS : BaseChars(static_cast<SQLINTEGER>(length));
        g_base_calls.push_back({.function = "SQLPutData", .text = BaseText(static_cast<SQLTCHAR*>(data), chars),
                                .length = static_cast<SQLINTEGER>(length)});
        return SQL_SUCCESS;
    }

    // Records the parameter's text as the base driver reads it when it executes.
    SQLRETURN FakeSQLBindParameter(SQLHSTMT /*stmt*/, SQLUSMALLINT /*number*/, SQLSMALLINT io_type,
                                   SQLSMALLINT /*value_type*/, SQLSMALLINT /*param_type*/, SQLULEN /*column_size*/,
                                   SQLSMALLINT /*digits*/, SQLPOINTER value, SQLLEN buffer_length,
                                   SQLLEN* indicator) {
        if (indicator != nullptr && (*indicator == SQL_DATA_AT_EXEC || *indicator <= SQL_LEN_DATA_AT_EXEC(0))) {
            g_data_at_exec_param = {.value = value, .buffer_length = buffer_length, .indicator = indicator,
                                    .io_type = io_type};
            g_base_calls.push_back(
                {.function = "SQLBindParameter", .text = "", .length = static_cast<SQLINTEGER>(*indicator)});
            return SQL_SUCCESS;
        }
        g_param = {.value = value, .buffer_length = buffer_length, .indicator = indicator, .io_type = io_type};
        const SQLLEN length = indicator != nullptr ? *indicator : SQL_NTS;
        const SQLINTEGER chars = length == SQL_NTS ? SQL_NTS : BaseChars(static_cast<SQLINTEGER>(length));
        // A driver reads an output parameter's buffer only to write it.
        const std::string text = io_type == SQL_PARAM_OUTPUT ? "" : BaseText(static_cast<SQLTCHAR*>(value), chars);
        g_base_calls.push_back({.function = "SQLBindParameter", .text = text, .length = static_cast<SQLINTEGER>(length)});
        return SQL_SUCCESS;
    }

    // A base driver with only the calls these tests make.
    class FakeBaseDriver : public RdsLibLoader {
    public:
        // A dummy path, so the base constructor sets up the function cache.
        FakeBaseDriver() : RdsLibLoader("") {}

        RdsPlatform::FuncHandle GetFunction(const std::string& function_name) override {
            if (function_name == RdsFuncNames::ExecDirect) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLExecDirect);
            }
            if (function_name == RdsFuncNames::Execute) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLExecute);
            }
            if (function_name == RdsFuncNames::SetConnectAttr) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLSetConnectAttr);
            }
            if (function_name == RdsFuncNames::SetDescField) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLSetDescField);
            }
            if (function_name == RdsFuncNames::Prepare) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLPrepare);
            }
            if (function_name == RdsFuncNames::NativeSql) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLNativeSql);
            }
            if (function_name == RdsFuncNames::Tables) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLTables);
            }
            if (function_name == RdsFuncNames::SetCursorName) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLSetCursorName);
            }
            if (function_name == RdsFuncNames::AllocHandle) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLAllocHandle);
            }
            if (function_name == RdsFuncNames::BindCol) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLBindCol);
            }
            if (function_name == RdsFuncNames::Fetch) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLFetch);
            }
            if (function_name == RdsFuncNames::PutData) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLPutData);
            }
            if (function_name == RdsFuncNames::ParamData) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLParamData);
            }
            if (function_name == RdsFuncNames::BindParameter) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSQLBindParameter);
            }
            return nullptr;
        }
    };

    // The plugin chain as RDS_SQLExecDirect calls it: records the text, to its NUL, and TextLength.
    class RecordingPlugin : public BasePlugin {
    public:
        SQLRETURN Execute(SQLHSTMT /*StatementHandle*/, SQLTCHAR* StatementText, SQLINTEGER TextLength) override {
            text = BaseText(StatementText, SQL_NTS);
            text_length = TextLength;
            ++calls;
            return SQL_SUCCESS;
        }

        std::string text;
        SQLINTEGER text_length = 0;
        int calls = 0;
    };

    // Text as an app passes it to the RDS_ helpers: UTF-16 in Unicode builds (a 2-byte SQLWCHAR), bytes in ANSI.
    class AppText {
    public:
#if UNICODE
        explicit AppText(const std::string& text) : buffer_(ConvertUTF8ToUTF16(text)) {}
#else
        explicit AppText(const std::string& text) : buffer_(text) {}
#endif
        SQLTCHAR* Get() { return reinterpret_cast<SQLTCHAR*>(buffer_.data()); }

    private:
#if UNICODE
        std::vector<uint16_t> buffer_;
#else
        std::string buffer_;
#endif
    };

#if UNICODE
    // Text from an app with a 4-byte SQLWCHAR (iODBC): UTF-32, two SQLTCHARs per code point.
    SQLTCHAR* FourByte(std::u32string& text) {
        return reinterpret_cast<SQLTCHAR*>(text.data());
    }

    // A 2-byte SQLWCHAR text as UTF-16, padded with NULs, so reading it as 4-byte text (the connect functions do,
    // to detect a 4-byte app) stays inside the buffer.
    std::vector<uint16_t> PaddedTwoByte(const std::string& text) {
        std::vector<uint16_t> buffer = ConvertUTF8ToUTF16(text);
        buffer.resize((buffer.size() * 2) + 2, 0);
        return buffer;
    }
#endif

}  // namespace

// A connected statement and descriptor over the fake base driver, a 2-byte one in Unicode builds.
class InputTextLengthTest : public testing::Test {
protected:
    ENV* env_ = nullptr;
    DBC* dbc_ = nullptr;
    STMT* stmt_ = nullptr;
    DESC* desc_ = nullptr;
    int base_dbc_stand_in_ = 0;
    int base_stmt_stand_in_ = 0;
    int base_desc_stand_in_ = 0;
    std::shared_ptr<FakeBaseDriver> lib_loader_;
    std::shared_ptr<OdbcHelper> odbc_helper_;

    void SetUp() override {
        g_base_calls.clear();
        g_base_four_byte = false;
        g_param = {};
        g_param_output.clear();
        g_exec_ret = SQL_SUCCESS;
        g_data_at_exec_param = {};
        g_data_asked = false;
        g_column = {};
        g_column_value.clear();
        lib_loader_ = std::make_shared<FakeBaseDriver>();
        env_ = new ENV();
        env_->driver_lib_loader = lib_loader_;
        dbc_ = new DBC();
        dbc_->env = env_;
        dbc_->conn_status = CONN_NOT_CONNECTED;
        dbc_->wrapped_dbc = static_cast<SQLHDBC>(&base_dbc_stand_in_);
        odbc_helper_ = std::make_shared<OdbcHelper>(lib_loader_, env_);
        const auto plugin_service = std::make_shared<testing::NiceMock<MockPluginService>>();
        ON_CALL(*plugin_service, GetOdbcHelper()).WillByDefault(testing::Return(odbc_helper_));
        dbc_->plugin_service = plugin_service;
        stmt_ = new STMT();
        stmt_->dbc = dbc_;
        // Its implicit ARD and APD, which hold the copies of the SQL_C_WCHAR bindings.
        stmt_->app_row_desc = new DESC();
        stmt_->app_row_desc->dbc = dbc_;
        stmt_->app_row_desc->stmt = stmt_;
        stmt_->app_param_desc = new DESC();
        stmt_->app_param_desc->dbc = dbc_;
        stmt_->app_param_desc->stmt = stmt_;
        OdbcHelper::SetWrappedStmt(stmt_, static_cast<SQLHSTMT>(&base_stmt_stand_in_));
        desc_ = new DESC();
        desc_->dbc = dbc_;
        desc_->wrapped_desc = static_cast<SQLHDESC>(&base_desc_stand_in_);
    }

    void TearDown() override {
        delete desc_;
        delete stmt_->app_row_desc;
        delete stmt_->app_param_desc;
        delete stmt_;
        delete dbc_;
        delete env_;
    }
};

// ODBC: a TextLength of 0, or below 0 but not SQL_NTS, is HY090 "Invalid string or buffer length". Nothing runs:
// an empty text would have made the base driver execute whatever was prepared.
TEST_F(InputTextLengthTest, ExecDirect_InvalidLength_ReturnsHY090AndRunsNothing) {
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    AppText text("SELECT 1");
    for (const SQLINTEGER length : {0, -5}) {
        EXPECT_EQ(SQL_ERROR, RDS_SQLExecDirect(stmt_, text.Get(), length)) << length;
        ASSERT_NE(nullptr, stmt_->err) << length;
        EXPECT_EQ("HY090", stmt_->err->sqlstate) << length;
    }
    EXPECT_TRUE(g_base_calls.empty());
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, text.Get(), SQL_NTS));
    ASSERT_EQ(1U, g_base_calls.size());
    EXPECT_EQ("SQLExecDirect", g_base_calls[0].function);
    EXPECT_EQ("SELECT 1", g_base_calls[0].text);
}

// An empty text, however it comes, runs nothing: DefaultPlugin takes an empty text for SQLExecute, so it ran the
// statement prepared on the handle again. A null pointer is HY009, an empty text HY090.
TEST_F(InputTextLengthTest, ExecDirect_EmptyText_DoesNotRunThePreparedStatement) {
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    AppText insert("INSERT INTO t VALUES (1)");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLPrepare(stmt_, insert.Get(), SQL_NTS));
    EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt_));

    AppText x("x");
    AppText empty("");
#if UNICODE
    std::vector<uint16_t> nul_first = {0, 'x', 0};
#else
    std::vector<char> nul_first = {0, 'x', 0};
#endif
    struct Case {
        SQLTCHAR* text;
        SQLINTEGER length;
        const char* state;
    };
    for (const Case& c : {Case{x.Get(), 0, "HY090"}, Case{empty.Get(), SQL_NTS, "HY090"},
                          Case{reinterpret_cast<SQLTCHAR*>(nul_first.data()), 2, "HY090"},
                          Case{nullptr, SQL_NTS, "HY009"}}) {
        EXPECT_EQ(SQL_ERROR, RDS_SQLExecDirect(stmt_, c.text, c.length)) << c.state << " " << c.length;
        EXPECT_EQ(c.state, stmt_->err != nullptr ? stmt_->err->sqlstate : "no record") << c.length;
    }
    const auto executes = std::count_if(g_base_calls.begin(), g_base_calls.end(),
                                        [](const BaseCall& call) { return call.function == "SQLExecute"; });
    const auto direct = std::count_if(g_base_calls.begin(), g_base_calls.end(),
                                      [](const BaseCall& call) { return call.function == "SQLExecDirect"; });
    EXPECT_EQ(1, executes);
    EXPECT_EQ(0, direct);
}

#if !UNICODE
namespace {
    // An ANSI app's text in a heap buffer of exactly its bytes, with no NUL: reading past it shows under ASan.
    std::vector<SQLTCHAR> ExactAnsi(const std::string& text) {
        return {text.begin(), text.end()};
    }
}  // namespace

// An ANSI app's lengths count bytes, and its text needn't end in a NUL either. "COMMITX" cut to 6 runs a COMMIT.
TEST_F(InputTextLengthTest, ExecDirect_AnsiApp_RunsTextLengthCharacters) {
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    AppText text("COMMITX");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, text.Get(), 6));
    ASSERT_EQ(1U, g_base_calls.size());
    EXPECT_EQ("SQLExecDirect", g_base_calls[0].function);
    EXPECT_EQ("COMMIT", g_base_calls[0].text);
}

TEST_F(InputTextLengthTest, ExecDirect_AnsiApp_ChainGetsTheTextAndItsLength) {
    RecordingPlugin chain;
    dbc_->plugin_head = &chain;
    AppText text("COMMITX");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, text.Get(), 6));
    EXPECT_EQ("COMMIT", chain.text);
    EXPECT_EQ(6, chain.text_length);
}

// The base driver gets the app's text and length as they are for these, and reads TextLength bytes of it.
TEST_F(InputTextLengthTest, PrepareAndCatalog_AnsiApp_BaseDriverGetsTextLengthCharacters) {
    AppText text("COMMITX");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLPrepare(stmt_, text.Get(), 6));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLTables(stmt_, nullptr, 0, nullptr, 0, text.Get(), 6, nullptr, 0));
    ASSERT_EQ(2U, g_base_calls.size());
    for (const BaseCall& call : g_base_calls) {
        EXPECT_EQ("COMMIT", call.text) << call.function;
        EXPECT_EQ(6, call.length) << call.function;
    }
}

TEST_F(InputTextLengthTest, Connect_AnsiApp_ReadsNoFurtherThanTheLengths) {
#ifndef _WIN32
    {
        const std::string dsn = "no-such-dsn-input-length-test";
        const GuardedBuffer guarded_server(dsn.data(), dsn.size());
        const GuardedBuffer guarded_user("admin", 5);
        const GuardedBuffer guarded_password("secret", 6);
        ASSERT_NE(nullptr, guarded_server.Get());
        // Each name ends against an unreadable page: a read past one faults in the child.
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        EXPECT_EXIT((NoCoreFile(), RDS_SQLConnect(dbc_, guarded_server.Get(), static_cast<SQLSMALLINT>(dsn.size()),
                                    guarded_user.Get(), 5, guarded_password.Get(), 6),
                     _exit(0)),
                    testing::ExitedWithCode(0), "");
        dbc_->conn_attr.clear();
    }
#endif
    std::vector<SQLTCHAR> server = ExactAnsi("no-such-dsn-input-length-test");
    std::vector<SQLTCHAR> user = ExactAnsi("admin");
    std::vector<SQLTCHAR> password = ExactAnsi("secret");
    // No base driver to load: the connect fails after reading the arguments.
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, server.data(), static_cast<SQLSMALLINT>(server.size()), user.data(), 5,
                                        password.data(), 6));
    EXPECT_EQ("no-such-dsn-input-length-test", dbc_->conn_attr[KEY_DSN]);
    EXPECT_EQ("admin", dbc_->conn_attr[KEY_DB_USERNAME]);
    EXPECT_EQ("secret", dbc_->conn_attr[KEY_DB_PASSWORD]);
}

TEST_F(InputTextLengthTest, SetCursorName_AnsiApp_ReadsNoFurtherThanTheLength) {
    OdbcHelper::SetWrappedStmt(stmt_, SQL_NULL_HSTMT);
#ifndef _WIN32
    {
        const GuardedBuffer guarded_cursor("cursor1", 7);
        ASSERT_NE(nullptr, guarded_cursor.Get());
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        EXPECT_EXIT((NoCoreFile(), RDS_SQLSetCursorName(stmt_, guarded_cursor.Get(), 7), _exit(0)),
                    testing::ExitedWithCode(0), "");
    }
#endif
    std::vector<SQLTCHAR> cursor = ExactAnsi("cursor1");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetCursorName(stmt_, cursor.data(), 7));
    EXPECT_EQ("cursor1", stmt_->cursor_name);
}

// iODBC sets its SQL_ATTR_APP_WCHAR_TYPE (1061) on an ANSI driver too. The ANSI driver takes it and keeps nothing for
// the base driver: it kept it and set it on psqlODBC at the connect, which refuses it ("Unknown connect option").
TEST_F(InputTextLengthTest, AppWcharType_AnsiDriver_TakesItAndKeepsNothing) {
    constexpr SQLINTEGER IODBC_APP_WCHAR_TYPE = 1061;
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(dbc_, IODBC_APP_WCHAR_TYPE, reinterpret_cast<SQLPOINTER>(3), 0));
    EXPECT_FALSE(dbc_->attr_map.contains(IODBC_APP_WCHAR_TYPE));
    // Connected: not set on the base driver either.
    dbc_->wrapped_dbc = static_cast<SQLHDBC>(&base_dbc_stand_in_);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(dbc_, IODBC_APP_WCHAR_TYPE, reinterpret_cast<SQLPOINTER>(1), 0));
    EXPECT_FALSE(dbc_->attr_map.contains(IODBC_APP_WCHAR_TYPE));
    EXPECT_TRUE(g_base_calls.empty());
}

TEST_F(InputTextLengthTest, DriverConnect_AnsiApp_ParsesStringLength1Characters) {
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
    const std::string text = "DSN=no-such-dsn-input-length-test;UID=adminX";
    AppText app_text(text);
    EXPECT_EQ(SQL_ERROR, RDS_SQLDriverConnect(dbc_, nullptr, app_text.Get(), static_cast<SQLSMALLINT>(text.size() - 1),
                                              nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ("admin", dbc_->conn_attr[KEY_DB_USERNAME]);
}
#endif

// An empty user name or password isn't a value to give: it stays absent, so the DSN's UID and PWD (or the plugin's
// default user) apply. A NameLength of 0 is the same as an empty text.
TEST_F(InputTextLengthTest, Connect_EmptyUserAndPassword_AreNotGiven) {
    AppText server("no-such-dsn-input-length-test");
    AppText empty("");
    AppText admin("admin");
    AppText secret("secret");

    // No base driver to load: the connect fails after reading the arguments.
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, server.Get(), SQL_NTS, empty.Get(), SQL_NTS, empty.Get(), SQL_NTS));
    EXPECT_FALSE(dbc_->conn_attr.contains(KEY_DB_USERNAME));
    EXPECT_FALSE(dbc_->conn_attr.contains(KEY_DB_PASSWORD));

    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, server.Get(), SQL_NTS, admin.Get(), 0, secret.Get(), 0));
    EXPECT_FALSE(dbc_->conn_attr.contains(KEY_DB_USERNAME));
    EXPECT_FALSE(dbc_->conn_attr.contains(KEY_DB_PASSWORD));

    // A value still overrides.
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, server.Get(), SQL_NTS, admin.Get(), SQL_NTS, secret.Get(), SQL_NTS));
    EXPECT_EQ("admin", dbc_->conn_attr[KEY_DB_USERNAME]);
    EXPECT_EQ("secret", dbc_->conn_attr[KEY_DB_PASSWORD]);
}

// The DSN's UID and PWD (test/unit_test/resources/odbc.ini) stay when SQLConnect's are empty, and a given one wins.
TEST_F(InputTextLengthTest, Connect_EmptyUser_KeepsTheDsnUid) {
    if (OdbcDsnHelper::Load("dsn-with-uid", "UID").empty()) {
        GTEST_SKIP() << "the driver manager doesn't read the odbc.ini fixture";
    }
    AppText server("dsn-with-uid");
    AppText empty("");
    AppText bob("bob");

    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, server.Get(), SQL_NTS, empty.Get(), SQL_NTS, empty.Get(), SQL_NTS));
    EXPECT_EQ("app_user", dbc_->conn_attr[KEY_DB_USERNAME]);
    EXPECT_EQ("dsn_password", dbc_->conn_attr[KEY_DB_PASSWORD]);

    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, server.Get(), SQL_NTS, bob.Get(), SQL_NTS, empty.Get(), SQL_NTS));
    EXPECT_EQ("bob", dbc_->conn_attr[KEY_DB_USERNAME]);
    EXPECT_EQ("dsn_password", dbc_->conn_attr[KEY_DB_PASSWORD]);

#if UNICODE
    // An app with a 4-byte SQLWCHAR.
    env_->use_4_bytes_user_app = true;
    std::u32string server_32 = U"dsn-with-uid";
    std::u32string empty_32 = U"";
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, FourByte(server_32), SQL_NTS, FourByte(empty_32), SQL_NTS,
                                        FourByte(empty_32), SQL_NTS));
    EXPECT_EQ("app_user", dbc_->conn_attr[KEY_DB_USERNAME]);
    EXPECT_EQ("dsn_password", dbc_->conn_attr[KEY_DB_PASSWORD]);
#endif
}

#if UNICODE
// "COMMITX" cut to 6 runs a COMMIT. The X right after the 6 characters is a sentinel the wrapper mustn't reach.
TEST_F(InputTextLengthTest, ExecDirect_FourByteApp_RunsTextLengthCharacters) {
    env_->use_4_bytes_user_app = true;
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    std::u32string text = U"COMMITX";
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, FourByte(text), 6));
    ASSERT_EQ(1U, g_base_calls.size());
    EXPECT_EQ("SQLExecDirect", g_base_calls[0].function);
    EXPECT_EQ("COMMIT", g_base_calls[0].text);
}

// The plugin chain gets the converted UTF-16 text and its length in UTF-16 units, not the app's code points.
TEST_F(InputTextLengthTest, ExecDirect_FourByteApp_ChainGetsTheTextAndItsUtf16Length) {
    env_->use_4_bytes_user_app = true;
    RecordingPlugin chain;
    dbc_->plugin_head = &chain;
    std::u32string text = U"\U0001F600\U0001F600abc";
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, FourByte(text), 2));
    EXPECT_EQ("\xF0\x9F\x98\x80\xF0\x9F\x98\x80", chain.text);
    EXPECT_EQ(4, chain.text_length);
}

// SQLConnect's names keep all NameLength characters: they were cut to NameLength UTF-8 bytes.
TEST_F(InputTextLengthTest, Connect_KeepsTextLengthCharactersOfANonAsciiName) {
    for (const bool four_byte_app : {false, true}) {
        env_->use_4_bytes_user_app = four_byte_app;
        dbc_->conn_attr.clear();
        std::u32string server_32 = U"no-such-dsn-input-length-test";
        std::u32string user_32 = U"J\u00FCrgenX";
        std::u32string password_32 = U"p\u00E4sswordX";
        std::vector<uint16_t> server_16 = PaddedTwoByte("no-such-dsn-input-length-test");
        std::vector<uint16_t> user_16 = PaddedTwoByte("J\xC3\xBCrgenX");
        std::vector<uint16_t> password_16 = PaddedTwoByte("p\xC3\xA4sswordX");
        SQLTCHAR* server = four_byte_app ? FourByte(server_32) : reinterpret_cast<SQLTCHAR*>(server_16.data());
        SQLTCHAR* user = four_byte_app ? FourByte(user_32) : reinterpret_cast<SQLTCHAR*>(user_16.data());
        SQLTCHAR* password = four_byte_app ? FourByte(password_32) : reinterpret_cast<SQLTCHAR*>(password_16.data());

        // No base driver to load: the connect fails after reading the arguments.
        EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, server, SQL_NTS, user, 6, password, 8)) << four_byte_app;
        EXPECT_EQ("J\xC3\xBCrgen", dbc_->conn_attr[KEY_DB_USERNAME]) << four_byte_app;
        EXPECT_EQ("p\xC3\xA4ssword", dbc_->conn_attr[KEY_DB_PASSWORD]) << four_byte_app;
    }
    EXPECT_EQ("no-such-dsn-input-length-test", dbc_->conn_attr[KEY_DSN]);
}

// A length of 0 is no connection string: it no longer makes the wrapper read the whole text as an app's 4-byte text.
TEST_F(InputTextLengthTest, DriverConnect_ZeroLength_ReadsNoText) {
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
    std::u32string text = U"DSN=no-such-dsn-input-length-test";
    EXPECT_EQ(SQL_ERROR, RDS_SQLDriverConnect(dbc_, nullptr, FourByte(text), 0, nullptr, 0, nullptr,
                                              SQL_DRIVER_NOPROMPT));
    EXPECT_FALSE(env_->use_4_bytes_user_app.load());
    EXPECT_FALSE(dbc_->conn_attr.contains(KEY_DSN));
}

TEST_F(InputTextLengthTest, SetCursorName_KeepsTextLengthCharactersOfANonAsciiName) {
    OdbcHelper::SetWrappedStmt(stmt_, SQL_NULL_HSTMT);
    env_->use_4_bytes_user_app = true;
    std::u32string name_32 = U"c\u00FCrsorX";
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetCursorName(stmt_, FourByte(name_32), 6));
    EXPECT_EQ("c\xC3\xBCrsor", stmt_->cursor_name);
    env_->use_4_bytes_user_app = false;
    AppText name_16("c\xC3\xBCrsorX");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetCursorName(stmt_, name_16.Get(), 6));
    EXPECT_EQ("c\xC3\xBCrsor", stmt_->cursor_name);
}

// A string attribute's or descriptor field's length is in bytes: 8 bytes of a 4-byte app's text are 2 characters.
TEST_F(InputTextLengthTest, SetConnectAttrAndDescField_FourByteApp_ReadStringLengthBytes) {
    env_->use_4_bytes_user_app = true;
    std::u32string value = U"dbX";
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(dbc_, SQL_ATTR_CURRENT_CATALOG, FourByte(value), 8));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(desc_, 1, SQL_DESC_NAME, FourByte(value), 8));
    ASSERT_EQ(2U, g_base_calls.size());
    EXPECT_EQ("SQLSetConnectAttr", g_base_calls[0].function);
    EXPECT_EQ("db", g_base_calls[0].text);
    EXPECT_EQ("SQLSetDescField", g_base_calls[1].function);
    EXPECT_EQ("db", g_base_calls[1].text);
}

// psqlODBC counts TextLength in UTF-16 units and a 4-byte app in code points, so it was given too few for text
// outside the BMP. With the converted text, which ends in a NUL, it gets SQL_NTS and reads all of it.
TEST_F(InputTextLengthTest, Prepare_FourByteApp_BaseDriverGetsTheWholeTextOutsideTheBmp) {
    env_->use_4_bytes_user_app = true;
    std::u32string text = U"\U0001F600\U0001F600abc";
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLPrepare(stmt_, FourByte(text), 2));
    ASSERT_EQ(1U, g_base_calls.size());
    EXPECT_EQ("SQLPrepare", g_base_calls[0].function);
    EXPECT_EQ("\xF0\x9F\x98\x80\xF0\x9F\x98\x80", g_base_calls[0].text);
}

TEST_F(InputTextLengthTest, NativeSqlCatalogAndCursorName_FourByteApp_BaseDriverGetsTheWholeTextOutsideTheBmp) {
    env_->use_4_bytes_user_app = true;
    std::u32string text = U"\U0001F600\U0001F600abc";
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLNativeSql(dbc_, FourByte(text), 2, nullptr, 0, nullptr));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLTables(stmt_, nullptr, 0, nullptr, 0, FourByte(text), 2, nullptr, 0));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetCursorName(stmt_, FourByte(text), 2));
    ASSERT_EQ(3U, g_base_calls.size());
    for (const BaseCall& call : g_base_calls) {
        EXPECT_EQ("\xF0\x9F\x98\x80\xF0\x9F\x98\x80", call.text) << call.function;
    }
}

// A string value's length counts the app's bytes, and a 4-byte base driver needs twice as many for the same text.
TEST_F(InputTextLengthTest, SetConnectAttrAndDescField_FourByteBaseDriver_GetsTheWholeValue) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    AppText value("dbX");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(dbc_, SQL_ATTR_CURRENT_CATALOG, value.Get(), 4));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetDescField(desc_, 1, SQL_DESC_NAME, value.Get(), 4));
    ASSERT_EQ(2U, g_base_calls.size());
    EXPECT_EQ("db", g_base_calls[0].text);
    EXPECT_EQ("db", g_base_calls[1].text);
}

// The cursor name the wrapper keeps goes to the base driver with its length in UTF-16 units, not UTF-8 bytes.
TEST_F(InputTextLengthTest, ExecDirect_SetsTheCursorNameWithItsUtf16Length) {
    OdbcHelper::SetWrappedStmt(stmt_, SQL_NULL_HSTMT);
    stmt_->cursor_name = "c\xC3\xBCrsor";
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    AppText text("SELECT 1");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, text.Get(), SQL_NTS));
    ASSERT_EQ(2U, g_base_calls.size());
    EXPECT_EQ("SQLSetCursorName", g_base_calls[0].function);
    EXPECT_EQ("c\xC3\xBCrsor", g_base_calls[0].text);
    EXPECT_EQ(6, g_base_calls[0].length);
    EXPECT_EQ("SQLExecDirect", g_base_calls[1].function);
}
#endif

#if UNICODE && !defined(_WIN32)
namespace {
    // A 4-byte app's text in a heap buffer of exactly its code points, with no NUL: reading past it shows under ASan.
    std::vector<char32_t> ExactFourByte(const std::u32string& text) {
        return {text.begin(), text.end()};
    }
}  // namespace

// SQLPutData's StrLen_or_Ind counts bytes: 12 bytes of a 4-byte app's text are 3 characters. The X after them is a
// sentinel it mustn't send; the exact-size buffer has nothing after them to read.
TEST_F(InputTextLengthTest, PutData_FourByteApp_SendsStrLenOrIndBytesOnly) {
    env_->use_4_bytes_user_app = true;
    stmt_->put_data_char_conversion = true;
    std::u32string with_sentinel = U"abcX";
    std::vector<char32_t> exact = ExactFourByte(U"abc");
    EXPECT_EQ(SQL_SUCCESS, SQLPutData(stmt_, with_sentinel.data(), 12));
    EXPECT_EQ(SQL_SUCCESS, SQLPutData(stmt_, exact.data(), 12));
    ASSERT_EQ(2U, g_base_calls.size());
    for (const BaseCall& call : g_base_calls) {
        EXPECT_EQ("SQLPutData", call.function);
        EXPECT_EQ("abc", call.text);
    }
}

// A 2-byte app's data for a 4-byte base driver: 6 bytes are 3 characters.
TEST_F(InputTextLengthTest, PutData_FourByteBaseDriver_SendsStrLenOrIndBytesOnly) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    stmt_->put_data_char_conversion = true;
    std::u16string with_sentinel = u"abcX";
    EXPECT_EQ(SQL_SUCCESS, SQLPutData(stmt_, with_sentinel.data(), 6));
    ASSERT_EQ(1U, g_base_calls.size());
    EXPECT_EQ("abc", g_base_calls[0].text);
}

// A bound parameter's indicator counts bytes too: 8 bytes of a 4-byte app's text are two characters outside the BMP,
// which the base driver gets whole, and the wrapper reads none past them.
TEST_F(InputTextLengthTest, BoundParameter_FourByteApp_BaseDriverGetsTheIndicatorsCharacters) {
    env_->use_4_bytes_user_app = true;
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    std::vector<char32_t> value = ExactFourByte(U"\U0001F600\U0001F600");
    SQLLEN indicator = 8;
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, value.data(), 8,
                                            &indicator));
    EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt_));
    ASSERT_EQ(3U, g_base_calls.size());
    EXPECT_EQ("SQLBindParameter", g_base_calls[1].function);
    EXPECT_EQ("\xF0\x9F\x98\x80\xF0\x9F\x98\x80", g_base_calls[1].text);
    EXPECT_EQ("SQLExecute", g_base_calls[2].function);
}
#endif

#if UNICODE
namespace {
    // A 2-byte app's text in a heap buffer of exactly its UTF-16 units, with its NUL or without one.
    std::vector<uint16_t> ExactTwoByte(const std::string& utf8, bool with_nul) {
        std::vector<uint16_t> units = ConvertUTF8ToUTF16(utf8);
        if (!with_nul) {
            units.pop_back();
        }
        units.shrink_to_fit();
        return units;
    }
}  // namespace

// While the environment doesn't know the app's SQLWCHAR, SQLConnect also reads the name as an app's 4-byte text. For a
// 2-byte app's name that read stays inside the name (ASan shows a read past it), and the 2-byte name is kept when it
// finds no DSN either.
TEST_F(InputTextLengthTest, Connect_TwoByteApp_FourByteCheckReadsNoFurtherThanTheName) {
    for (const bool nts : {false, true}) {
        dbc_->conn_attr.clear();
        std::vector<uint16_t> server = ExactTwoByte("no-such-dsn", nts);
        EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, reinterpret_cast<SQLTCHAR*>(server.data()), nts ? SQL_NTS : 11,
                                            nullptr, 0, nullptr, 0)) << nts;
        EXPECT_FALSE(env_->use_4_bytes_user_app.load()) << nts;
        EXPECT_EQ("no-such-dsn", dbc_->conn_attr[KEY_DSN]) << nts;
    }
}
#endif

#if UNICODE && !defined(_WIN32)
// SQLDriverConnect's check, the same way: a 2-byte app's string is read no further, and its own keys stay.
TEST_F(InputTextLengthTest, DriverConnect_TwoByteApp_FourByteCheckReadsNoFurtherAndKeepsTheKeys) {
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
    for (const bool nts : {false, true}) {
        dbc_->conn_attr.clear();
        std::vector<uint16_t> text = ExactTwoByte("SERVER=host;UID=bob", nts);
        EXPECT_EQ(SQL_ERROR, RDS_SQLDriverConnect(dbc_, nullptr, reinterpret_cast<SQLTCHAR*>(text.data()),
                                                  nts ? SQL_NTS : 19, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT)) << nts;
        EXPECT_FALSE(env_->use_4_bytes_user_app.load()) << nts;
        EXPECT_EQ("host", dbc_->conn_attr[KEY_SERVER]) << nts;
        EXPECT_EQ("bob", dbc_->conn_attr[KEY_DB_USERNAME]) << nts;
    }
}

// Under unixODBC the connect functions don't check for a 4-byte app: its SQLWCHAR is 2 bytes, and a 4-byte app's text
// can't get through it. A 2-byte app's text of one character with SQL_NTS, which can't be told from the start of a
// 4-byte text, is read no further than its NUL (here against an unreadable page), and its 2-byte reading stays. The
// check read it as 4-byte text and on past it.
TEST_F(InputTextLengthTest, ConnectAndDriverConnect_UnderUnixOdbc_OneCharacterTextIsReadNoFurther) {
    dbc_->connect_through_unix_odbc = true;  // as SQLConnectW and SQLDriverConnectW find from their caller
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
    const char16_t text[] = u"A";  // the character and its NUL, 4 bytes
    const GuardedBuffer guarded(text, sizeof(text));
    ASSERT_NE(nullptr, guarded.Get());
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT((NoCoreFile(), RDS_SQLConnect(dbc_, guarded.Get(), SQL_NTS, nullptr, 0, nullptr, 0), _exit(0)),
                testing::ExitedWithCode(0), "");
    EXPECT_EXIT((NoCoreFile(), RDS_SQLDriverConnect(dbc_, nullptr, guarded.Get(), SQL_NTS, nullptr, 0, nullptr,
                                                    SQL_DRIVER_NOPROMPT), _exit(0)),
                testing::ExitedWithCode(0), "");

    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, guarded.Get(), SQL_NTS, nullptr, 0, nullptr, 0));
    EXPECT_FALSE(env_->use_4_bytes_user_app.load());
    EXPECT_EQ("A", dbc_->conn_attr[KEY_DSN]);
}

// SQLConnectW and SQLDriverConnectW tell a connect through unixODBC by the library their caller's code is in: unixODBC's
// libodbc (or its cursor library), not an app's code. Here the caller is unixODBC's own SQLConnectW, in a child process
// that loads libodbc, so that the other tests don't have it loaded.
TEST_F(InputTextLengthTest, ConnectCaller_UnixOdbcIsToldByItsLibrary) {
    EXPECT_FALSE(RDS_IsInUnixOdbc(nullptr));
    EXPECT_FALSE(RDS_IsInUnixOdbc(reinterpret_cast<const void*>(&RDS_SQLConnect)));
    RDS_NoteConnectCaller(dbc_, reinterpret_cast<const void*>(&RDS_SQLConnect));
    EXPECT_FALSE(dbc_->connect_through_unix_odbc);
#ifdef __APPLE__
    constexpr const char* UNIX_ODBC = "libodbc.2.dylib";
#else
    constexpr const char* UNIX_ODBC = "libodbc.so.2";
#endif
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Exits 0 also where libodbc isn't installed: nothing to check then.
    EXPECT_EXIT(
        {
            void* library = dlopen(UNIX_ODBC, RTLD_NOW | RTLD_LOCAL);
            void* connect = library != nullptr ? dlsym(library, "SQLConnectW") : nullptr;
            if (connect != nullptr) {
                RDS_NoteConnectCaller(dbc_, connect);
            }
            _exit(connect == nullptr || (RDS_IsInUnixOdbc(connect) && dbc_->connect_through_unix_odbc) ? 0 : 1);
        },
        testing::ExitedWithCode(0), "");
}

// An app with a 4-byte SQLWCHAR is still recognized, with SQL_NTS and with an explicit length.
TEST_F(InputTextLengthTest, DriverConnect_FourByteApp_StillRecognized) {
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
    for (const bool nts : {false, true}) {
        env_->use_4_bytes_user_app = false;
        dbc_->conn_attr.clear();
        std::u32string text = U"DSN=no-such-dsn-input-length-test;UID=bob";
        const auto length = static_cast<SQLSMALLINT>(nts ? SQL_NTS : text.size());
        EXPECT_EQ(SQL_ERROR, RDS_SQLDriverConnect(dbc_, nullptr, FourByte(text), length, nullptr, 0, nullptr,
                                                  SQL_DRIVER_NOPROMPT)) << nts;
        EXPECT_TRUE(env_->use_4_bytes_user_app.load()) << nts;
        EXPECT_EQ("bob", dbc_->conn_attr[KEY_DB_USERNAME]) << nts;
    }
}
#endif

#if UNICODE
namespace {
    // iODBC's SQL_ATTR_APP_WCHAR_TYPE and its values (iodbcext.h, 3.52.14 and later). iODBC sets it on the driver's
    // connection in each SQLConnect or SQLDriverConnect, before calling the driver's, to the app's SQLWCHAR type.
    constexpr SQLINTEGER IODBC_APP_WCHAR_TYPE = 1061;
    constexpr SQLULEN IODBC_CP_UTF16 = 1;
    constexpr SQLULEN IODBC_CP_UTF8 = 2;
    constexpr SQLULEN IODBC_CP_UCS4 = 3;

    // iODBC's call before the driver's SQLConnect or SQLDriverConnect.
    SQLRETURN SetAppWcharType(DBC* dbc, SQLULEN type) {
        return RDS_SQLSetConnectAttr(dbc, IODBC_APP_WCHAR_TYPE, reinterpret_cast<SQLPOINTER>(type), 0);
    }
}  // namespace

// With iODBC saying UCS-4, the app's text is read as 4-byte text from the start, without the check: a DSN that isn't
// in odbc.ini (which the check needs to find) still works, and so do the user name and SQLDriverConnect's keys.
TEST_F(InputTextLengthTest, AppWcharTypeUcs4_ReadsTheAppsTextAsFourByte) {
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
    EXPECT_EQ(SQL_SUCCESS, SetAppWcharType(dbc_, IODBC_CP_UCS4));
    EXPECT_TRUE(env_->use_4_bytes_user_app.load());
    EXPECT_FALSE(dbc_->attr_map.contains(IODBC_APP_WCHAR_TYPE));

    std::u32string server = U"no-such-dsn-input-length-test";
    std::u32string user = U"bob";
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, FourByte(server), SQL_NTS, FourByte(user), SQL_NTS, nullptr, 0));
    EXPECT_EQ("no-such-dsn-input-length-test", dbc_->conn_attr[KEY_DSN]);
    EXPECT_EQ("bob", dbc_->conn_attr[KEY_DB_USERNAME]);

    dbc_->conn_attr.clear();
    std::u32string text = U"SERVER=host;UID=bob";
    EXPECT_EQ(SQL_ERROR, RDS_SQLDriverConnect(dbc_, nullptr, FourByte(text), SQL_NTS, nullptr, 0, nullptr,
                                              SQL_DRIVER_NOPROMPT));
    EXPECT_EQ("host", dbc_->conn_attr[KEY_SERVER]);
    EXPECT_EQ("bob", dbc_->conn_attr[KEY_DB_USERNAME]);
}

// With iODBC saying UTF-16, the app's text is never read as 4-byte text: a 1-character name, which the check can't
// tell from 4-byte text, isn't read past (ASan shows it).
TEST_F(InputTextLengthTest, AppWcharTypeUtf16_NeverReadsTheAppsTextAsFourByte) {
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
    EXPECT_EQ(SQL_SUCCESS, SetAppWcharType(dbc_, IODBC_CP_UTF16));
    EXPECT_FALSE(env_->use_4_bytes_user_app.load());
    EXPECT_FALSE(dbc_->attr_map.contains(IODBC_APP_WCHAR_TYPE));

#ifndef _WIN32
    {
        const std::vector<uint16_t> dsn = ConvertUTF8ToUTF16("D");
        const std::vector<uint16_t> conn_str = ConvertUTF8ToUTF16("X");
        const GuardedBuffer guarded_dsn(dsn.data(), dsn.size() * sizeof(uint16_t));
        const GuardedBuffer guarded_conn_str(conn_str.data(), conn_str.size() * sizeof(uint16_t));
        ASSERT_NE(nullptr, guarded_dsn.Get());
        // "D" and "X" and their NULs end against an unreadable page: reading them as 4-byte text faults in the child.
        GTEST_FLAG_SET(death_test_style, "threadsafe");
        EXPECT_EXIT((NoCoreFile(), RDS_SQLConnect(dbc_, guarded_dsn.Get(), SQL_NTS, nullptr, 0, nullptr, 0),
                     RDS_SQLDriverConnect(dbc_, nullptr, guarded_conn_str.Get(), SQL_NTS, nullptr, 0, nullptr,
                                          SQL_DRIVER_NOPROMPT),
                     _exit(0)),
                    testing::ExitedWithCode(0), "");
    }
#endif

    std::vector<uint16_t> server = ConvertUTF8ToUTF16("D");
    server.shrink_to_fit();
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(dbc_, reinterpret_cast<SQLTCHAR*>(server.data()), SQL_NTS, nullptr, 0,
                                        nullptr, 0));
    EXPECT_FALSE(env_->use_4_bytes_user_app.load());
    EXPECT_EQ("D", dbc_->conn_attr[KEY_DSN]);

    std::vector<uint16_t> text = ConvertUTF8ToUTF16("X");
    text.shrink_to_fit();
    EXPECT_EQ(SQL_ERROR, RDS_SQLDriverConnect(dbc_, nullptr, reinterpret_cast<SQLTCHAR*>(text.data()), SQL_NTS,
                                              nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_FALSE(env_->use_4_bytes_user_app.load());
}

// The wrapper doesn't take an app's UTF-8 SQLWCHAR (HY024): iODBC then converts the app's text itself.
TEST_F(InputTextLengthTest, AppWcharTypeUtf8_IsRefused) {
    EXPECT_EQ(SQL_ERROR, SetAppWcharType(dbc_, IODBC_CP_UTF8));
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("HY024", dbc_->err->sqlstate);
    EXPECT_FALSE(env_->use_4_bytes_user_app.load());
    EXPECT_FALSE(dbc_->attr_map.contains(IODBC_APP_WCHAR_TYPE));
}
#endif

#if UNICODE && !defined(_WIN32)
// A NULL indicator means the text ends in a NUL (ODBC; psqlODBC reads it so). It was taken as a length of 0.
TEST_F(InputTextLengthTest, BoundParameter_FourByteApp_NullIndicatorMeansNts) {
    env_->use_4_bytes_user_app = true;
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    std::u32string value = U"abc";
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, value.data(), 16,
                                            nullptr));
    EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt_));
    ASSERT_EQ("SQLExecute", g_base_calls.back().function);
    EXPECT_EQ("abc", g_base_calls.back().param_text);
}

// A BufferLength of 0, common for an input parameter: the binding is converted too. The base driver got the app's
// UTF-32 buffer and read it as UTF-16.
TEST_F(InputTextLengthTest, BoundParameter_FourByteApp_ZeroBufferLengthIsConverted) {
    env_->use_4_bytes_user_app = true;
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    std::u32string value = U"abc";
    SQLLEN indicator = SQL_NTS;
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, value.data(), 0,
                                            &indicator));
    EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt_));
    ASSERT_EQ("SQLExecute", g_base_calls.back().function);
    EXPECT_EQ("abc", g_base_calls.back().param_text);
}

// SQL_DEFAULT_PARAM and SQL_COLUMN_IGNORE reach the base driver as they are, and the buffer isn't read: it was read
// to a NUL and sent as text.
TEST_F(InputTextLengthTest, BoundParameter_FourByteApp_DefaultAndIgnoreGoThrough) {
    env_->use_4_bytes_user_app = true;
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    std::u32string value = U"abc";
    for (const SQLLEN special : {static_cast<SQLLEN>(SQL_DEFAULT_PARAM), static_cast<SQLLEN>(SQL_COLUMN_IGNORE)}) {
        SQLLEN indicator = special;
        EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, value.data(),
                                                16, &indicator));
        EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt_));
        ASSERT_EQ("SQLExecute", g_base_calls.back().function);
        EXPECT_EQ(special, g_base_calls.back().param_indicator);
        EXPECT_EQ("", g_base_calls.back().param_text);
    }
}

// A 2-byte app's text for a 4-byte base driver, with a NULL indicator.
TEST_F(InputTextLengthTest, BoundParameter_FourByteBaseDriver_NullIndicatorMeansNts) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    std::u16string value = u"abc";
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, value.data(), 8,
                                            nullptr));
    EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt_));
    ASSERT_EQ("SQLExecute", g_base_calls.back().function);
    EXPECT_EQ("abc", g_base_calls.back().param_text);
}
#endif

#if UNICODE && !defined(_WIN32)
// SQLExecDirect gets the bound parameters converted too: they were for SQLExecute only, so the base driver read the
// wrapper's empty copy.
TEST_F(InputTextLengthTest, BoundParameter_FourByteApp_ConvertedForExecDirect) {
    env_->use_4_bytes_user_app = true;
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    std::u32string value = U"abc";
    SQLLEN indicator = 12;
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, value.data(), 16,
                                            &indicator));
    std::u32string text = U"INSERT INTO t VALUES (?)";
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, FourByte(text), SQL_NTS));
    ASSERT_EQ("SQLExecDirect", g_base_calls.back().function);
    EXPECT_EQ("abc", g_base_calls.back().param_text);
}

// And an output parameter comes back in the app's characters after SQLExecDirect, as after SQLExecute.
TEST_F(InputTextLengthTest, BoundParameter_FourByteApp_OutputCopiedBackAfterExecDirect) {
    env_->use_4_bytes_user_app = true;
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    g_param_output = "out";
    std::vector<char32_t> out(16, U'Z');
    SQLLEN indicator = 0;
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, out.data(),
                                            static_cast<SQLLEN>(out.size() * sizeof(char32_t)), &indicator));
    std::u32string text = U"{call p(?)}";
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, FourByte(text), SQL_NTS));
    EXPECT_EQ(U"out", std::u32string(out.begin(), std::find(out.begin(), out.end(), U'\0')));
    EXPECT_EQ(12, indicator);
}
#endif

#if UNICODE && !defined(_WIN32)
namespace {
    // A 4-byte app's text in its buffer, to its NUL or the buffer's end.
    std::u32string AppTextIn(const std::vector<char32_t>& buffer) {
        return {buffer.begin(), std::find(buffer.begin(), buffer.end(), U'\0')};
    }
}  // namespace

// A bound column fills a 4-byte app's whole buffer, as SQLGetData does: 7 characters and a NUL in 32 bytes. It was
// cut at about half the buffer (ConvertUTF16ToUTF32 got the room in code points where it takes SQLTCHARs).
TEST_F(InputTextLengthTest, BoundColumn_FourByteApp_FillsTheWholeBuffer) {
    env_->use_4_bytes_user_app = true;
    g_column_value = "abcdefg";
    std::vector<char32_t> target(8, U'Z');
    SQLLEN length = 0;
    EXPECT_EQ(SQL_SUCCESS, SQLBindCol(stmt_, 1, SQL_C_WCHAR, target.data(), 32, &length));
    EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt_));
    EXPECT_EQ(U"abcdefg", AppTextIn(target));
    EXPECT_EQ(28, length);
}

// Its length is in the app's bytes, 4 per code point: "a" and U+1F600 are 8 bytes, not 3 UTF-16 units times 4.
TEST_F(InputTextLengthTest, BoundColumn_FourByteApp_LengthCountsCodePoints) {
    env_->use_4_bytes_user_app = true;
    g_column_value = "a\xF0\x9F\x98\x80";
    std::vector<char32_t> target(8, U'Z');
    SQLLEN length = 0;
    EXPECT_EQ(SQL_SUCCESS, SQLBindCol(stmt_, 1, SQL_C_WCHAR, target.data(), 32, &length));
    EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt_));
    EXPECT_EQ(U"a\U0001F600", AppTextIn(target));
    EXPECT_EQ(8, length);
}

// A value the app's buffer can't hold is cut to whole characters with a NUL, gets its whole length, and SQLFetch
// says so with 01004: the base driver's own buffer (the wrapper's copy, 32 bytes of UTF-16) held it, so it reported
// nothing.
TEST_F(InputTextLengthTest, BoundColumn_FourByteApp_CutValueReports01004) {
    env_->use_4_bytes_user_app = true;
    g_column_value = "abcdef";
    std::vector<char32_t> target(4, U'Z');
    SQLLEN length = 0;
    EXPECT_EQ(SQL_SUCCESS, SQLBindCol(stmt_, 1, SQL_C_WCHAR, target.data(), 16, &length));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt_));
    EXPECT_EQ(U"abc", AppTextIn(target));
    EXPECT_EQ(24, length);
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("01004", stmt_->err->sqlstate);
}
#endif

#if UNICODE && !defined(_WIN32)
// An output parameter comes back the same way as a bound column: the 4-byte app's whole buffer, its length in the
// app's bytes (4 per code point), and 01004 when the app's buffer cut it.
TEST_F(InputTextLengthTest, BoundParameter_FourByteApp_OutputUsesTheWholeBufferAndCountsCodePoints) {
    env_->use_4_bytes_user_app = true;
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    struct Case {
        const char* value;
        SQLLEN buffer_bytes;
        const char32_t* expected;
        SQLLEN expected_length;
        SQLRETURN expected_ret;
    };
    for (const Case& c : {Case{"abcdefg", 32, U"abcdefg", 28, SQL_SUCCESS},
                          Case{"a\xF0\x9F\x98\x80", 32, U"a\U0001F600", 8, SQL_SUCCESS},
                          Case{"abcdef", 16, U"abc", 24, SQL_SUCCESS_WITH_INFO}}) {
        g_param_output = c.value;
        std::vector<char32_t> out(static_cast<size_t>(c.buffer_bytes) / sizeof(char32_t), U'Z');
        SQLLEN indicator = 0;
        EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, out.data(),
                                                c.buffer_bytes, &indicator));
        EXPECT_EQ(c.expected_ret, SQLExecute(stmt_)) << c.value;
        EXPECT_EQ(std::u32string(c.expected), AppTextIn(out)) << c.value;
        EXPECT_EQ(c.expected_length, indicator) << c.value;
        if (c.expected_ret == SQL_SUCCESS_WITH_INFO) {
            EXPECT_EQ("01004", stmt_->err != nullptr ? stmt_->err->sqlstate : "no record");
        }
    }
}
#endif

#if UNICODE && !defined(_WIN32)
// An output parameter with a BufferLength of 0 (for its length only) from a 2-byte app over a 4-byte base driver: the
// wrapper's copy holds one of the base driver's characters and a NUL, and the copy back reads no further than the copy.
// The copy was one SQLTCHAR, given to the base driver as 2 bytes, and the copy back read past it (ASan). Nothing goes
// into the app's buffer of 0 bytes, which here starts at an unreadable page. (A 4-byte app's SQLWCHAR is the base
// driver's: no copy, the base driver gets its buffer.)
TEST_F(InputTextLengthTest, BoundParameter_FourByteBaseDriver_OutputWithZeroBufferLengthStaysInTheCopy) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    g_param_output = "abc";
    env_->use_4_bytes_user_app = false;
    const GuardedBuffer out(nullptr, 0);
    ASSERT_NE(nullptr, out.Get());
    SQLLEN indicator = 0;
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, out.Get(), 0,
                                            &indicator));
    // One of the base driver's 4-byte characters and its NUL.
    EXPECT_GE(g_param.buffer_length, static_cast<SQLLEN>(2 * 2 * sizeof(SQLTCHAR)));
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT((NoCoreFile(), SQLExecute(stmt_), _exit(0)), testing::ExitedWithCode(0), "");
    EXPECT_TRUE(SQL_SUCCEEDED(SQLExecute(stmt_)));
}
#endif

#if UNICODE && !defined(_WIN32)
namespace {
    // A 2-byte app's text in its buffer, to its NUL or the buffer's end.
    std::u16string TwoByteAppTextIn(const std::vector<char16_t>& buffer) {
        return {buffer.begin(), std::find(buffer.begin(), buffer.end(), u'\0')};
    }

    std::string StateOf(const STMT* stmt) {
        return stmt->err != nullptr ? stmt->err->sqlstate : std::string("no record");
    }
}  // namespace

// A 2-byte app over a 4-byte base driver: an output parameter the wrapper cuts for the app's buffer is
// SQL_SUCCESS_WITH_INFO with 01004 and its whole length, as a 4-byte app's is. The base driver held all 10 characters
// in the wrapper's copy and returned SQL_SUCCESS; the app's 16 bytes hold 7 and a NUL. Also for SQL_PARAM_INPUT_OUTPUT
// with a NULL input, and after SQLExecDirect.
TEST_F(InputTextLengthTest, BoundParameter_TwoByteAppFourByteBase_OutputTheWrapperCutsReports01004) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    g_param_output = "abcdefghij";
    for (const SQLSMALLINT io_type : {SQL_PARAM_OUTPUT, SQL_PARAM_INPUT_OUTPUT}) {
        for (const bool direct : {false, true}) {
            std::vector<char16_t> out(8, u'Z');
            SQLLEN indicator = SQL_NULL_DATA;
            ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, io_type, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, out.data(), 16,
                                                    &indicator));
            AppText text("{call p(?)}");
            const SQLRETURN ret = direct ? RDS_SQLExecDirect(stmt_, text.Get(), SQL_NTS) : SQLExecute(stmt_);
            EXPECT_EQ(SQL_SUCCESS, g_exec_ret) << io_type << direct;
            EXPECT_EQ(SQL_SUCCESS_WITH_INFO, ret) << io_type << direct;
            EXPECT_EQ("01004", StateOf(stmt_)) << io_type << direct;
            EXPECT_EQ(u"abcdefg", TwoByteAppTextIn(out)) << io_type << direct;
            EXPECT_EQ(20, indicator) << io_type << direct;
        }
    }
}

// The cut keeps a surrogate pair whole: 7 of the UTF-16 units of "abcdef", U+1F600 and "ghij" would end in half of it.
TEST_F(InputTextLengthTest, BoundParameter_TwoByteAppFourByteBase_CutKeepsASurrogatePairWhole) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    g_param_output = "abcdef\xF0\x9F\x98\x80" "ghij";
    std::vector<char16_t> out(8, u'Z');
    SQLLEN indicator = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, out.data(), 16,
                                            &indicator));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLExecute(stmt_));
    EXPECT_EQ(u"abcdef", TwoByteAppTextIn(out));
    EXPECT_EQ(24, indicator);
}

// A value the base driver cut itself gets its whole length from the base driver's: the wrapper's copy for a
// BufferLength of 2 holds one 4-byte character, and "abcdef" is 12 bytes for the app, not the 2 of the character it
// got. Characters the wrapper never saw count as one UTF-16 unit each, exact for text in the BMP.
TEST_F(InputTextLengthTest, BoundParameter_TwoByteAppFourByteBase_ValueTheBaseCutGetsItsWholeLength) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    g_param_output = "abcdef";
    std::vector<char16_t> out(1, u'Z');
    SQLLEN indicator = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, out.data(), 2,
                                            &indicator));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLExecute(stmt_));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, g_exec_ret) << "the base driver cut it";
    EXPECT_EQ(u'\0', out[0]);
    EXPECT_EQ(12, indicator);
}

// And a bound column's: the base driver's buffer for the app's 4 bytes cut "abcdef".
TEST_F(InputTextLengthTest, BoundColumn_TwoByteAppFourByteBase_ValueTheBaseCutGetsItsWholeLength) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    g_column_value = "abcdef";
    std::vector<char16_t> target(2, u'Z');
    SQLLEN length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt_, 1, SQL_C_WCHAR, target.data(), 4, &length));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt_));
    EXPECT_EQ(12, length);
}
#endif

#if UNICODE && !defined(_WIN32)
// Text outside the BMP from a 4-byte base driver comes back whole, for either app: its UTF-32 becomes UTF-16 in the
// wrapper's copy, which has room for it. The conversion had room for only as many UTF-16 units as the code points the
// base driver wrote, and dropped a character outside the BMP that didn't fit: U+1F600 U+1F600 "b" came back as
// U+1F600 "b".
TEST_F(InputTextLengthTest, BoundParameter_FourByteBase_OutputOutsideTheBmpComesBackWhole) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    struct Case {
        const char* value;
        SQLLEN two_byte_length;
        SQLLEN four_byte_length;
    };
    for (const bool four_byte_app : {false, true}) {
        env_->use_4_bytes_user_app = four_byte_app;
        for (const Case& c : {Case{"\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80", 12, 12},
                              Case{"\xF0\x9F\x98\x80\xF0\x9F\x98\x80" "b", 10, 12}}) {
            g_param_output = c.value;
            std::vector<char32_t> out(8, U'Z');
            SQLLEN indicator = 0;
            ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0,
                                                    out.data(), 32, &indicator));
            EXPECT_EQ(SQL_SUCCESS, SQLExecute(stmt_)) << four_byte_app << " " << c.value;
            EXPECT_EQ(c.value, ConvertUserAppToUTF8(four_byte_app, reinterpret_cast<SQLTCHAR*>(out.data()), SQL_NTS))
                << four_byte_app;
            EXPECT_EQ(four_byte_app ? c.four_byte_length : c.two_byte_length, indicator) << four_byte_app << " "
                                                                                          << c.value;
        }
    }
}

// The whole length of a value the base driver cut counts the characters outside the BMP that the copy holds as two
// UTF-16 units: U+1F600 "bc" is 8 bytes for a 2-byte app (the copy for a BufferLength of 2 holds the U+1F600).
TEST_F(InputTextLengthTest, BoundParameter_TwoByteAppFourByteBase_CutLengthCountsTheCharactersOutsideTheBmp) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    g_param_output = "\xF0\x9F\x98\x80" "bc";
    std::vector<char16_t> out(1, u'Z');
    SQLLEN indicator = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0, out.data(), 2,
                                            &indicator));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLExecute(stmt_));
    EXPECT_EQ(u'\0', out[0]);
    EXPECT_EQ(8, indicator);
}

// A bound column the same way.
TEST_F(InputTextLengthTest, BoundColumn_FourByteBase_ValueOutsideTheBmpComesBackWhole) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    g_column_value = "\xF0\x9F\x98\x80\xF0\x9F\x98\x80" "b";
    for (const bool four_byte_app : {false, true}) {
        env_->use_4_bytes_user_app = four_byte_app;
        std::vector<char32_t> target(8, U'Z');
        SQLLEN length = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt_, 1, SQL_C_WCHAR, target.data(), 32, &length));
        EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt_)) << four_byte_app;
        EXPECT_EQ(g_column_value,
                  ConvertUserAppToUTF8(four_byte_app, reinterpret_cast<SQLTCHAR*>(target.data()), SQL_NTS))
            << four_byte_app;
        EXPECT_EQ(four_byte_app ? 12 : 10, length) << four_byte_app;
    }
}
#endif

#if UNICODE && !defined(_WIN32)
// SQL_PARAM_INPUT_OUTPUT from a 2-byte app over a 4-byte base driver: the rebind before each execute gives the base
// driver the copy and its size, as SQLBindParameter does, not the app's BufferLength. With the app's 16 bytes the base
// driver had room for 3 of its characters and a NUL, so the app got "abc" where its buffer holds 7 characters. (A
// 4-byte app's SQLWCHAR is the base driver's: no copy, and the base driver's own cut comes back.)
TEST_F(InputTextLengthTest, BoundParameter_FourByteBase_InputOutputRebindsWithTheCopysSize) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    g_param_output = "abcdefghij";
    struct Case {
        bool four_byte_app;
        const char* expected;
        SQLLEN expected_length;
    };
    for (const Case& c : {Case{false, "abcdefg", 20}}) {
        env_->use_4_bytes_user_app = c.four_byte_app;
        // "x" in the app's width, in 16 bytes.
        std::vector<char32_t> out(4, 0);
        if (c.four_byte_app) {
            out[0] = U'x';
        } else {
            const char16_t x[] = u"x";
            std::memcpy(out.data(), x, sizeof(x));
        }
        SQLLEN indicator = SQL_NTS;
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_INPUT_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0,
                                                out.data(), 16, &indicator));
        EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLExecute(stmt_)) << c.four_byte_app;
        ASSERT_EQ(1U, stmt_->app_param_desc->bound_param_buffers.size());
        EXPECT_EQ(static_cast<SQLLEN>(stmt_->app_param_desc->bound_param_buffers[0].local_buf.size() * sizeof(SQLTCHAR)),
                  g_param.buffer_length) << c.four_byte_app;
        EXPECT_EQ("x", g_base_calls.back().param_text) << c.four_byte_app;
        EXPECT_EQ(SQL_SUCCESS, g_exec_ret) << "the base driver held it: " << c.four_byte_app;
        EXPECT_EQ("01004", StateOf(stmt_)) << c.four_byte_app;
        EXPECT_EQ(c.expected,
                  ConvertUserAppToUTF8(c.four_byte_app, reinterpret_cast<SQLTCHAR*>(out.data()), SQL_NTS))
            << c.four_byte_app;
        EXPECT_EQ(c.expected_length, indicator) << c.four_byte_app;
    }
}
#endif

#if UNICODE && !defined(_WIN32)
// An output parameter comes back after the SQLParamData that runs the statement, when another parameter's data comes
// at execution: SQLExecute and SQLExecDirect return SQL_NEED_DATA then, and they were the only ones that copied it
// back. The data-at-execution parameter itself is bound with the app's own buffer, which the copy back leaves alone.
// A 4-byte app over a 2-byte base driver, and a 2-byte app over a 4-byte one.
TEST_F(InputTextLengthTest, BoundParameter_OutputCopiedBackAfterDataAtExecution) {
    DefaultPlugin chain(dbc_);
    dbc_->plugin_head = &chain;
    g_param_output = "out";
    for (const bool four_byte_app : {true, false}) {
        env_->use_4_bytes_user_app = four_byte_app;
        g_base_four_byte = !four_byte_app;
        odbc_helper_->SetUse4BytesBaseDriver(!four_byte_app);
        for (const bool direct : {false, true}) {
            std::vector<char32_t> token(4, U'T');
            SQLLEN token_indicator = SQL_DATA_AT_EXEC;
            ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 1, SQL_PARAM_INPUT_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0,
                                                    token.data(), 16, &token_indicator));
            std::vector<char32_t> out(8, 0);
            SQLLEN indicator = 0;
            ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt_, 2, SQL_PARAM_OUTPUT, SQL_C_WCHAR, SQL_VARCHAR, 10, 0,
                                                    out.data(), 32, &indicator));
            SQLRETURN ret = SQL_ERROR;
            std::u32string four_byte_text = U"{call p(?, ?)}";
            AppText two_byte_text("{call p(?, ?)}");
            if (!direct) {
                ret = SQLExecute(stmt_);
            } else {
                ret = RDS_SQLExecDirect(stmt_, four_byte_app ? FourByte(four_byte_text) : two_byte_text.Get(), SQL_NTS);
            }
            EXPECT_EQ(SQL_NEED_DATA, ret) << four_byte_app << direct;
            SQLPOINTER asked = nullptr;
            EXPECT_EQ(SQL_NEED_DATA, SQLParamData(stmt_, &asked)) << four_byte_app << direct;
            EXPECT_EQ(static_cast<SQLPOINTER>(token.data()), asked) << four_byte_app << direct;
            std::u32string four_byte_in = U"in";
            std::u16string two_byte_in = u"in";
            EXPECT_EQ(SQL_SUCCESS, four_byte_app ? SQLPutData(stmt_, four_byte_in.data(), 8)
                                                 : SQLPutData(stmt_, two_byte_in.data(), 4));
            EXPECT_EQ("in", g_base_calls.back().text) << four_byte_app << direct;
            EXPECT_EQ(SQL_SUCCESS, SQLParamData(stmt_, &asked)) << four_byte_app << direct;
            EXPECT_EQ("out", ConvertUserAppToUTF8(four_byte_app, reinterpret_cast<SQLTCHAR*>(out.data()), SQL_NTS))
                << four_byte_app << direct;
            EXPECT_EQ(four_byte_app ? 12 : 6, indicator) << four_byte_app << direct;
            EXPECT_EQ(SQL_DATA_AT_EXEC, token_indicator) << four_byte_app << direct;
            EXPECT_EQ(std::vector<char32_t>(4, U'T'), token) << four_byte_app << direct;
        }
    }
}
#endif

// SQLSetPos and SQLBulkOperations start their own data-at-execution columns: the SQLParamData after them is no
// execute's, also when an execute's SQL_NEED_DATA was left behind (an SQLCancel ends it), and copies nothing back.
TEST_F(InputTextLengthTest, DataAtExecution_SetPosAndBulkOperationsAreNoExecutesData) {
    for (const bool set_pos : {true, false}) {
        stmt_->execute_needs_data = true;
        if (set_pos) {
            SQLSetPos(stmt_, 1, SQL_UPDATE, SQL_LOCK_NO_CHANGE);
        } else {
            SQLBulkOperations(stmt_, SQL_ADD);
        }
        EXPECT_FALSE(stmt_->execute_needs_data) << set_pos;
    }
}

#if UNICODE && !defined(_WIN32)
// A bound column with a copy is bound with the copy's size, as a parameter is. With the app's BufferLength a 4-byte
// base driver had room for 3 characters and a NUL in 16 bytes, where a 2-byte app's 16 bytes hold 7: "abcdefg" came
// back as "abc" with SQL_SUCCESS_WITH_INFO. A longer value is the wrapper's cut, with 01004 and its whole length.
TEST_F(InputTextLengthTest, BoundColumn_TwoByteAppFourByteBase_BoundWithTheCopysSize) {
    g_base_four_byte = true;
    odbc_helper_->SetUse4BytesBaseDriver(true);
    struct Case {
        const char* value;
        SQLRETURN expected_ret;
        SQLLEN expected_length;
    };
    for (const Case& c : {Case{"abcdefg", SQL_SUCCESS, 14}, Case{"abcdefghij", SQL_SUCCESS_WITH_INFO, 20}}) {
        g_column_value = c.value;
        std::vector<char16_t> target(8, u'Z');
        SQLLEN length = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt_, 1, SQL_C_WCHAR, target.data(), 16, &length));
        ASSERT_EQ(1U, stmt_->app_row_desc->bound_col_buffers.size());
        EXPECT_EQ(static_cast<SQLLEN>(stmt_->app_row_desc->bound_col_buffers[0].local_buf.size() * sizeof(SQLTCHAR)),
                  g_column.buffer_length) << c.value;
        EXPECT_EQ(c.expected_ret, SQLFetch(stmt_)) << c.value;
        EXPECT_EQ(u"abcdefg", TwoByteAppTextIn(target)) << c.value;
        EXPECT_EQ(c.expected_length, length) << c.value;
        if (c.expected_ret == SQL_SUCCESS_WITH_INFO) {
            EXPECT_EQ("01004", StateOf(stmt_)) << c.value;
        }
    }
}
#endif
