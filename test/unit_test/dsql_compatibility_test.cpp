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
#include <ng-log/logging.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../driver/driver.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/default_plugin.h"
#include "../../driver/plugin/dsql/dsql_compatibility.h"
#include "../../driver/util/connection_string_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string AURORA_HOST = "database-test-name.cluster-XYZ.us-east-2.rds.amazonaws.com";
    // Behind a PrivateLink endpoint without private DNS, SERVER is the endpoint's address.
    const std::string PRIVATELINK_ADDRESS = "10.0.0.5";
    const std::string PSQLODBC_PATH = "/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so";

    // The connection string the last SQLDriverConnect got, as ASCII.
    std::string captured_conn_str;

    SQLRETURN FakeAllocHandle(SQLSMALLINT, SQLHANDLE, SQLHANDLE* output_handle) {
        *output_handle = static_cast<SQLHANDLE>(&captured_conn_str);
        return SQL_SUCCESS;
    }

    SQLRETURN FakeDriverConnect(SQLHDBC, SQLHWND, SQLTCHAR* in_connection_string, SQLSMALLINT, SQLTCHAR*, SQLSMALLINT,
                                SQLSMALLINT*, SQLUSMALLINT) {
        captured_conn_str.clear();
        for (const SQLTCHAR* c = in_connection_string; *c != 0; ++c) {
            captured_conn_str.push_back(static_cast<char>(*c));
        }
        // The tests only need the string psqlODBC would get.
        return SQL_ERROR;
    }

    // Answers SQLAllocHandle and SQLDriverConnect, records every lookup, and returns nullptr for anything else, so a
    // forwarded call fails without reaching a real driver.
    class FakeBaseDriver : public RdsLibLoader {
    public:
        FUNC_HANDLE GetFunction(const std::string& function_name) override {
            requested_functions.push_back(function_name);
            if (function_name == RDS_STR_SQLAllocHandle) {
                return reinterpret_cast<FUNC_HANDLE>(&FakeAllocHandle);
            }
            if (function_name == RDS_STR_SQLDriverConnect) {
                return reinterpret_cast<FUNC_HANDLE>(&FakeDriverConnect);
            }
            return nullptr;
        }

        bool Requested(const std::string& function_name) const {
            return std::ranges::find(requested_functions, function_name) != requested_functions.end();
        }

        std::vector<std::string> requested_functions;
    };

    // An integer attribute value, passed as ODBC passes it.
    SQLPOINTER AsPointer(SQLULEN value) {
        return reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(value));
    }

    // Collects every log line while it exists.
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

        // How many lines contain the text.
        std::ptrdiff_t Count(const std::string& text) {
            const std::lock_guard<std::mutex> lock(mutex_);
            return std::ranges::count_if(messages_, [&text](const std::string& m) { return m.find(text) != std::string::npos; });
        }

    private:
        std::mutex mutex_;
        std::vector<std::string> messages_;
    };
}  // namespace

class DsqlCompatibilityTest : public testing::Test {
protected:
    std::shared_ptr<FakeBaseDriver> base_driver_ = std::make_shared<FakeBaseDriver>();
    ENV env_;
    DBC dbc_{};
    STMT stmt_{};

    void SetUp() override {
        env_.driver_lib_loader = base_driver_;
        dbc_.env = &env_;
        stmt_.dbc = &dbc_;
        captured_conn_str.clear();
    }

    void TearDown() override {
        // The fake handles point at test objects; never let them outlive the test.
        stmt_.wrapped_stmt = SQL_NULL_HSTMT;
        dbc_.wrapped_dbc = SQL_NULL_HDBC;
        env_.wrapped_env = SQL_NULL_HENV;
        dbc_.plugin_service = nullptr;
    }

    // The attributes RDS_InitializeConnection leaves for this server, and the plugin service it builds.
    void UseServer(const std::string& server) {
        dbc_.conn_attr = {{KEY_SERVER, server}, {KEY_DRIVER, PSQLODBC_PATH}};
        dbc_.plugin_service = std::make_shared<PluginService>(base_driver_, &env_, dbc_.conn_attr, "");
    }

    // Any non-null handles, so the wrapper treats the connection and the statement as open.
    void Connect() {
        dbc_.wrapped_dbc = static_cast<SQLHDBC>(&dbc_);
        stmt_.wrapped_stmt = static_cast<SQLHSTMT>(&stmt_);
    }
};

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_TurnServerSidePrepareOff) {
    std::map<std::string, std::string> conn_attr;
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    EXPECT_EQ("0", conn_attr.at("USESERVERSIDEPREPARE"));
}

// psqlODBC's DSN setup writes UseServerSidePrepare=1 and BASE_DSN copies it into conn_attr, so a value there isn't a
// choice.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_OverrideUserServerSidePrepare) {
    std::map<std::string, std::string> long_form = {{"USESERVERSIDEPREPARE", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(long_form);
    EXPECT_EQ("0", long_form.at("USESERVERSIDEPREPARE"));

    std::map<std::string, std::string> short_form = {{"C8", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("C8"));
    EXPECT_EQ("0", short_form.at("USESERVERSIDEPREPARE"));
}

// CX=1c2305bab: 0x1c flags, bitmask 0x2305bab, which has UseServerSidePrepare (1 << 25) and UseDeclareFetch (1 << 14)
// on. psqlODBC reads the connection string from left to right, so the long keyword must come after CX.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_LongKeyFollowsCxFlags) {
    std::map<std::string, std::string> conn_attr = {{"CX", "1c2305bab"}};
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    const std::string conn_str = ConnectionStringHelper::BuildMinimumConnectionString(conn_attr);
    const size_t cx = conn_str.find("CX=1c2305bab");
    const size_t server_side_prepare = conn_str.find("USESERVERSIDEPREPARE=0");
    ASSERT_NE(std::string::npos, cx);
    ASSERT_NE(std::string::npos, server_side_prepare);
    EXPECT_LT(cx, server_side_prepare);
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_LogOnlyChangedValues) {
    CapturingLogSink log;
    std::map<std::string, std::string> changed = {{"USESERVERSIDEPREPARE", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(changed);
    std::map<std::string, std::string> unchanged = {{"USESERVERSIDEPREPARE", "0"}};
    DsqlCompatibility::ApplyBaseDriverOptions(unchanged);
    EXPECT_EQ(1, log.Count("[DSQL] Overriding UseServerSidePrepare=1 with 0: freeing a prepared statement inside a "
                           "transaction can silently roll the transaction back on Aurora DSQL"));
    EXPECT_EQ(0, log.Count("[DSQL] Overriding UseServerSidePrepare=0"));
}

TEST_F(DsqlCompatibilityTest, IsDsql_FollowsIsDsqlTarget) {
    EXPECT_FALSE(DsqlCompatibility::IsDsql(nullptr));
    EXPECT_FALSE(DsqlCompatibility::IsDsql(&dbc_));  // before the first connect: no attributes yet
    dbc_.conn_attr = {{KEY_SERVER, AURORA_HOST}};
    EXPECT_FALSE(DsqlCompatibility::IsDsql(&dbc_));
    dbc_.conn_attr = {{KEY_SERVER, DSQL_HOST}};
    EXPECT_TRUE(DsqlCompatibility::IsDsql(&dbc_));
    // An explicit non-DSQL dialect doesn't turn the options off: the server still rejects what they avoid.
    dbc_.conn_attr = {{KEY_SERVER, DSQL_HOST}, {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_POSTGRESQL}};
    EXPECT_TRUE(DsqlCompatibility::IsDsql(&dbc_));
    dbc_.conn_attr = {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL}};
    EXPECT_TRUE(DsqlCompatibility::IsDsql(&dbc_));
    dbc_.conn_attr = {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_DSQL}};
    EXPECT_TRUE(DsqlCompatibility::IsDsql(&dbc_));
    // DATABASE auth behind PrivateLink without a dialect: nothing says DSQL, so such connections must set
    // DATABASE_DIALECT=AURORA_DSQL.
    dbc_.conn_attr = {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_IAM_HOST, DSQL_HOST}};
    EXPECT_FALSE(DsqlCompatibility::IsDsql(&dbc_));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_Dsql_TurnsServerSidePrepareOff) {
    UseServer(DSQL_HOST);
    dbc_.conn_attr.insert_or_assign("C8", "1");
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_FALSE(dbc_.conn_attr.contains("C8"));
    EXPECT_EQ("0", dbc_.conn_attr.at("USESERVERSIDEPREPARE"));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherDatabase_ChangesNothing) {
    UseServer(AURORA_HOST);
    dbc_.conn_attr.insert_or_assign("USESERVERSIDEPREPARE", "1");
    const std::map<std::string, std::string> before = dbc_.conn_attr;
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_EQ(before, dbc_.conn_attr);
}

// A reconnect on the same DBC keeps its plugin chain and conn_attr, so PrepareConnect runs again on its own output.
TEST_F(DsqlCompatibilityTest, PrepareConnect_Twice_IsIdempotent) {
    UseServer(DSQL_HOST);
    DsqlCompatibility::PrepareConnect(&dbc_);
    const std::map<std::string, std::string> once = dbc_.conn_attr;
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_EQ(once, dbc_.conn_attr);
}

TEST_F(DsqlCompatibilityTest, DefaultPluginConnect_Dsql_BaseDriverGetsServerSidePrepareOff) {
    UseServer(DSQL_HOST);
    env_.wrapped_env = static_cast<SQLHENV>(&env_);
    DefaultPlugin plugin(&dbc_);
    EXPECT_EQ(SQL_ERROR, plugin.Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_NE(std::string::npos, captured_conn_str.find("SERVER=" + DSQL_HOST));
    EXPECT_NE(std::string::npos, captured_conn_str.find("USESERVERSIDEPREPARE=0"));
}

TEST_F(DsqlCompatibilityTest, DefaultPluginConnect_OtherDatabase_BaseDriverGetsNoDsqlOptions) {
    UseServer(AURORA_HOST);
    env_.wrapped_env = static_cast<SQLHENV>(&env_);
    DefaultPlugin plugin(&dbc_);
    EXPECT_EQ(SQL_ERROR, plugin.Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_NE(std::string::npos, captured_conn_str.find("SERVER=" + AURORA_HOST));
    for (const char* key : {"PROTOCOL=", "IGNORETIMEOUT=", "USEDECLAREFETCH=", "USESERVERSIDEPREPARE="}) {
        EXPECT_EQ(std::string::npos, captured_conn_str.find(key)) << key;
    }
}

namespace {
    // psqlODBC's own connection attributes for three of the forced options (psqlODBC pgapifunc.h).
    constexpr SQLINTEGER PGOPT_USE_DECLAREFETCH = 65539;
    constexpr SQLINTEGER PGOPT_SERVER_SIDE_PREPARE = 65540;
    constexpr SQLINTEGER PGOPT_IGNORETIMEOUT = 65551;
    const std::string SERVER_SIDE_PREPARE_REASON =
        "freeing a prepared statement inside a transaction can silently roll the transaction back on Aurora DSQL";
}  // namespace

// Set on a live connection, the attribute turns server-side prepare back on for new statements, and the wrapper sets
// it again after every reconnect: the transaction loss would be back.
TEST_F(DsqlCompatibilityTest, SetPgoptServerSidePrepare_Dsql_StaysOff) {
    UseServer(DSQL_HOST);
    Connect();
    CapturingLogSink log;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(1), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_STREQ("01S02", dbc_.err->sqlstate);
    EXPECT_EQ("SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE changed to 0: " + SERVER_SIDE_PREPARE_REASON,
              std::string(dbc_.err->error_msg));
    EXPECT_EQ(1, log.Count("[DSQL] SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE changed to 0: " + SERVER_SIDE_PREPARE_REASON
                           + " (requested 1)"));
    EXPECT_FALSE(base_driver_->Requested(RDS_STR_SQLSetConnectAttr));
    EXPECT_FALSE(dbc_.attr_map.contains(PGOPT_SERVER_SIDE_PREPARE));
}

TEST_F(DsqlCompatibilityTest, SetPgoptAttributes_DsqlForcedValues_SucceedLocally) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(0), 0));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, PGOPT_USE_DECLAREFETCH, AsPointer(0), 0));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, PGOPT_IGNORETIMEOUT, AsPointer(1), 0));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_FALSE(base_driver_->Requested(RDS_STR_SQLSetConnectAttr));
    EXPECT_TRUE(dbc_.attr_map.empty());
}

TEST_F(DsqlCompatibilityTest, SetPgoptDeclareFetchAndIgnoreTimeout_Dsql_KeepTheForcedValues) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, PGOPT_USE_DECLAREFETCH, AsPointer(1), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_STREQ("SQL_ATTR_PGOPT_USE_DECLAREFETCH changed to 0: Aurora DSQL doesn't support cursors", dbc_.err->error_msg);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, PGOPT_IGNORETIMEOUT, AsPointer(0), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_STREQ("SQL_ATTR_PGOPT_IGNORETIMEOUT changed to 1: Aurora DSQL doesn't support statement_timeout",
                 dbc_.err->error_msg);
    EXPECT_FALSE(base_driver_->Requested(RDS_STR_SQLSetConnectAttr));
    EXPECT_TRUE(dbc_.attr_map.empty());
}

TEST_F(DsqlCompatibilityTest, SetPgoptServerSidePrepare_OtherDatabase_ReachesBaseDriver) {
    UseServer(AURORA_HOST);
    Connect();
    RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(1), 0);
    EXPECT_TRUE(base_driver_->Requested(RDS_STR_SQLSetConnectAttr));
}

// Before the first connect nothing says DSQL yet: the value waits in attr_map for PrepareConnect.
TEST_F(DsqlCompatibilityTest, SetPgoptServerSidePrepare_BeforeConnect_IsKept) {
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(1), 0));
    EXPECT_TRUE(dbc_.attr_map.contains(PGOPT_SERVER_SIDE_PREPARE));
}

// DefaultPlugin::Connect would set a value from attr_map again after psqlODBC connects, over USESERVERSIDEPREPARE=0.
TEST_F(DsqlCompatibilityTest, PrepareConnect_Dsql_DropsPgoptAttributesSetBeforeConnect) {
    UseServer(DSQL_HOST);
    CapturingLogSink log;
    dbc_.attr_map.insert_or_assign(PGOPT_SERVER_SIDE_PREPARE, std::make_pair(AsPointer(1), 0));
    dbc_.attr_map.insert_or_assign(PGOPT_USE_DECLAREFETCH, std::make_pair(AsPointer(0), 0));
    dbc_.attr_map.insert_or_assign(PGOPT_IGNORETIMEOUT, std::make_pair(AsPointer(1), 0));
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_TRUE(dbc_.attr_map.empty());
    EXPECT_EQ(1, log.Count("[DSQL] Ignoring SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE=1 set before connect: "
                           + SERVER_SIDE_PREPARE_REASON));
    EXPECT_EQ(1, log.Count("[DSQL] Ignoring "));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherDatabase_KeepsPgoptAttributes) {
    UseServer(AURORA_HOST);
    dbc_.attr_map.insert_or_assign(PGOPT_SERVER_SIDE_PREPARE, std::make_pair(AsPointer(1), 0));
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_TRUE(dbc_.attr_map.contains(PGOPT_SERVER_SIDE_PREPARE));
}

// Between SQLDisconnect and the next connect conn_attr still describes the Aurora DSQL connection, so the value is
// answered as on an open connection and never reaches attr_map, which the next connect would set again.
TEST_F(DsqlCompatibilityTest, SetPgoptServerSidePrepare_DsqlHandleBetweenConnects_IsNotKept) {
    UseServer(DSQL_HOST);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(1), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_STREQ("01S02", dbc_.err->sqlstate);
    EXPECT_FALSE(base_driver_->Requested(RDS_STR_SQLSetConnectAttr));
    EXPECT_FALSE(dbc_.attr_map.contains(PGOPT_SERVER_SIDE_PREPARE));
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_SetEveryOption) {
    std::map<std::string, std::string> conn_attr;
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    const std::map<std::string, std::string> expected = {
        {"IGNORETIMEOUT", "1"}, {"PROTOCOL", "7.4-0"}, {"USEDECLAREFETCH", "0"}, {"USESERVERSIDEPREPARE", "0"}};
    EXPECT_EQ(expected, conn_attr);
}

// No "-<level>" means the default level, which sends SAVEPOINT; levels 1 and 2 send it too.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_OverrideUserProtocol) {
    for (const char* protocol : {"7.4", "7.4-2", "7.4-1"}) {
        std::map<std::string, std::string> conn_attr = {{"PROTOCOL", protocol}};
        DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
        EXPECT_EQ("7.4-0", conn_attr.at("PROTOCOL")) << protocol;
    }
    std::map<std::string, std::string> short_form = {{"A1", "7.4-1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("A1"));
    EXPECT_EQ("7.4-0", short_form.at("PROTOCOL"));
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_OverrideUserIgnoreTimeout) {
    std::map<std::string, std::string> long_form = {{"IGNORETIMEOUT", "0"}};
    DsqlCompatibility::ApplyBaseDriverOptions(long_form);
    EXPECT_EQ("1", long_form.at("IGNORETIMEOUT"));

    std::map<std::string, std::string> short_form = {{"D9", "0"}};
    DsqlCompatibility::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("D9"));
    EXPECT_EQ("1", short_form.at("IGNORETIMEOUT"));
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_OverrideUserDeclareFetch) {
    std::map<std::string, std::string> long_form = {{"USEDECLAREFETCH", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(long_form);
    EXPECT_EQ("0", long_form.at("USEDECLAREFETCH"));

    std::map<std::string, std::string> short_form = {{"B6", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("B6"));
    EXPECT_EQ("0", short_form.at("USEDECLAREFETCH"));
}

// What psqlODBC gets: each option once, under its long keyword, after the CX flags.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_BaseDriverStringHasOnlyLongKeysAfterCx) {
    std::map<std::string, std::string> conn_attr = {
        {"A1", "7.4-2"}, {"D9", "0"}, {"B6", "1"}, {"C8", "1"}, {"CX", "1c2305bab"}};
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    EXPECT_EQ("CX=1c2305bab;IGNORETIMEOUT=1;PROTOCOL=7.4-0;USEDECLAREFETCH=0;USESERVERSIDEPREPARE=0",
              ConnectionStringHelper::BuildMinimumConnectionString(conn_attr));
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_LogEachOverride) {
    CapturingLogSink log;
    std::map<std::string, std::string> user_values = {{"PROTOCOL", "7.4"}, {"IGNORETIMEOUT", "0"}, {"B6", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(user_values);
    std::map<std::string, std::string> already_right = {
        {"PROTOCOL", "7.4-0"}, {"IGNORETIMEOUT", "1"}, {"USEDECLAREFETCH", "0"}};
    DsqlCompatibility::ApplyBaseDriverOptions(already_right);
    EXPECT_EQ(1, log.Count("[DSQL] Overriding Protocol=7.4 with 7.4-0: Aurora DSQL doesn't support savepoints"));
    EXPECT_EQ(1, log.Count("[DSQL] Overriding IgnoreTimeout=0 with 1: Aurora DSQL doesn't support statement_timeout"));
    EXPECT_EQ(1, log.Count("[DSQL] Overriding UseDeclareFetch=1 with 0: Aurora DSQL doesn't support cursors"));
    EXPECT_EQ(3, log.Count("[DSQL] Overriding "));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_Dsql_SetsEveryOption) {
    UseServer(DSQL_HOST);
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_EQ("7.4-0", dbc_.conn_attr.at("PROTOCOL"));
    EXPECT_EQ("1", dbc_.conn_attr.at("IGNORETIMEOUT"));
    EXPECT_EQ("0", dbc_.conn_attr.at("USEDECLAREFETCH"));
    EXPECT_EQ("0", dbc_.conn_attr.at("USESERVERSIDEPREPARE"));
}

// A connection string as users write it: psqlODBC's mixed-case keywords and its short forms. The wrapper's parser
// upper-cases every key, so each form is replaced.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_ReplaceOptionsAsUsersWriteThem) {
    std::map<std::string, std::string> conn_attr;
    ConnectionStringHelper::ParseConnectionString("UseServerSidePrepare=1;Protocol=7.4;usedeclarefetch=1;D9=0;", conn_attr);
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    EXPECT_EQ("IGNORETIMEOUT=1;PROTOCOL=7.4-0;USEDECLAREFETCH=0;USESERVERSIDEPREPARE=0",
              ConnectionStringHelper::BuildMinimumConnectionString(conn_attr));
}

TEST_F(DsqlCompatibilityTest, SetQueryTimeout_Dsql_ReturnsOptionValueChanged) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(30), 0));
    ASSERT_NE(nullptr, stmt_.err);
    EXPECT_STREQ("01S02", stmt_.err->sqlstate);
    EXPECT_STREQ("SQL_ATTR_QUERY_TIMEOUT changed to 0: Aurora DSQL doesn't support query timeouts (it limits each "
                 "transaction to 5 minutes)", stmt_.err->error_msg);
    EXPECT_FALSE(base_driver_->Requested(RDS_STR_SQLSetStmtAttr));
    EXPECT_FALSE(stmt_.attr_map.contains(SQL_ATTR_QUERY_TIMEOUT));
}

TEST_F(DsqlCompatibilityTest, SetQueryTimeout_DsqlZero_ReachesBaseDriver) {
    UseServer(DSQL_HOST);
    Connect();
    RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(0), 0);
    EXPECT_TRUE(base_driver_->Requested(RDS_STR_SQLSetStmtAttr));
}

TEST_F(DsqlCompatibilityTest, SetQueryTimeout_OtherDatabase_ReachesBaseDriver) {
    UseServer(AURORA_HOST);
    Connect();
    RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(30), 0);
    EXPECT_TRUE(base_driver_->Requested(RDS_STR_SQLSetStmtAttr));
}

// ODBC 2 sets the timeout for later statements on the connection.
TEST_F(DsqlCompatibilityTest, SetConnectQueryTimeout_Dsql_ReturnsOptionValueChanged) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectOption(&dbc_, SQL_QUERY_TIMEOUT, 30));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_STREQ("01S02", dbc_.err->sqlstate);
    EXPECT_FALSE(base_driver_->Requested(RDS_STR_SQLSetConnectAttr));
    EXPECT_FALSE(dbc_.attr_map.contains(SQL_QUERY_TIMEOUT));
}

TEST_F(DsqlCompatibilityTest, SetConnectAttr_Dsql_OtherAttributesReachBaseDriver) {
    UseServer(DSQL_HOST);
    Connect();
    RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_OFF), 0);
    EXPECT_TRUE(base_driver_->Requested(RDS_STR_SQLSetConnectAttr));
    EXPECT_FALSE(dbc_.auto_commit);
}
