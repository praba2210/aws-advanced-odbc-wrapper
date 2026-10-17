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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <optional>
#include <string>

#include "auth_mock_objects.h"
#include "common_mock_objects.h"

#include "../../driver/driver.h"
#include "../../driver/plugin/dsql/dsql_auth_plugin.h"
#include "../../driver/util/aws_sdk_helper.h"
#include "../../driver/util/connection_string_keys.h"

#ifndef AWS_PROFILE_TEST_RESOURCES_DIR
    #error "AWS_PROFILE_TEST_RESOURCES_DIR must be defined by the build"
#endif

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-west-2.on.aws";
    const std::string PORT = "5432";
    const std::string USERNAME = "admin";
    const std::pair<std::string, bool> FRESH_TOKEN("dsql_token", false);

    void SetEnvVar(const char* name, const std::string& value) {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    void UnsetEnvVar(const char* name) {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }

    std::optional<std::string> GetEnvVar(const char* name) {
        const char* value = std::getenv(name);
        return value ? std::optional<std::string>(value) : std::nullopt;
    }

    void RestoreEnvVar(const char* name, const std::optional<std::string>& value) {
        if (value) {
            SetEnvVar(name, *value);
        } else {
            UnsetEnvVar(name);
        }
    }
}  // namespace

class DsqlAuthPluginTest : public testing::Test {
protected:
    std::shared_ptr<MockBasePlugin> mock_base_plugin_;
    std::shared_ptr<MockAuthProvider> mock_auth_provider_;
    std::shared_ptr<MockDialect> mock_dialect_;
    std::shared_ptr<MockOdbcHelper> mock_odbc_helper_;
    DBC* dbc_;
    // CI exports AWS_REGION and AWS_DEFAULT_REGION. Clear them so each test sees only the regions it sets.
    std::optional<std::string> saved_aws_region_;
    std::optional<std::string> saved_aws_default_region_;

    static void SetUpTestSuite() {
        AwsSdkHelper::Init();
    }
    static void TearDownTestSuite() {
        AwsSdkHelper::Shutdown();
    }

    void SetUp() override {
        saved_aws_region_ = GetEnvVar("AWS_REGION");
        saved_aws_default_region_ = GetEnvVar("AWS_DEFAULT_REGION");
        UnsetEnvVar("AWS_REGION");
        UnsetEnvVar("AWS_DEFAULT_REGION");

        mock_auth_provider_ = std::make_shared<MockAuthProvider>();
        mock_base_plugin_ = std::make_shared<MockBasePlugin>();
        mock_dialect_ = std::make_shared<MockDialect>();
        mock_odbc_helper_ = std::make_shared<MockOdbcHelper>();
        dbc_ = new DBC();
        dbc_->conn_attr.insert_or_assign(KEY_SERVER, DSQL_HOST);
        dbc_->conn_attr.insert_or_assign(KEY_PORT, PORT);
        dbc_->conn_attr.insert_or_assign(KEY_DB_USERNAME, USERNAME);
    }
    void TearDown() override {
        delete dbc_;
        mock_auth_provider_.reset();
        RestoreEnvVar("AWS_REGION", saved_aws_region_);
        RestoreEnvVar("AWS_DEFAULT_REGION", saved_aws_default_region_);
    }

    void ExpectTokenForRegion(const std::string& region) {
        EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, region, testing::_, testing::_,
            testing::_, testing::_, testing::_))
            .Times(testing::Exactly(1))
            .WillOnce(testing::Return(FRESH_TOKEN));
        EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
            .WillOnce(testing::Return(SQL_SUCCESS));
    }

    SQLRETURN ConnectWithMockProvider() {
        DsqlAuthPlugin plugin(dbc_, mock_base_plugin_, mock_auth_provider_);
        return plugin.Connect(dbc_, nullptr, nullptr, 0, 0, SQL_DRIVER_NOPROMPT);
    }
};

TEST_F(DsqlAuthPluginTest, Connect_RegionFromDsqlHost) {
    EXPECT_CALL(*mock_auth_provider_, GetToken(DSQL_HOST, "us-west-2", PORT, USERNAME,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::Return(SQL_SUCCESS));

    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
    EXPECT_EQ(FRESH_TOKEN.first, dbc_->conn_attr.at(KEY_DB_PASSWORD));
}

TEST_F(DsqlAuthPluginTest, Connect_RegionKeyOverridesHost) {
    dbc_->conn_attr.insert_or_assign(KEY_REGION, "eu-west-1");
    ExpectTokenForRegion("eu-west-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

// Same order as 6 of the 9 connectors: the endpoint's region beats the environment.
TEST_F(DsqlAuthPluginTest, Connect_HostRegionBeatsEnv) {
    SetEnvVar("AWS_REGION", "eu-central-1");
    ExpectTokenForRegion("us-west-2");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

TEST_F(DsqlAuthPluginTest, Connect_RegionFromEnv_AwsRegionFirst) {
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "db.internal.example.com");
    SetEnvVar("AWS_REGION", "ca-central-1");
    SetEnvVar("AWS_DEFAULT_REGION", "ap-south-1");
    ExpectTokenForRegion("ca-central-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

TEST_F(DsqlAuthPluginTest, Connect_RegionFromEnv_AwsDefaultRegion) {
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "db.internal.example.com");
    SetEnvVar("AWS_DEFAULT_REGION", "ap-south-1");
    ExpectTokenForRegion("ap-south-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

TEST_F(DsqlAuthPluginTest, Connect_RegionFromProfileWhenNoHostOrEnvRegion) {
    SetEnvVar("AWS_CONFIG_FILE", std::string(AWS_PROFILE_TEST_RESOURCES_DIR) + "/aws_config");
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "db.internal.example.com");
    dbc_->conn_attr.insert_or_assign(KEY_AWS_PROFILE, "dev");  // region = us-west-2 in the fixture
    ExpectTokenForRegion("us-west-2");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

// No us-east-1 fallback (9 of 9 connectors): with no region anywhere, the connect fails before a token is minted.
TEST_F(DsqlAuthPluginTest, Connect_NoRegion_Fails) {
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "db.internal.example.com");
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_)).Times(testing::Exactly(0));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));

    EXPECT_EQ(SQL_ERROR, ConnectWithMockProvider());
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ(dbc_->err->native_err, ERR_CLIENT_UNABLE_TO_ESTABLISH_CONNECTION);
    EXPECT_NE(std::string::npos, std::string(dbc_->err->error_msg).find("Region"));
}

// Tokens are never cached, so an access error isn't retried with a second token (no connector retries either).
TEST_F(DsqlAuthPluginTest, Connect_AccessError_NoSecondToken) {
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::Return(SQL_ERROR));
    EXPECT_CALL(*mock_odbc_helper_, GetSqlStateAndLogMessage(testing::An<DBC*>(), testing::_))
        .WillRepeatedly(testing::Return("28P01"));
    EXPECT_CALL(*mock_dialect_, IsSqlStateAccessError(testing::_, testing::An<const std::string&>()))
        .WillRepeatedly(testing::Return(true));

    DsqlAuthPlugin plugin(dbc_, mock_base_plugin_, mock_auth_provider_, mock_dialect_, mock_odbc_helper_);
    EXPECT_EQ(SQL_ERROR, plugin.Connect(dbc_, nullptr, nullptr, 0, 0, SQL_DRIVER_NOPROMPT));
}
