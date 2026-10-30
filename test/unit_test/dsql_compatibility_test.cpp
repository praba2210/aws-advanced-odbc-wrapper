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
