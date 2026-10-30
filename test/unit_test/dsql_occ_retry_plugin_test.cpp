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
#include "../../driver/plugin/dsql/dsql_occ_retry_plugin.h"
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

namespace {
    // The messages Aurora DSQL sends with SQLSTATE 40001 (DSQL user guide, "Concurrency control in Aurora DSQL").
    const std::string OC000_MESSAGE = "ERROR: change conflicts with another transaction (OC000)";
    const std::string OC001_MESSAGE = "ERROR: schema has been updated by another transaction (OC001)";
    const std::string UPDATE_STATEMENT = "UPDATE t SET v = v + 1 WHERE id = 1";

    Step Occ(const std::string& message) { return {SQL_ERROR, ERR_SERIALIZATION_FAILURE, "40001", message}; }
    Step Fail(SQL_STATE_CODE code, const std::string& state, const std::string& message) {
        return {SQL_ERROR, code, state, message};
    }

    // Collects log lines, so a test can count what the plugin logged.
    class OccLogSink : public nglog::LogSink {
    public:
        void send(nglog::LogSeverity /*severity*/, const char* /*full_filename*/, const char* /*base_filename*/,
                  int /*line*/, const nglog::LogMessageTime& /*time*/, const char* message, size_t message_len) override {
            const std::lock_guard<std::mutex> lock(mutex_);
            messages_.emplace_back(message, message_len);
        }

        long Count(const std::string& text) {
            const std::lock_guard<std::mutex> lock(mutex_);
            return std::ranges::count_if(messages_, [&text](const std::string& m) { return m.find(text) != std::string::npos; });
        }

    private:
        std::mutex mutex_;
        std::vector<std::string> messages_;
    };

    using Change = DsqlOccRetryPlugin::TransactionChange;
}  // namespace

TEST(DsqlOccRetryRuleTest, OccConflicts_AreMatchedBySqlState) {
    for (const std::string state : {"40001", "OC000", "OC001"}) {
        EXPECT_TRUE(DsqlOccRetryPlugin::IsOccConflict(state)) << state;
    }
}

// The message never decides on its own (XX000 can carry "(OC000)" and may have committed), as in Go and Rust.
TEST(DsqlOccRetryRuleTest, OtherErrors_AreNot) {
    for (const std::string state : {"23505", "40P01", "08S01", "XX000", "25P02", "0A000", ""}) {
        EXPECT_FALSE(DsqlOccRetryPlugin::IsOccConflict(state)) << state;
    }
}

TEST(DsqlOccRetryStatementTest, SingleStatements_AreRetryable) {
    for (const std::string statement : {
             "UPDATE t SET v = v + 1 WHERE id = 1",
             "update t set v = 1;",
             "  INSERT INTO t VALUES (1)  ;  \n",
             "-- bump the counter\nUPDATE t SET v = v + 1",
             "-- bump the counter\rUPDATE t SET v = v + 1",
             "/* outer /* nested */ still a comment */ DELETE FROM t WHERE id = 1",
             "WITH x AS (SELECT 1) SELECT * FROM x",
             "(SELECT 1)",
             "CREATE INDEX ASYNC t_v ON t (v)",
             "SELECT 1;;",
             "INSERT INTO commit_log VALUES (1)",
         }) {
        EXPECT_TRUE(DsqlOccRetryPlugin::IsRetryableStatement(statement)) << statement;
    }
}

TEST(DsqlOccRetryStatementTest, TransactionControlAndBatches_AreNot) {
    for (const std::string statement : {
             "",
             "   ",
             ";",
             "-- only a comment",
             "/* unterminated comment UPDATE t SET v = 1",
             "BEGIN",
             "begin transaction",
             "START TRANSACTION",
             "COMMIT",
             "  commit;",
             "-- done\nCOMMIT",
             "-- done\rCOMMIT",
             "/* done */ROLLBACK",
             "ROLLBACK TO SAVEPOINT s1",
             "END",
             "ABORT",
             "SAVEPOINT s1",
             "RELEASE SAVEPOINT s1",
             "PREPARE TRANSACTION 'tx1'",
             "SET search_path TO app",
             "UPDATE a SET v = 1; UPDATE b SET v = 1",
             // A ';' in a literal or before a trailing comment also counts as a batch, so no batch is ever retried.
             "UPDATE t SET note = 'a;b' WHERE id = 1",
             "UPDATE t SET v = 1; -- trailing note",
         }) {
        EXPECT_FALSE(DsqlOccRetryPlugin::IsRetryableStatement(statement)) << statement;
    }
}

TEST(DsqlOccRetryStatementTest, TransactionChange_Opens) {
    for (const std::string statement : {
             "BEGIN",
             "begin;",
             "\nBEGIN",
             "\tBEGIN ISOLATION LEVEL REPEATABLE READ",
             "-- open\nBEGIN",
             "-- open\rBEGIN",
             "/* open */START TRANSACTION",
             "/* a /* nested */ b */ BEGIN",
             "SELECT 1; BEGIN",
             "SELECT 1;BEGIN",
             "SELECT 1;\n-- then\nBEGIN",
             "SELECT 1; /* a ; b */ BEGIN",
             "UPDATE t SET v = 1; START TRANSACTION",
             "COMMIT; BEGIN",
             "COMMIT AND CHAIN",
             "ROLLBACK WORK AND CHAIN",
             // After a ';' inside a literal: a transaction that isn't there, which is the safe direction.
             "SELECT 'x;begin'",
         }) {
        EXPECT_EQ(Change::OPENS, DsqlOccRetryPlugin::GetTransactionChange(statement)) << statement;
    }
}

TEST(DsqlOccRetryStatementTest, TransactionChange_Closes) {
    for (const std::string statement : {
             "COMMIT",
             "commit;",
             "  COMMIT  ;  ",
             "\nCOMMIT",
             "-- done\nCOMMIT",
             "-- done\rCOMMIT",
             "COMMIT -- done",
             "/* done */ ROLLBACK",
             "END",
             "end transaction",
             "ABORT",
             "ROLLBACK WORK",
             "COMMIT AND NO CHAIN",
             "COMMIT TRANSACTION AND NO CHAIN",
         }) {
        EXPECT_EQ(Change::CLOSES, DsqlOccRetryPlugin::GetTransactionChange(statement)) << statement;
    }
}

TEST(DsqlOccRetryStatementTest, TransactionChange_None) {
    for (const std::string statement : {
             "",
             ";",
             "UPDATE t SET v = 1",
             "SELECT begin_date FROM t",
             "INSERT INTO commit_log VALUES (1)",
             "BEGINNING",
             "-- BEGIN",
             "/* BEGIN */ SELECT 1",
             // Only a single statement that does nothing else closes; these leave the state as it was.
             "SELECT 1; COMMIT",
             "COMMIT; SELECT 1",
             "COMMIT; -- done",
             "INSERT INTO t VALUES ('x;commit')",
             "ROLLBACK TO SAVEPOINT s1",
             "COMMIT PREPARED 'tx1'",
         }) {
        EXPECT_EQ(Change::NONE, DsqlOccRetryPlugin::GetTransactionChange(statement)) << statement;
    }
}

TEST(DsqlOccRetryBackoffTest, DoublesFromTheBaseUpToTheCap) {
    const OccRetryConfig config;
    const std::vector<int64_t> expected = {100, 200, 400, 800, 1600, 3200, 5000, 5000};
    for (size_t i = 0; i < expected.size(); ++i) {
        const int retry = static_cast<int>(i) + 1;
        EXPECT_EQ(expected[i], DsqlOccRetryPlugin::GetBackoffDelay(config, retry, 0.0).count()) << retry;
    }
}

TEST(DsqlOccRetryBackoffTest, AddsUpToTheJitterFactor) {
    const OccRetryConfig config;
    EXPECT_EQ(124, DsqlOccRetryPlugin::GetBackoffDelay(config, 1, 0.999).count());   // 100 + 24.975
    EXPECT_EQ(6248, DsqlOccRetryPlugin::GetBackoffDelay(config, 7, 0.999).count());  // jitter on the 5000 ms cap
    OccRetryConfig no_jitter;
    no_jitter.jitter_factor = 0.0;
    EXPECT_EQ(400, DsqlOccRetryPlugin::GetBackoffDelay(no_jitter, 3, 0.999).count());
}

TEST(DsqlOccRetryBackoffTest, ManyRetriesDoNotOverflow) {
    OccRetryConfig config;
    config.base_delay_ms = 1;
    config.max_delay_ms = INT_MAX;
    config.jitter_factor = 0.0;
    EXPECT_EQ(INT_MAX, DsqlOccRetryPlugin::GetBackoffDelay(config, 100, 0.0).count());
    config.base_delay_ms = INT_MAX;
    EXPECT_EQ(INT_MAX, DsqlOccRetryPlugin::GetBackoffDelay(config, 100, 0.0).count());
}

// The plugin with a sleep that records each delay and a fixed random number.
class DsqlOccRetryExecuteTest : public DsqlOccRetryPluginTest {
protected:
    std::vector<int64_t> sleeps_ms_;
    double random_unit_ = 0.0;

    std::shared_ptr<DsqlOccRetryPlugin> MakePlugin(const std::shared_ptr<BasePlugin>& next,
                                                   const std::map<std::string, std::string>& options = {}) {
        dbc_->conn_attr = options;
        dbc_->conn_attr.insert_or_assign(KEY_ENABLE_OCC_RETRY, VALUE_BOOL_TRUE);
        return std::make_shared<DsqlOccRetryPlugin>(dbc_, next,
            [this](std::chrono::milliseconds delay) { sleeps_ms_.push_back(delay.count()); },
            [this] { return random_unit_; });
    }

    // Runs one statement through a new plugin, starting with clear diagnostics as SQLExecDirect does. The transaction
    // state lives on the DBC, so it carries over from one run to the next.
    SQLRETURN Run(const std::shared_ptr<ScriptedPlugin>& chain, const std::string& statement,
                  const std::map<std::string, std::string>& options = {}) {
        ClearError(stmt_);
        TestText text(statement);
        return MakePlugin(chain, options)->Execute(stmt_, text.Get(), SQL_NTS);
    }

    // Runs a prepared statement: SQLExecute passes no text.
    SQLRETURN RunPrepared(const std::shared_ptr<ScriptedPlugin>& chain, const std::string& prepared_text) {
        ClearError(stmt_);
        stmt_->prepared_text = prepared_text;
        return MakePlugin(chain)->Execute(stmt_, nullptr, SQL_NTS);
    }

    // An UPDATE that conflicts once: 2 attempts when the plugin retried it, 1 when it didn't.
    size_t AttemptsForAConflict() {
        const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
        Run(chain, UPDATE_STATEMENT);
        return chain->calls;
    }
};

TEST_F(DsqlOccRetryExecuteTest, SucceedsFirstTime_RunsOnce) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()});
    EXPECT_EQ(SQL_SUCCESS, Run(chain, UPDATE_STATEMENT));
    EXPECT_EQ(1U, chain->calls);
    EXPECT_TRUE(sleeps_ms_.empty());
}

TEST_F(DsqlOccRetryExecuteTest, ConflictThenSuccess_RunsAgainOnTheSameHandle) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
    TestText text(UPDATE_STATEMENT);
    EXPECT_EQ(SQL_SUCCESS, MakePlugin(chain)->Execute(stmt_, text.Get(), SQL_NTS));
    EXPECT_EQ((std::vector<SQLHSTMT>{stmt_, stmt_}), chain->handles);
    EXPECT_EQ((std::vector<SQLTCHAR*>{text.Get(), text.Get()}), chain->texts);
    EXPECT_EQ((std::vector<int64_t>{100}), sleeps_ms_);
    EXPECT_EQ(nullptr, stmt_->err);
}

TEST_F(DsqlOccRetryExecuteTest, SchemaConflict_IsRetried) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC001_MESSAGE), Ok()});
    EXPECT_EQ(SQL_SUCCESS, Run(chain, "INSERT INTO t VALUES (1, 0)"));
    EXPECT_EQ(2U, chain->calls);
}

TEST_F(DsqlOccRetryExecuteTest, ConflictEveryTime_ReturnsTheLastError) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{
        Occ(OC000_MESSAGE + " 1"), Occ(OC000_MESSAGE + " 2"), Occ(OC000_MESSAGE + " 3"), Occ(OC000_MESSAGE + " 4")});
    EXPECT_EQ(SQL_ERROR, Run(chain, UPDATE_STATEMENT));
    EXPECT_EQ(4U, chain->calls);
    EXPECT_EQ((std::vector<int64_t>{100, 200, 400}), sleeps_ms_);
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_STREQ("40001", stmt_->err->sqlstate);
    EXPECT_EQ(OC000_MESSAGE + " 4", stmt_->err->error_msg);
    // The plugin's reads didn't use up SQLError's one record: the application gets it once.
    SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS,
              RDS_SQLError(nullptr, nullptr, stmt_, state, &native_error, message, MAX_MSG_LEN, &length, false));
    EXPECT_EQ("40001", ToUtf8(state));
    EXPECT_EQ(SQL_NO_DATA,
              RDS_SQLError(nullptr, nullptr, stmt_, state, &native_error, message, MAX_MSG_LEN, &length, false));
}

TEST_F(DsqlOccRetryExecuteTest, FollowsTheConfiguredBackoff) {
    random_unit_ = 0.5;
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE)});
    EXPECT_EQ(SQL_ERROR, Run(chain, UPDATE_STATEMENT, {{KEY_OCC_MAX_RETRIES, "4"}, {KEY_OCC_BASE_DELAY_MS, "10"},
                                                      {KEY_OCC_MAX_DELAY_MS, "30"}, {KEY_OCC_JITTER_FACTOR, "0.5"}}));
    EXPECT_EQ(5U, chain->calls);
    // 10, 20, then the 30 ms cap; each plus 0.5 x 0.5 of itself, rounded down.
    EXPECT_EQ((std::vector<int64_t>{12, 25, 37, 37}), sleeps_ms_);
}

TEST_F(DsqlOccRetryExecuteTest, ZeroMaxRetries_RunsOnce) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
    EXPECT_EQ(SQL_ERROR, Run(chain, UPDATE_STATEMENT, {{KEY_OCC_MAX_RETRIES, "0"}}));
    EXPECT_EQ(1U, chain->calls);
    EXPECT_TRUE(sleeps_ms_.empty());
    EXPECT_EQ(0, g_get_stmt_attr_calls);
}

TEST_F(DsqlOccRetryExecuteTest, OtherError_IsNotRetried) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{
        Fail(ERR_INTEGRITY_CONSTRAINT_VIOLATION, "23505", "duplicate key value violates unique constraint"), Ok()});
    EXPECT_EQ(SQL_ERROR, Run(chain, UPDATE_STATEMENT));
    EXPECT_EQ(1U, chain->calls);
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_STREQ("23000", stmt_->err->sqlstate);
}

TEST_F(DsqlOccRetryExecuteTest, OtherErrorOnARetry_IsReturnedAtOnce) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{
        Occ(OC000_MESSAGE), Fail(ERR_COMMUNICATION_LINK_FAILURE, "08S01", "server closed the connection unexpectedly"),
        Ok()});
    EXPECT_EQ(SQL_ERROR, Run(chain, UPDATE_STATEMENT));
    EXPECT_EQ(2U, chain->calls);
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_STREQ("08S01", stmt_->err->sqlstate);
}

TEST_F(DsqlOccRetryExecuteTest, OtherResults_PassThrough) {
    for (const SQLRETURN result :
         {SQL_SUCCESS_WITH_INFO, SQL_NO_DATA, SQL_NEED_DATA, SQL_STILL_EXECUTING, SQL_INVALID_HANDLE}) {
        const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{{result, ERR_GENERAL_ERROR, "", ""}, Ok()});
        EXPECT_EQ(result, Run(chain, UPDATE_STATEMENT));
        EXPECT_EQ(1U, chain->calls) << result;
    }
}

TEST_F(DsqlOccRetryExecuteTest, ManualCommit_IsNotRetried) {
    dbc_->auto_commit = false;
    dbc_->transaction_status = TRANSACTION_OPEN;
    EXPECT_EQ(1U, AttemptsForAConflict());
}

// SQLEndTran marks the transaction closed, but with autocommit off the next statement opens another one.
TEST_F(DsqlOccRetryExecuteTest, ManualCommitAfterEndTran_IsNotRetried) {
    dbc_->auto_commit = false;
    dbc_->transaction_status = TRANSACTION_CLOSED;
    EXPECT_EQ(1U, AttemptsForAConflict());
}

// A BEGIN run through SQLExecDirect leaves autocommit on with a transaction open.
TEST_F(DsqlOccRetryExecuteTest, OpenTransaction_IsNotRetried) {
    dbc_->transaction_status = TRANSACTION_OPEN;
    EXPECT_EQ(1U, AttemptsForAConflict());
    dbc_->transaction_status = TRANSACTION_ERROR;
    EXPECT_EQ(1U, AttemptsForAConflict());
}

TEST_F(DsqlOccRetryExecuteTest, TransactionControl_IsNotRetried) {
    for (const std::string statement : {"COMMIT", "  commit;", "-- end the transaction\nCOMMIT", "/* done */ROLLBACK",
                                        "END", "ABORT", "RELEASE SAVEPOINT s1"}) {
        dbc_->transaction_status = TRANSACTION_CLOSED;
        const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
        EXPECT_EQ(SQL_ERROR, Run(chain, statement)) << statement;
        EXPECT_EQ(1U, chain->calls) << statement;
    }
}

TEST_F(DsqlOccRetryExecuteTest, Batch_IsNotRetried) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
    EXPECT_EQ(SQL_ERROR, Run(chain, "UPDATE a SET v = 1; UPDATE b SET v = 1"));
    EXPECT_EQ(1U, chain->calls);
}

// The base driver runs a direct statement to its terminator whatever TextLength says (DefaultPlugin passes SQL_NTS), so
// the whole text is classified, here a batch.
TEST_F(DsqlOccRetryExecuteTest, TextAfterTextLength_CountsToo) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
    ClearError(stmt_);
    TestText text("UPDATE a SET v = 1; UPDATE b SET v = 1");
    EXPECT_EQ(SQL_ERROR, MakePlugin(chain)->Execute(stmt_, text.Get(), 18));
    EXPECT_EQ(1U, chain->calls);
}

TEST_F(DsqlOccRetryExecuteTest, ParameterArray_IsNotRetried) {
    g_paramset_size = 10;
    EXPECT_EQ(1U, AttemptsForAConflict());
}

// psqlODBC clears the statement's diagnostics on SQLGetStmtAttr, so asking after a failure would lose them.
TEST_F(DsqlOccRetryExecuteTest, ParameterSetSize_IsAskedOnceBeforeTheFirstAttempt) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Occ(OC000_MESSAGE), Ok()});
    EXPECT_EQ(SQL_SUCCESS, Run(chain, UPDATE_STATEMENT));
    EXPECT_EQ((std::vector<int>{1, 1, 1}), chain->stmt_attr_calls_seen);
}

TEST_F(DsqlOccRetryExecuteTest, NoBaseStatement_IsNotRetried) {
    stmt_->wrapped_stmt = nullptr;
    EXPECT_EQ(1U, AttemptsForAConflict());
}

TEST_F(DsqlOccRetryExecuteTest, PreparedStatement_IsRetriedWithItsKeptText) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
    EXPECT_EQ(SQL_SUCCESS, RunPrepared(chain, "UPDATE t SET v = v + 1 WHERE id = ?"));
    EXPECT_EQ((std::vector<SQLTCHAR*>{nullptr, nullptr}), chain->texts);
}

TEST_F(DsqlOccRetryExecuteTest, PreparedTransactionControl_IsNotRetried) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
    EXPECT_EQ(SQL_ERROR, RunPrepared(chain, "COMMIT"));
    EXPECT_EQ(1U, chain->calls);
}

TEST_F(DsqlOccRetryExecuteTest, PreparedWithoutKeptText_IsNotRetried) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
    EXPECT_EQ(SQL_ERROR, RunPrepared(chain, ""));
    EXPECT_EQ(1U, chain->calls);
}

TEST_F(DsqlOccRetryExecuteTest, LogsEachRetryAndWhenRetriesRunOut) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE)});
    OccLogSink sink;
    nglog::AddLogSink(&sink);
    EXPECT_EQ(SQL_ERROR, Run(chain, UPDATE_STATEMENT));
    nglog::RemoveLogSink(&sink);
    EXPECT_EQ(1, sink.Count("[DSQL] OCC conflict (OC000, SQLSTATE 40001): retry 1/3 in 100 ms"));
    EXPECT_EQ(1, sink.Count("[DSQL] OCC conflict (OC000, SQLSTATE 40001): retry 2/3 in 200 ms"));
    EXPECT_EQ(1, sink.Count("[DSQL] OCC conflict (OC000, SQLSTATE 40001): retry 3/3 in 400 ms"));
    EXPECT_EQ(3, sink.Count("): retry "));
    EXPECT_EQ(1, sink.Count(
        "[DSQL] OCC conflict (OC000, SQLSTATE 40001) persisted after 3 retries; returning the error"));
}

TEST_F(DsqlOccRetryExecuteTest, SuccessfulRetry_LogsNoGiveUpLine) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC001_MESSAGE), Ok()});
    OccLogSink sink;
    nglog::AddLogSink(&sink);
    EXPECT_EQ(SQL_SUCCESS, Run(chain, UPDATE_STATEMENT));
    nglog::RemoveLogSink(&sink);
    EXPECT_EQ(1, sink.Count("[DSQL] OCC conflict (OC001, SQLSTATE 40001): retry 1/3 in 100 ms"));
    EXPECT_EQ(0, sink.Count("persisted after"));
}

TEST_F(DsqlOccRetryExecuteTest, ConflictWithoutAnOcCode_LogsTheSqlState) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{
        Occ("could not serialize access due to concurrent update"), Ok()});
    OccLogSink sink;
    nglog::AddLogSink(&sink);
    EXPECT_EQ(SQL_SUCCESS, Run(chain, UPDATE_STATEMENT));
    nglog::RemoveLogSink(&sink);
    EXPECT_EQ(1, sink.Count("[DSQL] OCC conflict (SQLSTATE 40001): retry 1/3 in 100 ms"));
}

TEST_F(DsqlOccRetryExecuteTest, NotRetried_LogsNothing) {
    dbc_->auto_commit = false;
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE)});
    OccLogSink sink;
    nglog::AddLogSink(&sink);
    EXPECT_EQ(SQL_ERROR, Run(chain, UPDATE_STATEMENT));
    nglog::RemoveLogSink(&sink);
    EXPECT_EQ(0, sink.Count("[DSQL] OCC conflict"));
}

TEST_F(DsqlOccRetryExecuteTest, BadOption_IsRejected) {
    dbc_->conn_attr = {{KEY_ENABLE_OCC_RETRY, VALUE_BOOL_TRUE}, {KEY_OCC_MAX_RETRIES, "101"}};
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()});
    try {
        const DsqlOccRetryPlugin plugin(dbc_, chain, nullptr, nullptr);
        FAIL() << "the plugin accepted OCC_MAX_RETRIES=101";
    } catch (const std::invalid_argument& ex) {
        EXPECT_STREQ("Invalid value specified for connection string attribute: "
                     "OCC_MAX_RETRIES=101 (use an integer from 0 to 100)", ex.what());
    }
}

// psqlODBC's own record, read through SQLGetDiagRec as in production (DefaultPlugin sets no wrapper record).
TEST_F(DsqlOccRetryExecuteTest, BaseRecord_ConflictThenSuccess_IsRetried) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()},
                                                        Report::BASE_RECORD);
    EXPECT_EQ(SQL_SUCCESS, Run(chain, UPDATE_STATEMENT));
    EXPECT_EQ(2U, chain->calls);
}

TEST_F(DsqlOccRetryExecuteTest, BaseRecord_RetriesRunOut_TheAppReadsTheLastConflict) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{
        Occ(OC000_MESSAGE + " 1"), Occ(OC000_MESSAGE + " 2"), Occ(OC000_MESSAGE + " 3"), Occ(OC000_MESSAGE + " 4")},
        Report::BASE_RECORD);
    EXPECT_EQ(SQL_ERROR, Run(chain, UPDATE_STATEMENT));
    EXPECT_EQ(4U, chain->calls);
    // No wrapper record hides psqlODBC's.
    EXPECT_EQ(nullptr, stmt_->err);
    SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_STMT, stmt_, 1, state, &native_error, message, MAX_MSG_LEN,
                                             &length, false));
    EXPECT_EQ("40001", ToUtf8(state));
    EXPECT_EQ(OC000_MESSAGE + " 4", ToUtf8(message));
    EXPECT_EQ(SQL_SUCCESS,
              RDS_SQLError(nullptr, nullptr, stmt_, state, &native_error, message, MAX_MSG_LEN, &length, false));
    EXPECT_EQ("40001", ToUtf8(state));
    // SQLError then asks for record 0, which the fake answers with SQL_NO_DATA.
    EXPECT_EQ(SQL_NO_DATA,
              RDS_SQLError(nullptr, nullptr, stmt_, state, &native_error, message, MAX_MSG_LEN, &length, false));
}

TEST_F(DsqlOccRetryExecuteTest, BaseRecord_OtherErrorAfterAConflict_IsReturnedAtOnce) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{
        Occ(OC000_MESSAGE), Fail(ERR_COMMUNICATION_LINK_FAILURE, "08S01", "server closed the connection unexpectedly"),
        Ok()}, Report::BASE_RECORD);
    EXPECT_EQ(SQL_ERROR, Run(chain, UPDATE_STATEMENT));
    EXPECT_EQ(2U, chain->calls);
    SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_STMT, stmt_, 1, state, &native_error, message, MAX_MSG_LEN,
                                             &length, false));
    EXPECT_EQ("08S01", ToUtf8(state));
}

// A transaction DefaultPlugin doesn't record (it reads only direct text, trims only spaces and /* */
// comments, and looks at the first statement) must still stop retries: after DSQL rolled the transaction back, a retry
// would commit the statement on its own.
TEST_F(DsqlOccRetryExecuteTest, BeginInAnyForm_StopsRetries) {
    for (const std::string begin : {"\nBEGIN", "\tbegin", "-- open\nBEGIN", "-- open\rBEGIN",
                                    "/* open */\nSTART TRANSACTION", "SELECT 1; BEGIN", "SELECT 1;\nBEGIN",
                                    "SELECT 1; /* a ; b */ BEGIN", "COMMIT AND CHAIN"}) {
        dbc_->transaction_status = TRANSACTION_CLOSED;
        EXPECT_EQ(SQL_SUCCESS, Run(std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()}), begin)) << begin;
        EXPECT_EQ(TRANSACTION_OPEN, dbc_->transaction_status) << begin;
        EXPECT_EQ(1U, AttemptsForAConflict()) << begin;
    }
}

TEST_F(DsqlOccRetryExecuteTest, PreparedBegin_StopsRetries) {
    EXPECT_EQ(SQL_SUCCESS, RunPrepared(std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()}), "BEGIN"));
    EXPECT_EQ(TRANSACTION_OPEN, dbc_->transaction_status);
    EXPECT_EQ(1U, AttemptsForAConflict());
}

// The safe direction: a batch can fail after its BEGIN ran, and a batch waiting for data at execution runs its BEGIN
// when SQLParamData sends it, which never reaches the plugin.
TEST_F(DsqlOccRetryExecuteTest, FailedBegin_StillStopsRetries) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{
        Fail(ERR_COMMUNICATION_LINK_FAILURE, "08S01", "server closed the connection unexpectedly")});
    EXPECT_EQ(SQL_ERROR, Run(chain, "BEGIN; UPDATE t SET v = 1"));
    EXPECT_EQ(TRANSACTION_OPEN, dbc_->transaction_status);
    dbc_->transaction_status = TRANSACTION_CLOSED;
    const auto need_data =
        std::make_shared<ScriptedPlugin>(std::vector<Step>{{SQL_NEED_DATA, ERR_GENERAL_ERROR, "", ""}, Ok()});
    EXPECT_EQ(SQL_NEED_DATA, Run(need_data, "BEGIN; INSERT INTO t VALUES (?)"));
    EXPECT_EQ(TRANSACTION_OPEN, dbc_->transaction_status);
    EXPECT_EQ(1U, AttemptsForAConflict());
}

TEST_F(DsqlOccRetryExecuteTest, PlainClose_EndsATrackedTransaction) {
    for (const std::string close : {"\nCOMMIT", "-- done\nCOMMIT;", "end", "ROLLBACK", "ABORT WORK",
                                    "COMMIT AND NO CHAIN"}) {
        dbc_->transaction_status = TRANSACTION_OPEN;
        EXPECT_EQ(SQL_SUCCESS, Run(std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()}), close)) << close;
        EXPECT_EQ(TRANSACTION_CLOSED, dbc_->transaction_status) << close;
        EXPECT_EQ(2U, AttemptsForAConflict()) << close;
    }
}

// Text that only looks like an end: a COMMIT inside a literal, a savepoint rollback, a COMMIT inside a batch, and a
// COMMIT that failed (DSQL may have ended the transaction then, but the plugin doesn't assume it).
TEST_F(DsqlOccRetryExecuteTest, OtherText_LeavesATrackedTransactionOpen) {
    const std::vector<std::pair<std::string, Step>> cases = {
        {"INSERT INTO t VALUES ('x;commit')", Ok()},
        {"ROLLBACK TO SAVEPOINT s1", Ok()},
        {"SELECT 1; COMMIT", Ok()},
        {"COMMIT", Occ(OC000_MESSAGE)},
    };
    for (const auto& [statement, step] : cases) {
        dbc_->transaction_status = TRANSACTION_OPEN;
        Run(std::make_shared<ScriptedPlugin>(std::vector<Step>{step}), statement);
        EXPECT_EQ(TRANSACTION_OPEN, dbc_->transaction_status) << statement;
        EXPECT_EQ(1U, AttemptsForAConflict()) << statement;
    }
}

// With autocommit off the plugin leaves the transaction state to DefaultPlugin, SQLEndTran and the autocommit switch.
TEST_F(DsqlOccRetryExecuteTest, ManualCommit_TransactionTextIsNotFollowed) {
    dbc_->auto_commit = false;
    dbc_->transaction_status = TRANSACTION_OPEN;
    Run(std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()}), "COMMIT");
    EXPECT_EQ(TRANSACTION_OPEN, dbc_->transaction_status);
}

namespace {
    // The rest of a chain whose connect succeeded with a warning record, as the DSQL auth plugin's 01S02.
    class WarningConnectPlugin : public BasePlugin {
    public:
        WarningConnectPlugin() : BasePlugin(nullptr) {}
        SQLRETURN Connect(SQLHDBC ConnectionHandle, SQLHWND /*WindowHandle*/, SQLTCHAR* /*OutConnectionString*/,
                          SQLSMALLINT /*BufferLength*/, SQLSMALLINT* /*StringLengthPtr*/,
                          SQLUSMALLINT /*DriverCompletion*/) override {
            DBC* dbc = static_cast<DBC*>(ConnectionHandle);
            ClearError(dbc);
            dbc->err = std::make_unique<ErrInfo>("SSLmode=disable was changed to verify-full", WARN_OPTION_VALUE_CHANGED);
            return SQL_SUCCESS_WITH_INFO;
        }
    };
}  // namespace

// A successful connect's warning reaches the application untouched.
TEST_F(DsqlOccRetryPluginTest, Connect_PassesAWarningAndItsRecordThrough) {
    DsqlOccRetryPlugin plugin(dbc_, std::make_shared<WarningConnectPlugin>());
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, plugin.Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("01S02", dbc_->err->sqlstate);
}

// Text outside ASCII, as UTF-16 from SQLExecDirectW in Unicode builds and as UTF-8 bytes in ANSI builds: an UPDATE is
// still one retryable statement, and a BEGIN after a non-ASCII comment still opens a transaction.
TEST_F(DsqlOccRetryExecuteTest, NonAsciiText_IsClassifiedAsTheBaseDriverRunsIt) {
    const auto chain = std::make_shared<ScriptedPlugin>(std::vector<Step>{Occ(OC000_MESSAGE), Ok()});
    EXPECT_EQ(SQL_SUCCESS, Run(chain, "UPDATE t SET name = 'Zoë Ångström' WHERE id = 1"));
    EXPECT_EQ(2U, chain->calls);
    EXPECT_EQ(SQL_SUCCESS, Run(std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()}), "-- café\nBEGIN"));
    EXPECT_EQ(TRANSACTION_OPEN, dbc_->transaction_status);
    EXPECT_EQ(1U, AttemptsForAConflict());
}

// psqlODBC does nothing when autocommit is set on while it is already on (options.c:372-373), so a transaction opened
// with BEGIN stays open on the server, and the wrapper must keep saying so.
TEST_F(DsqlOccRetryExecuteTest, AutocommitSetOnAgain_KeepsATrackedTransactionOpen) {
    Run(std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()}), "\nBEGIN");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT,
                                                 reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_TRUE(dbc_->auto_commit);
    EXPECT_EQ(TRANSACTION_OPEN, dbc_->transaction_status);
    EXPECT_EQ(1U, AttemptsForAConflict());
}

// Switching autocommit on from off makes psqlODBC commit an open transaction (connection.c:528-545).
TEST_F(DsqlOccRetryExecuteTest, AutocommitSwitchedOn_EndsTheTransaction) {
    dbc_->auto_commit = false;
    dbc_->transaction_status = TRANSACTION_OPEN;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT,
                                                 reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_EQ(TRANSACTION_CLOSED, dbc_->transaction_status);
    EXPECT_EQ(2U, AttemptsForAConflict());
}

// SQLEndTran ends a transaction the application opened with BEGIN in autocommit mode: psqlODBC sends the COMMIT
// (execute.c:1313, statement.c:2149-2155), and the wrapper marks it closed (odbcapi_rds_helper.cpp:327-329).
TEST_F(DsqlOccRetryExecuteTest, EndTran_EndsATrackedTransaction) {
    Run(std::make_shared<ScriptedPlugin>(std::vector<Step>{Ok()}), "\nBEGIN");
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_DBC, dbc_, SQL_COMMIT));
    EXPECT_EQ(TRANSACTION_CLOSED, dbc_->transaction_status);
    EXPECT_EQ(2U, AttemptsForAConflict());
}
