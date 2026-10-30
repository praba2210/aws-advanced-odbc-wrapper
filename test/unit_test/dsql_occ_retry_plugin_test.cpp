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

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../../driver/driver.h"
#include "../../driver/error.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/base_plugin.h"
#include "../../driver/util/attribute_validator.h"
#include "../../driver/util/connection_string_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/logger_wrapper.h"
#include "../../driver/util/occ_retry_config.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/rds_lib_loader.h"
#include "../../driver/util/rds_strings.h"
#include "common_mock_objects.h"

// The OCC options are the wrapper's: they never reach psqlODBC's connection string.
TEST(DsqlOccRetrySetupTest, OccOptions_AreWrapperKeys) {
    for (const char* key : {KEY_ENABLE_OCC_RETRY, KEY_OCC_MAX_RETRIES, KEY_OCC_BASE_DELAY_MS, KEY_OCC_MAX_DELAY_MS,
                            KEY_OCC_JITTER_FACTOR}) {
        EXPECT_TRUE(ConnectionStringHelper::IsAwsOdbcKey(key)) << key;
    }
    const std::map<std::string, std::string> conn_attr = {
        {KEY_SERVER, "host"}, {KEY_ENABLE_OCC_RETRY, "1"}, {KEY_OCC_MAX_RETRIES, "5"}};
    EXPECT_EQ("SERVER=host", ConnectionStringHelper::BuildMinimumConnectionString(conn_attr));
}

TEST(DsqlOccRetrySetupTest, Statement_StartsWithNoPreparedText) {
    const STMT stmt{};
    EXPECT_TRUE(stmt.prepared_text.empty());
}

// Chain-order tests walk the chain from its head.
TEST(DsqlOccRetrySetupTest, BasePlugin_ShowsTheNextPluginAndItsName) {
    const auto bottom = std::make_shared<BasePlugin>(nullptr);
    const BasePlugin top(nullptr, bottom);
    EXPECT_EQ(bottom.get(), top.GetNextPlugin());
    EXPECT_EQ(nullptr, bottom->GetNextPlugin());
    EXPECT_EQ("BasePlugin", top.GetPluginName());
}

// The Aurora DSQL connection lifetime option is the wrapper's too, and a new connection handle has no lifetime yet.
TEST(DsqlConnectionLifetimeSetupTest, MaxConnectionLifetime_IsAWrapperKey) {
    EXPECT_TRUE(ConnectionStringHelper::IsAwsOdbcKey(KEY_MAX_CONNECTION_LIFETIME));
    const std::map<std::string, std::string> conn_attr = {{KEY_SERVER, "host"}, {KEY_MAX_CONNECTION_LIFETIME, "600"}};
    EXPECT_EQ("SERVER=host", ConnectionStringHelper::BuildMinimumConnectionString(conn_attr));
}

TEST(DsqlConnectionLifetimeSetupTest, Connection_StartsWithoutALifetime) {
    const DBC dbc{};
    EXPECT_TRUE(dbc.dsql_lifetime == nullptr);
}

TEST(DsqlOccRetryConfigTest, Parse_Defaults) {
    std::string error;
    const std::optional<OccRetryConfig> config = OccRetryConfig::Parse({}, error);
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->enabled);
    EXPECT_EQ(3, config->max_retries);
    EXPECT_EQ(100, config->base_delay_ms);
    EXPECT_EQ(5000, config->max_delay_ms);
    EXPECT_DOUBLE_EQ(0.25, config->jitter_factor);
    EXPECT_TRUE(error.empty());
}

TEST(DsqlOccRetryConfigTest, Parse_ReadsEveryOption) {
    std::string error;
    const std::optional<OccRetryConfig> config = OccRetryConfig::Parse({
        {KEY_ENABLE_OCC_RETRY, "1"}, {KEY_OCC_MAX_RETRIES, "10"}, {KEY_OCC_BASE_DELAY_MS, "1"},
        {KEY_OCC_MAX_DELAY_MS, "100"}, {KEY_OCC_JITTER_FACTOR, "0.5"}}, error);
    ASSERT_TRUE(config.has_value()) << error;
    EXPECT_TRUE(config->enabled);
    EXPECT_EQ(10, config->max_retries);
    EXPECT_EQ(1, config->base_delay_ms);
    EXPECT_EQ(100, config->max_delay_ms);
    EXPECT_DOUBLE_EQ(0.5, config->jitter_factor);
}

TEST(DsqlOccRetryConfigTest, Parse_AcceptsRangeEnds) {
    const std::vector<std::map<std::string, std::string>> cases = {
        {{KEY_ENABLE_OCC_RETRY, "0"}},
        {{KEY_OCC_MAX_RETRIES, "0"}},
        {{KEY_OCC_MAX_RETRIES, "100"}},
        {{KEY_OCC_MAX_RETRIES, " +7 "}},
        {{KEY_OCC_BASE_DELAY_MS, "1"}},
        {{KEY_OCC_BASE_DELAY_MS, "250"}, {KEY_OCC_MAX_DELAY_MS, "250"}},
        {{KEY_OCC_BASE_DELAY_MS, "60000"}, {KEY_OCC_MAX_DELAY_MS, "60000"}},
        {{KEY_OCC_JITTER_FACTOR, "0"}},
        {{KEY_OCC_JITTER_FACTOR, "1.0"}},
        {{KEY_OCC_JITTER_FACTOR, " 0.5 "}},
    };
    for (const auto& conn_attr : cases) {
        std::string error;
        EXPECT_TRUE(OccRetryConfig::Parse(conn_attr, error).has_value()) << error;
    }
}

TEST(DsqlOccRetryConfigTest, Parse_RejectsBadValuesWithWhatToUse) {
    const std::vector<std::pair<std::map<std::string, std::string>, std::string>> cases = {
        {{{KEY_ENABLE_OCC_RETRY, "yes"}}, "ENABLE_OCC_RETRY=yes (use 0 or 1)"},
        {{{KEY_OCC_MAX_RETRIES, "101"}}, "OCC_MAX_RETRIES=101 (use an integer from 0 to 100)"},
        {{{KEY_OCC_MAX_RETRIES, "-1"}}, "OCC_MAX_RETRIES=-1 (use an integer from 0 to 100)"},
        {{{KEY_OCC_MAX_RETRIES, "three"}}, "OCC_MAX_RETRIES=three (use an integer from 0 to 100)"},
        {{{KEY_OCC_BASE_DELAY_MS, "0"}}, "OCC_BASE_DELAY_MS=0 (use an integer from 1 to 60000)"},
        {{{KEY_OCC_BASE_DELAY_MS, "1.5"}}, "OCC_BASE_DELAY_MS=1.5 (use an integer from 1 to 60000)"},
        {{{KEY_OCC_BASE_DELAY_MS, "60001"}}, "OCC_BASE_DELAY_MS=60001 (use an integer from 1 to 60000)"},
        {{{KEY_OCC_MAX_DELAY_MS, "50"}},
         "OCC_MAX_DELAY_MS=50 (use an integer from OCC_BASE_DELAY_MS, 100, to 60000)"},
        {{{KEY_OCC_MAX_DELAY_MS, "soon"}},
         "OCC_MAX_DELAY_MS=soon (use an integer from OCC_BASE_DELAY_MS, 100, to 60000)"},
        {{{KEY_OCC_MAX_DELAY_MS, "60001"}},
         "OCC_MAX_DELAY_MS=60001 (use an integer from OCC_BASE_DELAY_MS, 100, to 60000)"},
        // The 5000 ms default cap is below this base, so the cap has to be set too: the entry says it's the default.
        {{{KEY_OCC_BASE_DELAY_MS, "10000"}},
         "OCC_MAX_DELAY_MS=5000 (the default; set it to an integer from OCC_BASE_DELAY_MS, 10000, to 60000)"},
        {{{KEY_OCC_JITTER_FACTOR, "1.5"}}, "OCC_JITTER_FACTOR=1.5 (use a number from 0.0 to 1.0)"},
        {{{KEY_OCC_JITTER_FACTOR, "-0.1"}}, "OCC_JITTER_FACTOR=-0.1 (use a number from 0.0 to 1.0)"},
        // A decimal comma is never read, whatever locale the application set.
        {{{KEY_OCC_JITTER_FACTOR, "0,25"}}, "OCC_JITTER_FACTOR=0,25 (use a number from 0.0 to 1.0)"},
        {{{KEY_OCC_JITTER_FACTOR, "nan"}}, "OCC_JITTER_FACTOR=nan (use a number from 0.0 to 1.0)"},
        {{{KEY_OCC_JITTER_FACTOR, ""}}, "OCC_JITTER_FACTOR= (use a number from 0.0 to 1.0)"},
    };
    for (const auto& [conn_attr, expected] : cases) {
        std::string error;
        EXPECT_FALSE(OccRetryConfig::Parse(conn_attr, error).has_value()) << expected;
        EXPECT_EQ(expected, error);
    }
}

TEST(DsqlOccRetryConfigTest, ValidateMap_ReportsTheBadOccOption) {
    EXPECT_EQ((std::unordered_set<std::string>{"OCC_MAX_RETRIES=101 (use an integer from 0 to 100)"}),
              AttributeValidator::ValidateMap({{KEY_OCC_MAX_RETRIES, "101"}, {KEY_PORT, "5432"}}));
    EXPECT_TRUE(AttributeValidator::ValidateMap({{KEY_OCC_MAX_RETRIES, "3"}, {KEY_ENABLE_OCC_RETRY, "0"}}).empty());
}

// Checked at every connect, before the base driver loads, whether or not ENABLE_OCC_RETRY is on.
TEST(DsqlOccRetryConfigTest, InitializeConnection_BadOccOptionFailsTheConnect) {
    ENV env;
    DBC dbc;
    dbc.env = &env;
    dbc.conn_attr = {{KEY_OCC_JITTER_FACTOR, "2"}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(&dbc, ""));

    ASSERT_TRUE(dbc.err);
    EXPECT_STREQ("01S00", dbc.err->sqlstate);
    EXPECT_NE(nullptr, std::strstr(dbc.err->error_msg, "OCC_JITTER_FACTOR=2 (use a number from 0.0 to 1.0)"));
}

namespace {
    // What the fake base driver answers.
    SQLRETURN g_prepare_result = SQL_SUCCESS;
    SQLULEN g_paramset_size = 1;
    int g_get_stmt_attr_calls = 0;
    // The base driver's diagnostic record 1 for the statement, as psqlODBC keeps it; an empty SQLSTATE means none.
    std::string g_base_state;
    std::string g_base_message;

    // Writes text into an ODBC character buffer of buffer_chars characters, terminated, as a driver does.
    void WriteText(SQLTCHAR* buffer, size_t buffer_chars, const std::string& text) {
        if (buffer == nullptr || buffer_chars == 0) {
            return;
        }
#if UNICODE
        const std::vector<uint16_t> utf16 = ConvertUTF8ToUTF16(text);
        const size_t count = std::min(utf16.size() - 1, buffer_chars - 1);
        std::copy_n(utf16.begin(), count, reinterpret_cast<uint16_t*>(buffer));
        reinterpret_cast<uint16_t*>(buffer)[count] = 0;
#else
        const size_t count = std::min(text.size(), buffer_chars - 1);
        std::copy_n(text.begin(), count, reinterpret_cast<char*>(buffer));
        reinterpret_cast<char*>(buffer)[count] = 0;
#endif
    }

    // An ODBC character buffer as UTF-8.
    std::string ToUtf8(SQLTCHAR* text) {
#if UNICODE
        return ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(text));
#else
        return reinterpret_cast<const char*>(text);
#endif
    }

    SQLRETURN FakeSQLPrepare(SQLHSTMT /*statement*/, SQLTCHAR* /*text*/, SQLINTEGER /*length*/) {
        return g_prepare_result;
    }

    SQLRETURN FakeSQLGetStmtAttr(SQLHSTMT /*statement*/, SQLINTEGER attribute, SQLPOINTER value,
                                 SQLINTEGER /*buffer_length*/, SQLINTEGER* /*string_length*/) {
        ++g_get_stmt_attr_calls;
        if (attribute == SQL_ATTR_PARAMSET_SIZE && value) {
            *static_cast<SQLULEN*>(value) = g_paramset_size;
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSQLGetDiagRec(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT record,
                                SQLTCHAR* state, SQLINTEGER* native_error, SQLTCHAR* message,
                                SQLSMALLINT buffer_length, SQLSMALLINT* text_length) {
        if (record != 1 || g_base_state.empty()) {
            return SQL_NO_DATA;
        }
        WriteText(state, MAX_SQL_STATE_LEN, g_base_state);
        WriteText(message, static_cast<size_t>(buffer_length), g_base_message);
        if (native_error) {
            *native_error = 0;
        }
        if (text_length) {
            *text_length = static_cast<SQLSMALLINT>(g_base_message.size());
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSQLSetConnectAttr(SQLHDBC /*connection*/, SQLINTEGER /*attribute*/, SQLPOINTER /*value*/,
                                    SQLINTEGER /*length*/) {
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSQLEndTran(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT /*completion*/) {
        return SQL_SUCCESS;
    }

    // A base driver with only the calls these tests make.
    class FakeBaseDriver : public RdsLibLoader {
    public:
        // A dummy path, so the base constructor sets up the function cache.
        FakeBaseDriver() : RdsLibLoader("") {}

        FUNC_HANDLE GetFunction(const std::string& function_name) override {
            if (function_name == RDS_STR_SQLPrepare) {
                return reinterpret_cast<FUNC_HANDLE>(&FakeSQLPrepare);
            }
            if (function_name == RDS_STR_SQLGetStmtAttr) {
                return reinterpret_cast<FUNC_HANDLE>(&FakeSQLGetStmtAttr);
            }
            if (function_name == RDS_STR_SQLGetDiagRec) {
                return reinterpret_cast<FUNC_HANDLE>(&FakeSQLGetDiagRec);
            }
            if (function_name == RDS_STR_SQLSetConnectAttr) {
                return reinterpret_cast<FUNC_HANDLE>(&FakeSQLSetConnectAttr);
            }
            if (function_name == RDS_STR_SQLEndTran) {
                return reinterpret_cast<FUNC_HANDLE>(&FakeSQLEndTran);
            }
            return nullptr;
        }
    };

    // Statement text as the RDS_ helpers and the plugin chain get it: UTF-16 in Unicode builds, bytes in ANSI builds.
    class TestText {
    public:
#if UNICODE
        explicit TestText(const std::string& text) : buffer_(ConvertUTF8ToUTF16(text)) {}
#else
        explicit TestText(const std::string& text) : buffer_(text) {}
#endif
        SQLTCHAR* Get() { return reinterpret_cast<SQLTCHAR*>(buffer_.data()); }

    private:
#if UNICODE
        std::vector<uint16_t> buffer_;
#else
        std::string buffer_;
#endif
    };

    // One scripted result of the rest of the chain. code is the wrapper's error code, state the base driver's SQLSTATE.
    struct Step {
        SQLRETURN ret;
        SQL_STATE_CODE code;
        std::string state;
        std::string message;
    };

    Step Ok() { return {SQL_SUCCESS, ERR_GENERAL_ERROR, "", ""}; }

    // How a scripted failure is reported: as the wrapper's own record (stmt->err), or as the base driver's record,
    // which RDS_SQLGetDiagRec reads through SQLGetDiagRec, as for every psqlODBC error in production.
    enum class Report : std::uint8_t { WRAPPER_RECORD, BASE_RECORD };

    // The rest of the plugin chain: returns the scripted results in order (the last one repeats) and reports each
    // SQL_ERROR the way chosen.
    class ScriptedPlugin : public BasePlugin {
    public:
        explicit ScriptedPlugin(std::vector<Step> steps, Report report = Report::WRAPPER_RECORD)
            : steps_(std::move(steps)), report_(report) {}

        SQLRETURN Execute(SQLHSTMT StatementHandle, SQLTCHAR* StatementText, SQLINTEGER /*TextLength*/) override {
            STMT* stmt = static_cast<STMT*>(StatementHandle);
            // Every attempt must start with the statement's diagnostics cleared.
            EXPECT_EQ(nullptr, stmt->err) << "attempt " << (calls + 1) << " started with a stale error";
            handles.push_back(StatementHandle);
            texts.push_back(StatementText);
            stmt_attr_calls_seen.push_back(g_get_stmt_attr_calls);
            const Step& step = steps_.at(std::min(calls, steps_.size() - 1));
            ++calls;
            // Like psqlODBC, every execution starts with no record of its own.
            g_base_state.clear();
            g_base_message.clear();
            if (step.ret == SQL_ERROR) {
                if (report_ == Report::BASE_RECORD) {
                    g_base_state = step.state;
                    g_base_message = step.message;
                } else {
                    stmt->err = std::make_unique<ErrInfo>(step.message.c_str(), step.code);
                }
            }
            return step.ret;
        }

        size_t calls = 0;
        std::vector<SQLHSTMT> handles;
        std::vector<SQLTCHAR*> texts;
        std::vector<int> stmt_attr_calls_seen;

    private:
        std::vector<Step> steps_;
        Report report_;
    };
}  // namespace

// A connected statement in autocommit mode with no transaction open, over the fake base driver.
class DsqlOccRetryPluginTest : public testing::Test {
protected:
    ENV* env_ = nullptr;
    DBC* dbc_ = nullptr;
    STMT* stmt_ = nullptr;
    int base_dbc_stand_in_ = 0;
    int base_stmt_stand_in_ = 0;
    std::shared_ptr<FakeBaseDriver> lib_loader_;

    void SetUp() override {
        g_prepare_result = SQL_SUCCESS;
        g_paramset_size = 1;
        g_get_stmt_attr_calls = 0;
        g_base_state.clear();
        g_base_message.clear();
        lib_loader_ = std::make_shared<FakeBaseDriver>();
        env_ = new ENV();
        env_->driver_lib_loader = lib_loader_;
        dbc_ = new DBC();
        dbc_->env = env_;
        dbc_->wrapped_dbc = static_cast<SQLHDBC>(&base_dbc_stand_in_);
        const auto plugin_service = std::make_shared<testing::NiceMock<MockPluginService>>();
        ON_CALL(*plugin_service, GetOdbcHelper())
            .WillByDefault(testing::Return(std::make_shared<OdbcHelper>(lib_loader_, env_)));
        dbc_->plugin_service = plugin_service;
        dbc_->auto_commit = true;
        dbc_->transaction_status = TRANSACTION_CLOSED;
        stmt_ = new STMT();
        stmt_->dbc = dbc_;
        stmt_->wrapped_stmt = static_cast<SQLHSTMT>(&base_stmt_stand_in_);
    }

    void TearDown() override {
        delete stmt_;
        delete dbc_;
        delete env_;
    }
};

TEST_F(DsqlOccRetryPluginTest, Prepare_KeepsTheStatementText) {
    TestText text("UPDATE t SET v = v + 1 WHERE id = ?");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLPrepare(stmt_, text.Get(), SQL_NTS));
    EXPECT_EQ("UPDATE t SET v = v + 1 WHERE id = ?", stmt_->prepared_text);
}

// psqlODBC prepares TextLength characters, so that's what is classified: "COMMITX" cut to 6 is a COMMIT.
TEST_F(DsqlOccRetryPluginTest, Prepare_KeepsOnlyTextLengthCharacters) {
    TestText text("UPDATE t SET v = 1 WHERE id = ?; trailing bytes");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLPrepare(stmt_, text.Get(), 31));
    EXPECT_EQ("UPDATE t SET v = 1 WHERE id = ?", stmt_->prepared_text);
    TestText commit("COMMITX");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLPrepare(stmt_, commit.Get(), 6));
    EXPECT_EQ("COMMIT", stmt_->prepared_text);
}

TEST_F(DsqlOccRetryPluginTest, Prepare_FailureKeepsNoText) {
    stmt_->prepared_text = "SELECT 1";
    g_prepare_result = SQL_ERROR;
    TestText text("UPDATE t SET v = 1");
    EXPECT_EQ(SQL_ERROR, RDS_SQLPrepare(stmt_, text.Get(), SQL_NTS));
    EXPECT_TRUE(stmt_->prepared_text.empty());
}

// A direct execute replaces the prepared statement on the handle.
TEST_F(DsqlOccRetryPluginTest, ExecDirect_DropsThePreparedText) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()});
    dbc_->plugin_head = chain.get();
    stmt_->prepared_text = "UPDATE t SET v = 1";
    TestText text("SELECT 1");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, text.Get(), SQL_NTS));
    EXPECT_TRUE(stmt_->prepared_text.empty());
}
