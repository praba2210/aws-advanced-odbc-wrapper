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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../driver/driver.h"
#include "../../driver/error.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/base_plugin.h"
#include "../../driver/plugin/dsql/dsql_error_hint_plugin.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/logger_wrapper.h"
#include "../../driver/util/plugin_service.h"

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-west-2.on.aws";
    const std::string AURORA_HOST = "database-1.cluster-abcdefghijkl.us-west-2.rds.amazonaws.com";
    const std::string CLUSTER_ID = "abcdefghijklmnopqrstuvwxyz";
    const std::string AGE_HINT = "This may be Aurora DSQL's 60-minute connection limit";
    // libpq's text once the server has closed the connection; psqlODBC reports it as 08S01.
    const std::string CONNECTION_CLOSED = "server closed the connection unexpectedly";

    // Collects every log line while it exists. It removes itself from ng-log even when an ASSERT returns early.
    class CapturingLogSink : public nglog::LogSink {
    public:
        CapturingLogSink() { nglog::AddLogSink(this); }
        ~CapturingLogSink() override { nglog::RemoveLogSink(this); }
        CapturingLogSink(const CapturingLogSink&) = delete;
        CapturingLogSink& operator=(const CapturingLogSink&) = delete;

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

    // The rest of the chain: returns the set codes and, for a failing statement, sets a wrapper diagnostic, which
    // RDS_SQLGetDiagRec reads back like a base driver record.
    class FakeNextPlugin : public BasePlugin {
    public:
        SQLRETURN Connect(SQLHDBC /*ConnectionHandle*/, SQLHWND /*WindowHandle*/, SQLTCHAR* /*OutConnectionString*/,
                          SQLSMALLINT /*BufferLength*/, SQLSMALLINT* /*StringLengthPtr*/,
                          SQLUSMALLINT /*DriverCompletion*/) override {
            return connect_ret;
        }

        SQLRETURN Execute(SQLHSTMT StatementHandle, SQLTCHAR* /*StatementText*/, SQLINTEGER /*TextLength*/) override {
            if (execute_err.has_value()) {
                STMT* stmt = static_cast<STMT*>(StatementHandle);
                ClearError(stmt);
                stmt->err = std::make_unique<ErrInfo>(execute_message.c_str(), execute_err.value());
            }
            return execute_ret;
        }

        SQLRETURN connect_ret = SQL_SUCCESS;
        SQLRETURN execute_ret = SQL_SUCCESS;
        std::optional<SQL_STATE_CODE> execute_err;
        std::string execute_message;
    };

    // Controls the clock and the connection state, records diagnostic reads and dead checks in order, and can stand in
    // a record for SQLSTATEs ErrInfo can't carry.
    class TestHintPlugin : public DsqlErrorHintPlugin {
    public:
        TestHintPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next) : DsqlErrorHintPlugin(dbc, std::move(next)) {}

        std::chrono::steady_clock::time_point now{};
        // A record to return instead of reading one; empty reads the real record.
        std::optional<std::pair<std::string, std::string>> diag;
        // The base driver's SQL_ATTR_CONNECTION_DEAD; empty asks the real OdbcHelper.
        std::optional<bool> dead = true;
        std::vector<std::string> calls;

    protected:
        void ReadStmtError(STMT* stmt, std::string& sql_state, std::string& message) override {
            calls.emplace_back("read");
            if (diag.has_value()) {
                sql_state = diag->first;
                message = diag->second;
                return;
            }
            DsqlErrorHintPlugin::ReadStmtError(stmt, sql_state, message);
        }

        bool IsConnectionDead(DBC* dbc) override {
            calls.emplace_back("dead");
            return dead.has_value() ? dead.value() : DsqlErrorHintPlugin::IsConnectionDead(dbc);
        }

        std::chrono::steady_clock::time_point Now() override { return now; }
    };
}  // namespace

TEST(DsqlErrorHintPluginHintTest, ConnectionAgeHint_AtOrAfterTheThreshold) {
    EXPECT_EQ("[DSQL] The connection failed after 61 minutes. This may be Aurora DSQL's 60-minute connection limit. "
              "Disconnect and connect again, and recycle pooled connections before 55 minutes.",
              DsqlErrorHintPlugin::GetConnectionAgeHint("08S01", std::chrono::minutes(61) + std::chrono::seconds(30)));
    // Any SQLSTATE: DSQL documents none for this close, and whether the connection is gone decides (in Execute).
    for (const std::string sql_state : {"08S01", "08006", "57P01", "XX000", ""}) {
        EXPECT_FALSE(DsqlErrorHintPlugin::GetConnectionAgeHint(sql_state,
            DsqlErrorHintPlugin::CONNECTION_AGE_HINT_THRESHOLD).empty()) << sql_state;
    }
}

TEST(DsqlErrorHintPluginHintTest, ConnectionAgeHint_NoneBeforeTheThresholdOrForTheMessageLimit) {
    EXPECT_EQ("", DsqlErrorHintPlugin::GetConnectionAgeHint("08S01",
        DsqlErrorHintPlugin::CONNECTION_AGE_HINT_THRESHOLD - std::chrono::seconds(1)));
    // DSQL's 10 MiB message limit closes the connection with its own error: FATAL: invalid message length.
    EXPECT_EQ("", DsqlErrorHintPlugin::GetConnectionAgeHint("08P01", std::chrono::minutes(61)));
}

class DsqlErrorHintPluginTest : public testing::Test {
protected:
    ENV* env_ = nullptr;
    DBC* dbc_ = nullptr;
    STMT* stmt_ = nullptr;
    std::shared_ptr<FakeNextPlugin> next_;

    void SetUp() override {
        env_ = new ENV();
        dbc_ = new DBC();
        dbc_->env = env_;
        // Unicode builds of RDS_SQLGetDiagRec reach the OdbcHelper through the plugin service. This one has none.
        dbc_->plugin_service = std::make_shared<PluginService>();
        stmt_ = new STMT();
        stmt_->dbc = dbc_;
        next_ = std::make_shared<FakeNextPlugin>();
    }

    void TearDown() override {
        delete stmt_;
        delete dbc_;
        delete env_;
    }
};

TEST_F(DsqlErrorHintPluginTest, Execute_ReadsNoDiagnosticsUnlessTheStatementFails) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    for (const SQLRETURN code : {SQL_SUCCESS, SQL_SUCCESS_WITH_INFO, SQL_NO_DATA, SQL_NEED_DATA, SQL_STILL_EXECUTING}) {
        next_->execute_ret = code;
        EXPECT_EQ(code, plugin->Execute(stmt_, nullptr, SQL_NTS));
    }
    EXPECT_TRUE(plugin->calls.empty());
}

// Through the real diagnostic read: the fake sets the record psqlODBC reports once the server has closed the
// connection.
TEST_F(DsqlErrorHintPluginTest, Execute_ConnectionLostAfterTheLimit_LogsTheHintOncePerConnection) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->execute_ret = SQL_ERROR;
    next_->execute_err = ERR_COMMUNICATION_LINK_FAILURE;
    next_->execute_message = CONNECTION_CLOSED;

    CapturingLogSink sink;
    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(61);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));
    EXPECT_EQ(1, sink.Count(AGE_HINT));
    EXPECT_EQ(1, sink.Count("failed after 61 minutes"));

    // A new connection can get its own hint.
    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(58);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));

    EXPECT_EQ(2, sink.Count(AGE_HINT));
    EXPECT_EQ(1, sink.Count("failed after 58 minutes"));
    // The base driver's record is untouched.
    ASSERT_TRUE(stmt_->err);
    EXPECT_STREQ("08S01", stmt_->err->sqlstate);
    EXPECT_STREQ(CONNECTION_CLOSED.c_str(), stmt_->err->error_msg);
}

// psqlODBC's SQLGetConnectAttr clears its connection error, which the statement's record no longer needs once read.
TEST_F(DsqlErrorHintPluginTest, Execute_ChecksTheConnectionOnlyAfterReadingTheRecord) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->execute_ret = SQL_ERROR;
    plugin->diag = std::make_pair(std::string("08S01"), CONNECTION_CLOSED);

    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(61);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));

    EXPECT_EQ((std::vector<std::string>{"read", "dead"}), plugin->calls);
}

TEST_F(DsqlErrorHintPluginTest, Execute_ConnectionLostEarly_LogsNothingAndLeavesTheConnectionAlone) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->execute_ret = SQL_ERROR;
    next_->execute_err = ERR_COMMUNICATION_LINK_FAILURE;
    next_->execute_message = CONNECTION_CLOSED;

    CapturingLogSink sink;
    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(10);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));

    EXPECT_EQ(0, sink.Count("[DSQL]"));
    // No dead check before the threshold.
    EXPECT_EQ((std::vector<std::string>{"read"}), plugin->calls);
}

// A statement error at minute 61 on a connection that still works: nothing to reconnect.
TEST_F(DsqlErrorHintPluginTest, Execute_FailureOnALiveConnection_LogsNothing) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->execute_ret = SQL_ERROR;
    plugin->diag = std::make_pair(std::string("54000"), std::string("ERROR: transaction age limit of 300s exceeded"));
    plugin->dead = false;

    CapturingLogSink sink;
    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(61);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));

    EXPECT_EQ(0, sink.Count(AGE_HINT));
}

TEST_F(DsqlErrorHintPluginTest, Execute_MessageLimitError_LogsNothing) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->execute_ret = SQL_ERROR;
    plugin->diag = std::make_pair(std::string("08P01"), std::string("FATAL: invalid message length"));

    CapturingLogSink sink;
    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(61);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));

    EXPECT_EQ(0, sink.Count(AGE_HINT));
    EXPECT_EQ((std::vector<std::string>{"read"}), plugin->calls);
}

// No age from a connect that failed, or from one that never happened.
TEST_F(DsqlErrorHintPluginTest, Execute_ConnectionLostWithoutASuccessfulConnect_LogsNothing) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->execute_ret = SQL_ERROR;
    next_->execute_err = ERR_COMMUNICATION_LINK_FAILURE;
    next_->execute_message = CONNECTION_CLOSED;

    CapturingLogSink sink;
    plugin->now += std::chrono::minutes(61);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    next_->connect_ret = SQL_ERROR;
    EXPECT_EQ(SQL_ERROR, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(61);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));

    EXPECT_EQ(0, sink.Count(AGE_HINT));
}

TEST_F(DsqlErrorHintPluginTest, Execute_WrapperDiagnostic_LogsNothing) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->execute_ret = SQL_ERROR;
    next_->execute_err = ERR_UNDERLYING_HANDLE_NULL;
    next_->execute_message = "Unable to allocate underlying STMT";
    plugin->dead = false;

    CapturingLogSink sink;
    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(61);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));

    EXPECT_EQ(0, sink.Count("[DSQL]"));
    ASSERT_TRUE(stmt_->err);
    EXPECT_STREQ("Unable to allocate underlying STMT", stmt_->err->error_msg);
}

// The real dead check: a plugin service without an OdbcHelper can't tell, so no hint.
TEST_F(DsqlErrorHintPluginTest, Execute_UnknownConnectionState_LogsNothing) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->execute_ret = SQL_ERROR;
    plugin->diag = std::make_pair(std::string("08S01"), CONNECTION_CLOSED);
    plugin->dead.reset();

    CapturingLogSink sink;
    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(61);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));

    EXPECT_EQ(0, sink.Count(AGE_HINT));
}

TEST_F(DsqlErrorHintPluginTest, AddToChain_WrapsOnlyAuroraDsqlConnections) {
    const std::shared_ptr<BasePlugin> head = next_;
    dbc_->conn_attr = {{KEY_SERVER, AURORA_HOST}};
    EXPECT_TRUE(DsqlErrorHintPlugin::AddToChain(dbc_, head) == head);

    dbc_->conn_attr = {{KEY_SERVER, CLUSTER_ID}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL}};
    EXPECT_TRUE(std::dynamic_pointer_cast<DsqlErrorHintPlugin>(DsqlErrorHintPlugin::AddToChain(dbc_, head)) != nullptr);

    dbc_->conn_attr = {{KEY_SERVER, DSQL_HOST}};
    const std::shared_ptr<BasePlugin> wrapped = DsqlErrorHintPlugin::AddToChain(dbc_, head);
    EXPECT_TRUE(std::dynamic_pointer_cast<DsqlErrorHintPlugin>(wrapped) != nullptr);
    // It delegates to the previous head.
    next_->execute_ret = SQL_SUCCESS_WITH_INFO;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, wrapped->Execute(stmt_, nullptr, SQL_NTS));
}

// A successful connect's warning reaches the application untouched: the DSQL auth plugin below reports an overridden
// SSL mode as SQL_SUCCESS_WITH_INFO with a 01S02 record.
TEST_F(DsqlErrorHintPluginTest, Connect_PassesAWarningAndItsRecordThrough) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->connect_ret = SQL_SUCCESS_WITH_INFO;
    dbc_->err = std::make_unique<ErrInfo>("SSLmode=disable was changed to verify-full", WARN_OPTION_VALUE_CHANGED);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("01S02", dbc_->err->sqlstate);
}

// ODBC 2 applications read a failed statement's record with SQLError, which hands it out once: the hint plugin's own
// read must leave it for them.
TEST_F(DsqlErrorHintPluginTest, Execute_LeavesTheRecordForSqlError) {
    const auto plugin = std::make_shared<TestHintPlugin>(dbc_, next_);
    next_->execute_ret = SQL_ERROR;
    next_->execute_err = ERR_COMMUNICATION_LINK_FAILURE;
    next_->execute_message = CONNECTION_CLOSED;

    ASSERT_EQ(SQL_SUCCESS, plugin->Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    plugin->now += std::chrono::minutes(61);
    EXPECT_EQ(SQL_ERROR, plugin->Execute(stmt_, nullptr, SQL_NTS));

    SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS,
              RDS_SQLError(nullptr, nullptr, stmt_, state, &native_error, message, MAX_MSG_LEN, &length, false));
    EXPECT_EQ(SQL_NO_DATA,
              RDS_SQLError(nullptr, nullptr, stmt_, state, &native_error, message, MAX_MSG_LEN, &length, false));
}
