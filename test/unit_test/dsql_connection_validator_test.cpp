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
#include <mutex>
#include <string>
#include <vector>

#include "../../driver/driver.h"
#include "../../driver/error.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/dsql_connection_validator.h"
#include "../../driver/util/logger_wrapper.h"

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-west-2.on.aws";
    const std::string AURORA_HOST = "database-1.cluster-abcdefghijkl.us-west-2.rds.amazonaws.com";
    const std::string RDS_HOST = "database-1.abcdefghijkl.us-west-2.rds.amazonaws.com";
    // Behind a PrivateLink endpoint without private DNS, SERVER is the endpoint's address and IAM_HOST the DSQL name.
    const std::string PRIVATELINK_ADDRESS = "10.0.0.5";
    // A bare cluster ID. Under RDS_AUTH_TYPE=DSQL the driver may expand it to an endpoint before these checks run.
    const std::string CLUSTER_ID = "abcdefghijklmnopqrstuvwxyz";
    const std::string MISSING_DRIVER = "/nonexistent/path/no-such-driver.so";

    const std::vector<std::string> AURORA_ONLY_PLUGIN_KEYS = {
        KEY_ENABLE_LIMITLESS,
        KEY_ENABLE_FAILOVER,
        KEY_ENABLE_SRW_SPLIT,
        KEY_ENABLE_RW_SPLIT,
        KEY_ENABLE_CUSTOM_ENDPOINT,
        KEY_ENABLE_BLUE_GREEN,
    };

    const std::string PLUGINS_ERROR =
        "Aurora DSQL doesn't support these plugins, which need Aurora or RDS cluster features: ";

    std::string PluginsError(const std::string& keys, const std::string& reason) {
        return PLUGINS_ERROR + keys + ". Set them to 0. The driver treats this connection as Aurora DSQL because "
            + reason + ".";
    }

    std::string AuthTypeError(const std::string& auth_type, const std::string& host_key) {
        return "RDS_AUTH_TYPE=" + auth_type + " doesn't work with Aurora DSQL: it signs an RDS IAM token, which Aurora "
            "DSQL rejects (" + host_key + " is an Aurora DSQL endpoint). Use RDS_AUTH_TYPE=DSQL, which signs an Aurora "
            "DSQL token with the credentials of AWS_PROFILE or the default credential chain; for an AWS IAM Identity "
            "Center (SSO) profile, run 'aws sso login' first.";
    }

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
}  // namespace

TEST(DsqlConnectionValidatorTest, IsDsqlTarget_EachDsqlSetting) {
    const std::vector<std::map<std::string, std::string>> targets = {
        {{KEY_SERVER, DSQL_HOST}},
        {{KEY_SERVER, "ABCDEFGHIJKLMNOPQRSTUVWXYZ.DSQL.US-WEST-2.ON.AWS"}},
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_AUTH_TYPE, "dsql"}},
        // A cluster ID, expanded or not: the auth type decides.
        {{KEY_SERVER, CLUSTER_ID}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL}},
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_DATABASE_DIALECT, "aurora_dsql"}},
        // The DSQL dialect chosen for a host without topology: the user said it's DSQL.
        {{KEY_SERVER, RDS_HOST}, {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_DSQL}},
        // An explicit other dialect doesn't hide a DSQL endpoint.
        {{KEY_SERVER, DSQL_HOST}, {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_POSTGRESQL}},
    };
    for (size_t i = 0; i < targets.size(); ++i) {
        EXPECT_TRUE(DsqlConnectionValidator::IsDsqlTarget(targets[i])) << "case " << i;
    }
}

TEST(DsqlConnectionValidatorTest, IsDsqlTarget_NoDsqlSetting) {
    const std::vector<std::map<std::string, std::string>> others = {
        {},
        {{KEY_SERVER, AURORA_HOST}},
        {{KEY_SERVER, AURORA_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_IAM}, {KEY_IAM_HOST, AURORA_HOST}},
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_POSTGRESQL}},
        // IAM_HOST doesn't count: only the token auth plugins read it (a leftover one here).
        {{KEY_SERVER, AURORA_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}, {KEY_IAM_HOST, DSQL_HOST}},
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_IAM_HOST, DSQL_HOST}},
        // A cluster ID under another auth type is a host name: only RDS_AUTH_TYPE=DSQL treats it as a cluster ID.
        {{KEY_SERVER, CLUSTER_ID}, {KEY_AUTH_TYPE, VALUE_AUTH_IAM}},
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_DATABASE_DIALECT, "foo"}},
    };
    for (size_t i = 0; i < others.size(); ++i) {
        EXPECT_FALSE(DsqlConnectionValidator::IsDsqlTarget(others[i])) << "case " << i;
    }
}

TEST(DsqlConnectionValidatorTest, Validate_RejectsEachAuroraOnlyPlugin) {
    for (const std::string& key : AURORA_ONLY_PLUGIN_KEYS) {
        EXPECT_EQ(PluginsError(key, "RDS_AUTH_TYPE=DSQL"),
                  DsqlConnectionValidator::Validate(
                      {{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL}, {key, VALUE_BOOL_TRUE}}));
    }
}

TEST(DsqlConnectionValidatorTest, Validate_ListsEveryEnabledPluginInBuilderOrder) {
    EXPECT_EQ(PluginsError("ENABLE_CLUSTER_FAILOVER, ENABLE_RW_SPLIT, ENABLE_BLUE_GREEN",
                           "SERVER is an Aurora DSQL endpoint"),
              DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_ENABLE_BLUE_GREEN, VALUE_BOOL_TRUE},
                  {KEY_ENABLE_RW_SPLIT, VALUE_BOOL_TRUE}, {KEY_ENABLE_FAILOVER, VALUE_BOOL_TRUE}}));
}

TEST(DsqlConnectionValidatorTest, Validate_NamesWhyTheConnectionIsDsql) {
    // The DSQL dialect on a non-DSQL host: failover can't work with a dialect that has no topology.
    EXPECT_EQ(PluginsError("ENABLE_CLUSTER_FAILOVER", "DATABASE_DIALECT=AURORA_DSQL"),
              DsqlConnectionValidator::Validate({{KEY_SERVER, RDS_HOST}, {KEY_DATABASE_DIALECT, "aurora_dsql"},
                  {KEY_ENABLE_FAILOVER, VALUE_BOOL_TRUE}}));
    // The auth type comes before the host, so a cluster ID gives the same reason whether or not SERVER was expanded.
    EXPECT_EQ(PluginsError("ENABLE_LIMITLESS", "RDS_AUTH_TYPE=DSQL"),
              DsqlConnectionValidator::Validate({{KEY_SERVER, CLUSTER_ID}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL},
                  {KEY_ENABLE_LIMITLESS, VALUE_BOOL_TRUE}}));
}

// The builder builds a plugin only for the value 1. Anything else leaves it off, so it isn't rejected either.
TEST(DsqlConnectionValidatorTest, Validate_AllowsPluginsTheBuilderLeavesOff) {
    for (const std::string value : {VALUE_BOOL_FALSE, "true", " 1", ""}) {
        EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_ENABLE_FAILOVER, value}}))
            << "value '" << value << "'";
    }
    // Other ENABLE_ keys pass: the DSQL OCC retry plugin's, and the initial connection strategy, which steps aside
    // for any host that isn't an Aurora cluster endpoint. Plugin settings pass too.
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {"ENABLE_OCC_RETRY", VALUE_BOOL_TRUE},
        {KEY_ENABLE_AURORA_INITIAL_CONNECTION_STRATEGY, VALUE_BOOL_TRUE},
        {KEY_FAILOVER_MODE, VALUE_FAILOVER_MODE_STRICT_WRITER}}));
}

TEST(DsqlConnectionValidatorTest, Validate_IgnoresOtherDatabases) {
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, AURORA_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_IAM},
        {KEY_ENABLE_FAILOVER, VALUE_BOOL_TRUE}, {KEY_ENABLE_LIMITLESS, VALUE_BOOL_TRUE}}));
    // A leftover DSQL IAM_HOST, which DATABASE auth never reads.
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, AURORA_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE},
        {KEY_IAM_HOST, DSQL_HOST}, {KEY_ENABLE_FAILOVER, VALUE_BOOL_TRUE}}));
}

TEST(DsqlConnectionValidatorTest, Validate_RejectsAuthTypesThatSignRdsTokens) {
    for (const std::string auth_type : {VALUE_AUTH_IAM, VALUE_AUTH_ADFS, VALUE_AUTH_OKTA, VALUE_AUTH_SSO}) {
        EXPECT_EQ(AuthTypeError(auth_type, "SERVER"),
                  DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, auth_type}}));
    }
    // Any case, like the plugin builder; the message names the type upper-cased.
    EXPECT_EQ(AuthTypeError("AWS_SSO", "SERVER"),
              DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, "aws_sso"}}));
    // The dialect doesn't matter: an RDS token never works against DSQL.
    EXPECT_EQ(AuthTypeError("IAM", "SERVER"),
              DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_IAM},
                  {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_POSTGRESQL}}));
}

TEST(DsqlConnectionValidatorTest, Validate_RejectsRdsTokensForADsqlIamHost) {
    // PrivateLink without private DNS: the IAM plugin signs for IAM_HOST.
    EXPECT_EQ(AuthTypeError("IAM", "IAM_HOST"),
              DsqlConnectionValidator::Validate({{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_IAM_HOST, DSQL_HOST},
                  {KEY_AUTH_TYPE, VALUE_AUTH_IAM}}));
    // The token is signed for the DSQL name, not for SERVER, so it can't work either.
    EXPECT_EQ(AuthTypeError("ADFS", "IAM_HOST"),
              DsqlConnectionValidator::Validate({{KEY_SERVER, AURORA_HOST}, {KEY_IAM_HOST, DSQL_HOST},
                  {KEY_AUTH_TYPE, VALUE_AUTH_ADFS}}));
}

TEST(DsqlConnectionValidatorTest, Validate_AllowsRdsTokensWithoutADsqlHost) {
    // The DSQL dialect picked for plain RDS PostgreSQL (no topology monitor), with RDS IAM tokens, which RDS accepts.
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, RDS_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_IAM},
        {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_DSQL}}));
    // A cluster ID under IAM is a host name; the IAM plugin reports what's missing.
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, CLUSTER_ID}, {KEY_AUTH_TYPE, VALUE_AUTH_IAM}}));
}

TEST(DsqlConnectionValidatorTest, Validate_AllowsDsqlAndDatabaseAuth) {
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL}}));
    // DATABASE, or no auth type: the app passes a token it generated as PWD.
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}}));
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}}));
}

TEST(DsqlConnectionValidatorTest, Validate_SecretsManagerLogsAnInfoLine) {
    CapturingLogSink sink;
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_SECRETS}}));
    EXPECT_EQ(1, sink.Count("the secret's password must be a current Aurora DSQL token"));
}

TEST(DsqlConnectionValidatorTest, Validate_WarnsAboutAnotherDialectOnDsql) {
    CapturingLogSink sink;
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST},
        {KEY_DATABASE_DIALECT, "aurora_postgresql"}}));
    EXPECT_EQ(1, sink.Count("[DSQL] DATABASE_DIALECT=AURORA_POSTGRESQL on an Aurora DSQL connection (SERVER is an "
                            "Aurora DSQL endpoint): the driver uses that dialect"));
    // None for the DSQL dialect, an unknown value (the driver ignores it), or another database.
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST},
        {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_DSQL}}));
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST}, {KEY_DATABASE_DIALECT, "foo"}}));
    EXPECT_EQ("", DsqlConnectionValidator::Validate({{KEY_SERVER, AURORA_HOST},
        {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_POSTGRESQL}}));
    EXPECT_EQ(1, sink.Count("on an Aurora DSQL connection"));
}

TEST(DsqlConnectionValidatorTest, Validate_AuthTypeErrorComesFirst) {
    const std::string error = DsqlConnectionValidator::Validate({{KEY_SERVER, DSQL_HOST},
        {KEY_AUTH_TYPE, VALUE_AUTH_IAM}, {KEY_ENABLE_FAILOVER, VALUE_BOOL_TRUE}});
    EXPECT_TRUE(error.starts_with("RDS_AUTH_TYPE=IAM doesn't work")) << error;
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

    CapturingLogSink sink;
    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ(static_cast<SQLINTEGER>(ERR_CLIENT_UNABLE_TO_ESTABLISH_CONNECTION), dbc_->err->native_err);
    EXPECT_EQ(PluginsError("ENABLE_CLUSTER_FAILOVER", "RDS_AUTH_TYPE=DSQL"), std::string(dbc_->err->error_msg));
    EXPECT_EQ(1, sink.Count("[DSQL] " + PLUGINS_ERROR));
    EXPECT_FALSE(env_->driver_lib_loader);
    EXPECT_FALSE(dbc_->plugin_service);
}

TEST_F(DsqlConnectionValidatorInitTest, RdsTokenAuthOnDsql_Fails08001) {
    dbc_->conn_attr = {{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_IAM}, {KEY_BASE_DRIVER, MISSING_DRIVER}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ(AuthTypeError("IAM", "SERVER"), std::string(dbc_->err->error_msg));
}

// The attribute validator runs first, so an invalid value is reported with its own message.
TEST_F(DsqlConnectionValidatorInitTest, InvalidValueIsReportedBeforeTheDsqlChecks) {
    dbc_->conn_attr = {{KEY_SERVER, DSQL_HOST}, {KEY_ENABLE_FAILOVER, VALUE_BOOL_TRUE}, {KEY_PORT, "-1"},
        {KEY_BASE_DRIVER, MISSING_DRIVER}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("01S00", dbc_->err->sqlstate);
}

// Settings that work go on to the driver load, which fails here on the missing driver.
TEST_F(DsqlConnectionValidatorInitTest, ValidDsqlSettings_ReachTheDriverLoad) {
    dbc_->conn_attr = {{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL}, {KEY_BASE_DRIVER, MISSING_DRIVER}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("IM003", dbc_->err->sqlstate);
}

// Holds whether or not the driver expands the cluster ID first (REGION is set): the reason is the auth type either
// way.
TEST_F(DsqlConnectionValidatorInitTest, ClusterIdWithDsqlAuth_PluginsRejectedWithOrWithoutTheExpansion) {
    dbc_->conn_attr = {{KEY_SERVER, CLUSTER_ID}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL}, {KEY_REGION, "us-west-2"},
        {KEY_ENABLE_FAILOVER, VALUE_BOOL_TRUE}, {KEY_BASE_DRIVER, MISSING_DRIVER}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ(PluginsError("ENABLE_CLUSTER_FAILOVER", "RDS_AUTH_TYPE=DSQL"), std::string(dbc_->err->error_msg));
}

// Under IAM a cluster ID is a host name, never expanded and never Aurora DSQL.
TEST_F(DsqlConnectionValidatorInitTest, ClusterIdWithIamAuth_ReachesTheDriverLoad) {
    dbc_->conn_attr = {{KEY_SERVER, CLUSTER_ID}, {KEY_AUTH_TYPE, VALUE_AUTH_IAM}, {KEY_REGION, "us-west-2"},
        {KEY_BASE_DRIVER, MISSING_DRIVER}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("IM003", dbc_->err->sqlstate);
}

// A rejected handle keeps no state from that attempt, so the next connect with the setting fixed goes on.
TEST_F(DsqlConnectionValidatorInitTest, RejectedHandleConnectsAfterTheSettingIsFixed) {
    dbc_->conn_attr = {{KEY_SERVER, DSQL_HOST}, {KEY_ENABLE_RW_SPLIT, VALUE_BOOL_TRUE},
        {KEY_BASE_DRIVER, MISSING_DRIVER}};
    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));
    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("08001", dbc_->err->sqlstate);

    dbc_->conn_attr.insert_or_assign(KEY_ENABLE_RW_SPLIT, VALUE_BOOL_FALSE);
    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));
    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("IM003", dbc_->err->sqlstate);
}
