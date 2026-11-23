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
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../driver/driver.h"
#include "../../driver/error.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/dsql/dsql_auth_plugin.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/dsql_connection_validator.h"
#include "../../driver/util/logger_wrapper.h"
#include "../../driver/util/rds_lib_loader.h"

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-west-2.on.aws";
    const std::string AURORA_HOST = "database-1.cluster-abcdefghijkl.us-west-2.rds.amazonaws.com";
    const std::string RDS_HOST = "database-1.abcdefghijkl.us-west-2.rds.amazonaws.com";
    // Behind a PrivateLink endpoint without private DNS, SERVER is the endpoint's address and IAM_HOST the DSQL name.
    const std::string PRIVATELINK_ADDRESS = "10.0.0.5";
    const std::string CLUSTER_ID = "abcdefghijklmnopqrstuvwxyz";
    const std::string MISSING_DRIVER = "/nonexistent/path/no-such-driver.so";

    std::string PluginsError(const std::string& keys, const std::string& reason) {
        return "Aurora DSQL doesn't support these plugins, which need Aurora or RDS cluster features: " + keys
            + ". Set them to 0. The driver treats this connection as Aurora DSQL because " + reason + ".";
    }

    std::string AuthTypeError(const std::string& auth_type, const std::string& host_key) {
        return "RDS_AUTH_TYPE=" + auth_type + " doesn't work with Aurora DSQL: it signs an RDS IAM token, which Aurora "
            "DSQL rejects (" + host_key + " is an Aurora DSQL endpoint). Use RDS_AUTH_TYPE=DSQL, which signs an Aurora "
            "DSQL token with the credentials of AWS_PROFILE or the default credential chain; for an AWS IAM Identity "
            "Center (SSO) profile, run 'aws sso login' first.";
    }

    // Collects the WARNING lines while it exists.
    class WarningSink : public nglog::LogSink {
    public:
        WarningSink() { nglog::AddLogSink(this); }
        ~WarningSink() override { nglog::RemoveLogSink(this); }
        WarningSink(const WarningSink&) = delete;
        WarningSink& operator=(const WarningSink&) = delete;

        void send(nglog::LogSeverity severity, const char* /*full_filename*/, const char* /*base_filename*/,
                  int /*line*/, const nglog::LogMessageTime& /*time*/, const char* message, size_t message_len) override {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (severity == nglog::NGLOG_WARNING) {
                messages_.emplace_back(message, message_len);
            }
        }

        long Count(const std::string& text) {
            const std::lock_guard<std::mutex> lock(mutex_);
            return std::ranges::count_if(messages_, [&text](const std::string& m) { return m.find(text) != std::string::npos; });
        }

    private:
        std::mutex mutex_;
        std::vector<std::string> messages_;
    };
}  // namespace

TEST(DsqlConnectionValidatorTest, IsDsqlTarget_EachDsqlSetting) {
    const std::vector<std::map<std::string, std::string>> targets = {
        {{KEY_SERVER, DSQL_HOST}},
        {{KEY_SERVER, "ABCDEFGHIJKLMNOPQRSTUVWXYZ.DSQL.US-WEST-2.ON.AWS"}},
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_AUTH_TYPE, "dsql"}},
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_DATABASE_DIALECT, "aurora_dsql"}},
        // An explicit other dialect doesn't hide a DSQL endpoint.
        {{KEY_SERVER, DSQL_HOST}, {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_POSTGRESQL}},
    };
    for (size_t i = 0; i < targets.size(); ++i) {
        EXPECT_TRUE(DsqlConnectionValidator::IsDsqlTarget(targets[i])) << "case " << i;
    }
}

TEST(DsqlConnectionValidatorTest, Validate_RejectsEachAuroraOnlyPlugin) {
    for (const std::string key : {KEY_ENABLE_LIMITLESS, KEY_ENABLE_FAILOVER, KEY_ENABLE_SRW_SPLIT, KEY_ENABLE_RW_SPLIT,
             KEY_ENABLE_CUSTOM_ENDPOINT, KEY_ENABLE_BLUE_GREEN}) {
        EXPECT_EQ(PluginsError(key, "RDS_AUTH_TYPE=DSQL"),
                  DsqlConnectionValidator::Validate(
                      {{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL}, {key, VALUE_BOOL_TRUE}}));
    }
}

TEST(DsqlConnectionValidatorTest, Validate_RejectsAuthTypesThatSignRdsTokens) {
    for (const std::string auth_type : {VALUE_AUTH_IAM, VALUE_AUTH_ADFS, VALUE_AUTH_OKTA, VALUE_AUTH_SSO}) {
        EXPECT_EQ(AuthTypeError(auth_type, "SERVER"),
                  DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, auth_type}}));
    }
    // Any case, like the plugin builder; the message names the type upper-cased.
    EXPECT_EQ(AuthTypeError("AWS_SSO", "SERVER"),
              DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, "aws_sso"}}));
}

TEST(DsqlConnectionValidatorTest, Validate_RejectsRdsTokensForADsqlIamHost) {
    // PrivateLink without private DNS: the IAM plugin signs for IAM_HOST.
    EXPECT_EQ(AuthTypeError("IAM", "IAM_HOST"),
              DsqlConnectionValidator::Validate({{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_IAM_HOST, DSQL_HOST},
                  {KEY_AUTH_TYPE, VALUE_AUTH_IAM}}));
}

TEST(DsqlConnectionValidatorTest, Validate_WarnsAboutAnotherDialectOnDsql) {
    WarningSink sink;
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST},
        {KEY_DATABASE_DIALECT, "aurora_postgresql"}}));
    EXPECT_EQ(1, sink.Count("[DSQL] DATABASE_DIALECT=aurora_postgresql on an Aurora DSQL connection (SERVER is an "
                            "Aurora DSQL endpoint): the driver uses that dialect"));
    // None for another database.
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, AURORA_HOST},
        {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_POSTGRESQL}}));
    EXPECT_EQ(1, sink.Count("on an Aurora DSQL connection"));
}

// Settings that aren't Aurora DSQL pass unchanged, Aurora-only plugins and RDS tokens included.
TEST(DsqlConnectionValidatorTest, CheckSettings_OtherDatabase_ChangesNothing) {
    const std::vector<std::map<std::string, std::string>> others = {
        {{KEY_SERVER, AURORA_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_IAM}, {KEY_ENABLE_FAILOVER, VALUE_BOOL_TRUE}},
        // Without RDS_AUTH_TYPE=DSQL a cluster ID is a host name, and SERVERNAME is left to the base driver.
        {{KEY_SERVER, CLUSTER_ID}, {ALIAS_KEY_SERVER_1, RDS_HOST}, {KEY_REGION, "us-west-2"}},
    };
    for (size_t i = 0; i < others.size(); ++i) {
        DBC dbc;
        dbc.conn_attr = others[i];
        EXPECT_TRUE(DsqlConnectionValidator::CheckSettings(&dbc)) << "case " << i;
        EXPECT_EQ(others[i], dbc.conn_attr) << "case " << i;
        EXPECT_FALSE(dbc.err) << "case " << i;
    }
}

class DsqlConnectionValidatorInitTest : public testing::Test {
protected:
    ENV* env_ = nullptr;
    DBC* dbc_ = nullptr;

    void SetUp() override {
        env_ = new ENV();
        dbc_ = new DBC();
        dbc_->env = env_;
    }

    void TearDown() override {
        delete dbc_;
        delete env_;
    }
};

// The check runs before the base driver is loaded: the missing driver is never reached, and nothing is created.
TEST_F(DsqlConnectionValidatorInitTest, AuroraOnlyPluginOnDsql_Fails08001BeforeTheDriverLoad) {
    dbc_->conn_attr = {{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL},
        {KEY_ENABLE_FAILOVER, VALUE_BOOL_TRUE}, {KEY_BASE_DRIVER, MISSING_DRIVER}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ(PluginsError("ENABLE_CLUSTER_FAILOVER", "RDS_AUTH_TYPE=DSQL"), dbc_->err->error_msg);
    EXPECT_FALSE(env_->driver_lib_loader);
    EXPECT_FALSE(dbc_->plugin_service);
}

// The cluster ID is expanded before the base driver is loaded, and a SERVER/SERVERNAME conflict fails there.
TEST_F(DsqlConnectionValidatorInitTest, ClusterId_IsExpandedBeforeTheDriverLoad) {
    dbc_->conn_attr = {{KEY_AUTH_TYPE, VALUE_AUTH_DSQL}, {KEY_SERVER, CLUSTER_ID}, {KEY_REGION, "eu-west-1"},
        {KEY_BASE_DRIVER, MISSING_DRIVER}};
    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", dbc_->conn_attr.at(KEY_SERVER));
    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ("IM003", dbc_->err->sqlstate);

    dbc_->conn_attr.insert_or_assign(ALIAS_KEY_SERVER_1, RDS_HOST);
    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));
    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ("DSQL Authentication failed. SERVER and SERVERNAME name different hosts ('" + CLUSTER_ID
              + ".dsql.eu-west-1.on.aws' and '" + RDS_HOST + "'): set only one of them", dbc_->err->error_msg);
}

// RDS_InitializeConnection up to a working chain: the environment already holds a loader for the driver's path, which
// doesn't load, and a base environment handle.
class DsqlConnectionValidatorReuseTest : public testing::Test {
protected:
    const char* const FAKE_DRIVER_PATH = "/nonexistent/validator-test-driver.so";
    ENV* env_ = nullptr;
    DBC* dbc_ = nullptr;
    int base_env_stand_in_ = 0;

    void SetUp() override {
        env_ = new ENV();
        env_->driver_lib_loader = std::make_shared<RdsLibLoader>(FAKE_DRIVER_PATH);
        env_->wrapped_env = static_cast<SQLHENV>(&base_env_stand_in_);
        dbc_ = new DBC();
        dbc_->env = env_;
    }

    void TearDown() override {
        delete dbc_;
        delete env_;
    }

    // A later connect's string replaces SERVER and keeps the other keys, as the connection string parser does.
    SQLRETURN ConnectTo(const std::string& server) {
        dbc_->conn_attr.insert_or_assign(KEY_BASE_DRIVER, FAKE_DRIVER_PATH);
        dbc_->conn_attr.insert_or_assign(KEY_SERVER, server);
        return RDS_InitializeConnection(dbc_, "");
    }
};

TEST_F(DsqlConnectionValidatorReuseTest, ReusedHandle_OtherThenDsql_Fails08001) {
    ASSERT_EQ(SQL_SUCCESS, ConnectTo("localhost"));
    ASSERT_EQ(SQL_SUCCESS, ConnectTo(RDS_HOST));

    EXPECT_EQ(SQL_ERROR, ConnectTo(DSQL_HOST));
    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ("This connection handle was first used to connect to another database, and the driver treats this "
              "connection as Aurora DSQL because SERVER is an Aurora DSQL endpoint. Allocate a new connection handle "
              "to connect to Aurora DSQL.", dbc_->err->error_msg);
}

// SERVER was the only Aurora DSQL setting, so the new string drops every one.
TEST_F(DsqlConnectionValidatorReuseTest, ReusedHandle_DsqlThenOther_Fails08001) {
    ASSERT_EQ(SQL_SUCCESS, ConnectTo(DSQL_HOST));

    EXPECT_EQ(SQL_ERROR, ConnectTo("localhost"));
    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ("This connection handle was first used to connect to Aurora DSQL, and these settings aren't Aurora DSQL: "
              "none of RDS_AUTH_TYPE=DSQL, an Aurora DSQL endpoint as SERVER, or DATABASE_DIALECT=AURORA_DSQL is set. "
              "Allocate a new connection handle to connect to another database.", dbc_->err->error_msg);
}

// RDS_AUTH_TYPE=DSQL builds the Aurora DSQL auth plugin, with the compatibility plugin below it: the auth plugin
// forwards the isolation info, and the compatibility plugin answers it.
TEST_F(DsqlConnectionValidatorReuseTest, InitializeConnection_DsqlAuth_BuildsTheDsqlPlugins) {
    dbc_->conn_attr.insert_or_assign(KEY_AUTH_TYPE, "DSQL");
    ASSERT_EQ(SQL_SUCCESS, ConnectTo(DSQL_HOST));
    EXPECT_NE(nullptr, dynamic_cast<DsqlAuthPlugin*>(dbc_->plugin_head));

    int base_dbc_stand_in = 0;
    dbc_->wrapped_dbc = static_cast<SQLHDBC>(&base_dbc_stand_in);
    SQLUINTEGER value = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(dbc_, SQL_TXN_ISOLATION_OPTION, &value,
                                          static_cast<SQLSMALLINT>(sizeof(value)), nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_TXN_REPEATABLE_READ), value);
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
}
