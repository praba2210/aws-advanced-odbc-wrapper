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

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unicode/uloc.h>
#include <unicode/utypes.h>

#include "auth_mock_objects.h"
#include "common_mock_objects.h"
#include "scoped_ctype_locale.h"
#include "scoped_env_vars.h"

#include "../../driver/driver.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/dsql/dsql_auth_plugin.h"
#include "../../driver/util/aws_sdk_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/dsql_auth_provider.h"
#include "../../driver/util/logger_wrapper.h"

#ifndef AWS_PROFILE_TEST_RESOURCES_DIR
    #error "AWS_PROFILE_TEST_RESOURCES_DIR must be defined by the build"
#endif

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-west-2.on.aws";
    const std::string PORT = "5432";
    const std::string USERNAME = "admin";
    const std::pair<std::string, bool> FRESH_TOKEN("dsql_token", false);

    using test_env::SetEnvVar;
    using test_env::UnsetEnvVar;
}  // namespace

class DsqlAuthPluginTest : public testing::Test {
protected:
    std::shared_ptr<MockBasePlugin> mock_base_plugin_;
    std::shared_ptr<MockAuthProvider> mock_auth_provider_;
    std::shared_ptr<MockDialect> mock_dialect_;
    std::shared_ptr<MockOdbcHelper> mock_odbc_helper_;
    DBC* dbc_;
    // CI exports AWS_REGION and AWS_DEFAULT_REGION, and a developer's ~/.aws/config may set a default region. Each
    // test sees only the regions it sets, with the fixture config file in place of the user's.
    std::optional<ScopedEnvVars> saved_env_;

    static void SetUpTestSuite() {
        AwsSdkHelper::Init();
    }
    static void TearDownTestSuite() {
        AwsSdkHelper::Shutdown();
    }

    void SetUp() override {
        saved_env_.emplace(std::initializer_list<ScopedEnvVars::Var>{{"AWS_REGION", std::nullopt},
                                                                     {"AWS_DEFAULT_REGION", std::nullopt},
                                                                     {"AWS_PROFILE", std::nullopt},
                                                                     {"AWS_CONFIG_FILE", std::nullopt}});
        UseConfigFile("aws_config");

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
        saved_env_.reset();
    }

    static void UseConfigFile(const std::string& fixture) {
        SetEnvVar("AWS_CONFIG_FILE", std::string(AWS_PROFILE_TEST_RESOURCES_DIR) + "/" + fixture);
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

    // Connects through the real DsqlAuthProvider the plugin builds; the base plugin is expected once and succeeds.
    SQLRETURN ConnectWithRealProvider() {
        EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
            .WillOnce(testing::Return(SQL_SUCCESS));
        DsqlAuthPlugin plugin(dbc_, mock_base_plugin_);
        const SQLRETURN ret = plugin.Connect(dbc_, nullptr, nullptr, 0, 0, SQL_DRIVER_NOPROMPT);
        testing::Mock::VerifyAndClearExpectations(mock_base_plugin_.get());
        return ret;
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

// The token is signed for IAM_HOST, so its region wins over the one in SERVER.
TEST_F(DsqlAuthPluginTest, Connect_RegionFromIamHostBeforeServer) {
    dbc_->conn_attr.insert_or_assign(KEY_IAM_HOST, "abcdefghijklmnopqrstuvwxyz.dsql.eu-west-1.on.aws");
    ExpectTokenForRegion("eu-west-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

// PrivateLink without private DNS: SERVER is the endpoint's own name, the token is signed for the cluster endpoint
// in IAM_HOST, and verify-full can't pass because the certificate doesn't name the endpoint.
TEST_F(DsqlAuthPluginTest, Connect_PrivateLinkWithoutPrivateDns) {
    const std::string cluster_host = "abcdefghijklmnopqrstuvwxyz.dsql.ap-southeast-2.on.aws";
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "vpce-0123456789abcdef0-abcdefgh.dsql-fnh4.ap-southeast-2.vpce.amazonaws.com");
    dbc_->conn_attr.insert_or_assign(KEY_IAM_HOST, cluster_host);
    dbc_->conn_attr.insert_or_assign("SSLMODE", "verify-ca");
    dbc_->conn_attr.insert_or_assign("PQOPT", "{sslrootcert=/etc/ssl/AmazonRootCA1.pem}");
    EXPECT_CALL(*mock_auth_provider_, GetToken(cluster_host, "ap-southeast-2", PORT, USERNAME,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(SQL_SUCCESS));

    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
    EXPECT_EQ("verify-ca", dbc_->conn_attr.at("SSLMODE"));
}

TEST_F(DsqlAuthPluginTest, Connect_RegionFromProfileWhenNoHostOrEnvRegion) {
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "db.internal.example.com");
    dbc_->conn_attr.insert_or_assign(KEY_AWS_PROFILE, "dev");  // region = us-west-2 in the fixture
    ExpectTokenForRegion("us-west-2");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

// With no AWS_PROFILE key the credentials come from the AWS_PROFILE variable's profile, so the region does too.
TEST_F(DsqlAuthPluginTest, Connect_RegionFromEnvProfile) {
    UseConfigFile("aws_config_with_default");
    SetEnvVar("AWS_PROFILE", "env_profile");  // region = ap-northeast-2 in the fixture
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "db.internal.example.com");
    ExpectTokenForRegion("ap-northeast-2");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

TEST_F(DsqlAuthPluginTest, Connect_RegionFromDefaultProfile) {
    UseConfigFile("aws_config_with_default");  // [default] region = eu-north-1
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "db.internal.example.com");
    ExpectTokenForRegion("eu-north-1");
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

// Missing or expired credentials fail the connect before signing, so no token the server would reject is sent.
TEST_F(DsqlAuthPluginTest, Connect_UnresolvedCredentials_FailsBeforeSigning) {
    mock_auth_provider_->mock_has_resolved_credentials = false;
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));

    EXPECT_EQ(SQL_ERROR, ConnectWithMockProvider());
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("Unable to resolve AWS credentials for DSQL authentication from the default credential chain (such as "
              "environment variables, ~/.aws/credentials, an AWS IAM Identity Center (SSO) session, or a container "
              "or instance role)", dbc_->err->error_msg);
}

// The plugin checks the credentials and then asks for the token; the real provider reads its source once for both.
TEST_F(DsqlAuthPluginTest, Connect_ReadsCredentialsOnce) {
    const auto source = std::make_shared<CountingCredentialsProvider>(
        std::vector{Aws::Auth::AWSCredentials("AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY")});
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(SQL_SUCCESS));
    DsqlAuthPlugin plugin(dbc_, mock_base_plugin_, std::make_shared<DsqlAuthProvider>("us-west-2", source));
    EXPECT_EQ(SQL_SUCCESS, plugin.Connect(dbc_, nullptr, nullptr, 0, 0, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(1, source->Reads());
}

namespace {
    const std::string EXPECTED_APP_NAME = std::string("aurora-dsql-odbc/") + DRIVER_VERSION;
}  // namespace

TEST_F(DsqlAuthPluginTest, Defaults_EmptyAttributes) {
    std::map<std::string, std::string> conn_attr;
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("verify-full", conn_attr.at("SSLMODE"));
    EXPECT_EQ("admin", conn_attr.at(KEY_DB_USERNAME));
    EXPECT_EQ("postgres", conn_attr.at(KEY_DATABASE));
    EXPECT_EQ("{sslrootcert=system application_name=" + EXPECTED_APP_NAME + "}", conn_attr.at("PQOPT"));
}

// DSQL rejects plaintext, and Go, .NET, PHP, Ruby and Rust always verify the server. A mode that may not encrypt,
// or one psqlODBC doesn't know, is overridden.
TEST_F(DsqlAuthPluginTest, Defaults_ForcesVerifyFull) {
    for (const std::string mode : {"disable", "allow", "prefer", "verify-fulll"}) {
        std::map<std::string, std::string> conn_attr = {{"SSLMODE", mode}};
        DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
        EXPECT_EQ("verify-full", conn_attr.at("SSLMODE")) << mode;
    }
}

TEST_F(DsqlAuthPluginTest, Defaults_ForcesVerifyFull_ShortKey) {
    std::map<std::string, std::string> conn_attr = {{"CA", "disable"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_FALSE(conn_attr.contains("CA"));
    EXPECT_EQ("verify-full", conn_attr.at("SSLMODE"));
}

// verify-ca and require still encrypt, and a PrivateLink endpoint's own DNS name needs one of them. libpq rejects
// sslrootcert=system with either, so it isn't added. psqlODBC reads the mode's first letter case-sensitively
// ("REQUIRE" would become disable), so the mode is passed in lowercase.
TEST_F(DsqlAuthPluginTest, Defaults_KeepsVerifyCaAndRequire) {
    for (const auto& [requested, expected] : std::map<std::string, std::string>{
             {"verify-ca", "verify-ca"}, {"Verify-CA", "verify-ca"}, {"require", "require"}, {"REQUIRE", "require"}}) {
        std::map<std::string, std::string> conn_attr = {{"SSLMODE", requested}};
        DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
        EXPECT_EQ(expected, conn_attr.at("SSLMODE")) << requested;
        EXPECT_EQ("{application_name=" + EXPECTED_APP_NAME + "}", conn_attr.at("PQOPT")) << requested;
    }
}

TEST_F(DsqlAuthPluginTest, Defaults_KeepsVerifyCa_ShortKey) {
    std::map<std::string, std::string> conn_attr = {{"CA", "verify-ca"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_FALSE(conn_attr.contains("CA"));
    EXPECT_EQ("verify-ca", conn_attr.at("SSLMODE"));
}

TEST_F(DsqlAuthPluginTest, Defaults_KeepUserAndDatabase) {
    std::map<std::string, std::string> conn_attr = {{KEY_DB_USERNAME, "app_user"}, {KEY_DATABASE, "mydb"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("app_user", conn_attr.at(KEY_DB_USERNAME));
    EXPECT_EQ("mydb", conn_attr.at(KEY_DATABASE));
}

// A connection string keeps USERNAME/USER as typed (only DSN entries are mapped to UID), but the token is signed for
// UID. The alias moves to UID, so psqlODBC also gets a single user name. psqlODBC reads DX itself, so it stays.
TEST_F(DsqlAuthPluginTest, Defaults_MapsUsernameAliasToUid) {
    for (const char* alias : {ALIAS_KEY_USERNAME_1, ALIAS_KEY_USERNAME_2}) {
        std::map<std::string, std::string> conn_attr = {{alias, "app_user"}, {"DX", "mydb"}};
        DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
        EXPECT_EQ("app_user", conn_attr.at(KEY_DB_USERNAME)) << alias;
        EXPECT_FALSE(conn_attr.contains(alias)) << alias;
        EXPECT_FALSE(conn_attr.contains(KEY_DATABASE)) << alias;
    }
}

TEST_F(DsqlAuthPluginTest, Defaults_UidBeatsUsernameAlias) {
    std::map<std::string, std::string> conn_attr = {{KEY_DB_USERNAME, "app_user"}, {ALIAS_KEY_USERNAME_2, "other"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("app_user", conn_attr.at(KEY_DB_USERNAME));
    EXPECT_FALSE(conn_attr.contains(ALIAS_KEY_USERNAME_2));
}

TEST_F(DsqlAuthPluginTest, Connect_UsernameAlias_SignsForThatUser) {
    dbc_->conn_attr.erase(KEY_DB_USERNAME);
    dbc_->conn_attr.insert_or_assign(ALIAS_KEY_USERNAME_2, "app_user");
    EXPECT_CALL(*mock_auth_provider_, GetToken(DSQL_HOST, "us-west-2", PORT, "app_user",
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(SQL_SUCCESS));

    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
    EXPECT_EQ("app_user", dbc_->conn_attr.at(KEY_DB_USERNAME));
    EXPECT_FALSE(dbc_->conn_attr.contains(ALIAS_KEY_USERNAME_2));
}

TEST_F(DsqlAuthPluginTest, Defaults_PqoptKeepsUserRootCert) {
    std::map<std::string, std::string> conn_attr = {{"PQOPT", "{sslrootcert=/etc/ssl/corp-ca.pem}"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("{sslrootcert=/etc/ssl/corp-ca.pem application_name=" + EXPECTED_APP_NAME + "}", conn_attr.at("PQOPT"));
}

TEST_F(DsqlAuthPluginTest, Defaults_PqoptKeepsUserApplicationName_ShortKey) {
    std::map<std::string, std::string> conn_attr = {{"D5", "{application_name=billing}"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_FALSE(conn_attr.contains("PQOPT"));
    EXPECT_EQ("{application_name=billing sslrootcert=system}", conn_attr.at("D5"));
}

TEST_F(DsqlAuthPluginTest, Connect_AppliesDefaultsBeforeBaseDriver) {
    dbc_->conn_attr.erase(KEY_DB_USERNAME);
    dbc_->conn_attr.insert_or_assign("SSLMODE", "disable");
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, "admin",
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(SQL_SUCCESS));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, ConnectWithMockProvider());
    EXPECT_EQ("verify-full", dbc_->conn_attr.at("SSLMODE"));
    EXPECT_EQ("admin", dbc_->conn_attr.at(KEY_DB_USERNAME));
}

namespace {
    // What libpq 14 and 15 report for sslrootcert=system: they look for a file of that name.
    const std::string OLD_LIBPQ_ROOT_CERT_ERROR =
        "connection to server at \"x.dsql.us-east-1.on.aws\", port 5432 failed: root certificate file \"system\" does "
        "not exist\nEither provide the file or change sslmode to disable server certificate verification.";
}  // namespace

TEST_F(DsqlAuthPluginTest, RootCertHint_SystemRootCertError) {
    std::map<std::string, std::string> conn_attr;
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    for (const std::string& error : {OLD_LIBPQ_ROOT_CERT_ERROR, std::string("SSL error: certificate verify failed")}) {
        const std::string_view hint = DsqlAuthPlugin::GetRootCertHint(conn_attr, error);
        EXPECT_NE(std::string::npos, hint.find("libpq 16")) << error;
        EXPECT_NE(std::string::npos, hint.find("pqopt={sslrootcert=")) << error;
    }
}

TEST_F(DsqlAuthPluginTest, RootCertHint_NoneForOwnRootCertOrOtherErrors) {
    std::map<std::string, std::string> own_cert = {{"PQOPT", "{sslrootcert=/etc/ssl/corp-ca.pem}"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(own_cert);
    EXPECT_EQ("", DsqlAuthPlugin::GetRootCertHint(own_cert, "SSL error: certificate verify failed"));

    std::map<std::string, std::string> defaults;
    DsqlAuthPlugin::ApplyConnectionDefaults(defaults);
    EXPECT_EQ("", DsqlAuthPlugin::GetRootCertHint(defaults, "FATAL:  unable to accept connection, access denied"));
    // The root certificates worked; the host name (an IP here) isn't in the certificate (smoke run, 2026-10-22).
    EXPECT_EQ("", DsqlAuthPlugin::GetRootCertHint(defaults,
        "connection to server at \"18.97.33.132\", port 5432 failed: server certificate for \"*.dsql.us-east-1.on.aws\" "
        "(and 2 other names) does not match host name \"18.97.33.132\""));
}

namespace {
    // Collects every log line, so a test can check what the driver logged.
    class CapturingLogSink : public nglog::LogSink {
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
}  // namespace

// The hint comes from the error the base plugin already read: the error is read once, and the hint logged once.
TEST_F(DsqlAuthPluginTest, Connect_RootCertError_LogsHintOnce) {
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(SQL_ERROR));
    EXPECT_CALL(*mock_odbc_helper_, GetSqlStateAndLogMessage(testing::An<DBC*>(), testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::DoAll(testing::SetArgReferee<1>(OLD_LIBPQ_ROOT_CERT_ERROR), testing::Return("08001")));
    EXPECT_CALL(*mock_dialect_, IsSqlStateAccessError(testing::_, testing::An<const std::string&>()))
        .WillRepeatedly(testing::Return(false));

    CapturingLogSink sink;
    nglog::AddLogSink(&sink);
    DsqlAuthPlugin plugin(dbc_, mock_base_plugin_, mock_auth_provider_, mock_dialect_, mock_odbc_helper_);
    const SQLRETURN ret = plugin.Connect(dbc_, nullptr, nullptr, 0, 0, SQL_DRIVER_NOPROMPT);
    nglog::RemoveLogSink(&sink);

    EXPECT_EQ(SQL_ERROR, ret);
    EXPECT_EQ(1, sink.Count("needs libpq 16 or later"));
}

// require doesn't verify the certificate at all (with no ~/.postgresql/root.crt), so the token could reach an
// impostor; verify-ca only skips the host name. The warning says which.
TEST_F(DsqlAuthPluginTest, Defaults_WarnsWhatTheKeptModeSkips) {
    CapturingLogSink sink;
    nglog::AddLogSink(&sink);
    for (const std::string mode : {"require", "verify-ca"}) {
        std::map<std::string, std::string> conn_attr = {{"SSLMODE", mode}};
        DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    }
    nglog::RemoveLogSink(&sink);

    EXPECT_EQ(1, sink.Count("SSLmode=require encrypts but doesn't verify the server certificate"));
    EXPECT_EQ(1, sink.Count("SSLmode=verify-ca verifies the server certificate but not its host name"));
}

// SigV4 signs the region as given and region names are lowercase, so EU-WEST-1 couldn't authenticate.
TEST_F(DsqlAuthPluginTest, Connect_RegionKeyIsLowercased) {
    dbc_->conn_attr.insert_or_assign(KEY_REGION, "EU-WEST-1");
    ExpectTokenForRegion("eu-west-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

TEST_F(DsqlAuthPluginTest, Connect_EnvRegionIsLowercased) {
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "db.internal.example.com");
    SetEnvVar("AWS_REGION", "CA-CENTRAL-1");
    ExpectTokenForRegion("ca-central-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

// The signing region is trimmed like the region a cluster ID expands with, so the two always match.
TEST_F(DsqlAuthPluginTest, Connect_RegionKeyIsTrimmed) {
    dbc_->conn_attr.insert_or_assign(KEY_REGION, " EU-WEST-1\t");
    ExpectTokenForRegion("eu-west-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

// A blank REGION counts as unset, so the token is signed for the region in SERVER's endpoint.
TEST_F(DsqlAuthPluginTest, Connect_BlankRegionKey_UsesHostRegion) {
    dbc_->conn_attr.insert_or_assign(KEY_REGION, "  ");
    ExpectTokenForRegion("us-west-2");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

namespace {
    const std::string CLUSTER_ID = "abcdefghijklmnopqrstuvwxyz";
    const std::string NO_REGION_ERROR =
        "DSQL Authentication failed. Region is required when host is a cluster ID (set 'REGION')";

    std::map<std::string, std::string> DsqlAttributes(const std::string& server) {
        return {{KEY_AUTH_TYPE, VALUE_AUTH_DSQL}, {KEY_SERVER, server}};
    }
}  // namespace

// A bare cluster ID connects to <id>.dsql.<region>.on.aws, as in the Aurora DSQL connectors.
TEST_F(DsqlAuthPluginTest, ClusterId_ExpandsWithRegionKey) {
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_REGION, "eu-west-1");
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", conn_attr.at(KEY_SERVER));
    EXPECT_EQ("", error);
}

TEST_F(DsqlAuthPluginTest, ClusterId_AuthTypeIsCaseInsensitive) {
    std::map<std::string, std::string> conn_attr = {{KEY_AUTH_TYPE, "dsql"}, {KEY_SERVER, CLUSTER_ID},
                                                    {KEY_REGION, "eu-west-1"}};
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", conn_attr.at(KEY_SERVER));
}

TEST_F(DsqlAuthPluginTest, ClusterId_RegionFromEnv) {
    SetEnvVar("AWS_REGION", "ca-central-1");
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.ca-central-1.on.aws", conn_attr.at(KEY_SERVER));
}

// Containers and CI hosts often set only AWS_DEFAULT_REGION.
TEST_F(DsqlAuthPluginTest, ClusterId_RegionFromAwsDefaultRegion) {
    SetEnvVar("AWS_DEFAULT_REGION", "ap-northeast-1");
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.ap-northeast-1.on.aws", conn_attr.at(KEY_SERVER));
}

TEST_F(DsqlAuthPluginTest, ClusterId_RegionFromProfile) {
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_AWS_PROFILE, "dev");  // region = us-west-2 in the fixture
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.us-west-2.on.aws", conn_attr.at(KEY_SERVER));
}

// The region in IAM_HOST's endpoint, the host the token is signed for, comes before the environment's.
TEST_F(DsqlAuthPluginTest, ClusterId_RegionFromIamHostEndpoint) {
    SetEnvVar("AWS_REGION", "ca-central-1");
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_IAM_HOST, CLUSTER_ID + ".dsql.eu-west-1.on.aws");
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", conn_attr.at(KEY_SERVER));
}

// The majority connectors' rule: no region, no endpoint. Nothing is changed.
TEST_F(DsqlAuthPluginTest, ClusterId_NoRegion_Fails) {
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    std::string error;
    EXPECT_FALSE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(NO_REGION_ERROR, error);
    EXPECT_EQ(CLUSTER_ID, conn_attr.at(KEY_SERVER));
}

namespace {
    std::string InvalidRegionError(const std::string& region) {
        return "DSQL Authentication failed. Invalid region '" + region + "' for a cluster ID host: it must be letters, "
               "digits and hyphens, start and end with a letter or digit, and be at most 63 characters long, such as "
               "us-east-1 (set 'REGION')";
    }

    // Not a DNS host label: an inner space or tab, a dot, an underscore, a comma (libpq's host list separator), a ';'
    // (it would end SERVER in psqlODBC's connection string), a leading or trailing hyphen, a non-ASCII letter, or more
    // than 63 characters.
    const std::vector<std::string> INVALID_REGIONS = {"us east-1", "us\teast-1", "us.east-1", "us_east_1",
                                                      "us-east-1,eu-west-1", "us-east-1;port=1", "-us-east-1",
                                                      "us-east-1-", "\xC3\xBCs-east-1", std::string(64, 'a')};

    // Spaces, tabs and line breaks around the region are dropped, such as the space Windows cmd keeps in
    // "set AWS_REGION=us-east-1 && app.exe", or the \r of a file with Windows line ends.
    const std::vector<std::string> PADDED_REGIONS = {"us-east-1 ", " us-east-1", "\tus-east-1", "us-east-1\t",
                                                     "us-east-1\r", "us-east-1\r\n", " US-EAST-1 "};

    const std::string BLANK_REGION = " \t\r\n";
}  // namespace

// The region becomes a DNS label of the endpoint. One that breaks the DNS host label rule fails here, with the value,
// instead of later as a host name or connection option error from libpq.
TEST_F(DsqlAuthPluginTest, ClusterId_InvalidRegion_Fails) {
    for (const std::string& region : INVALID_REGIONS) {
        std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
        conn_attr.insert_or_assign(KEY_REGION, region);
        std::string error;
        EXPECT_FALSE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << region;
        EXPECT_EQ(InvalidRegionError(region), error) << region;
        EXPECT_EQ(CLUSTER_ID, conn_attr.at(KEY_SERVER)) << region;
    }
}

TEST_F(DsqlAuthPluginTest, ClusterId_InvalidEnvRegion_Fails) {
    for (const std::string& region : INVALID_REGIONS) {
        SetEnvVar("AWS_REGION", region);
        std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
        std::string error;
        EXPECT_FALSE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << region;
        EXPECT_EQ(InvalidRegionError(region), error) << region;
        EXPECT_EQ(CLUSTER_ID, conn_attr.at(KEY_SERVER)) << region;
    }
}

TEST_F(DsqlAuthPluginTest, ClusterId_RegionWithSpacesAround_IsTrimmed) {
    for (const std::string& region : PADDED_REGIONS) {
        std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
        conn_attr.insert_or_assign(KEY_REGION, region);
        std::string error;
        EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
        EXPECT_EQ(CLUSTER_ID + ".dsql.us-east-1.on.aws", conn_attr.at(KEY_SERVER)) << region;
    }
}

TEST_F(DsqlAuthPluginTest, ClusterId_EnvRegionWithSpacesAround_IsTrimmed) {
    for (const std::string& region : PADDED_REGIONS) {
        SetEnvVar("AWS_REGION", region);
        std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
        std::string error;
        EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
        EXPECT_EQ(CLUSTER_ID + ".dsql.us-east-1.on.aws", conn_attr.at(KEY_SERVER)) << region;
    }
}

// A region of only spaces, tabs or line breaks counts as unset, as an empty one does: the next source gives the region.
TEST_F(DsqlAuthPluginTest, ClusterId_BlankRegionKey_UsesIamHostEndpoint) {
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_REGION, BLANK_REGION);
    conn_attr.insert_or_assign(KEY_IAM_HOST, CLUSTER_ID + ".dsql.eu-west-1.on.aws");
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", conn_attr.at(KEY_SERVER));
}

// SERVER is the endpoint and IAM_HOST the cluster ID, as behind PrivateLink.
TEST_F(DsqlAuthPluginTest, ClusterId_BlankRegionKey_UsesServerEndpoint) {
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID + ".dsql.ap-south-1.on.aws");
    conn_attr.insert_or_assign(KEY_REGION, BLANK_REGION);
    conn_attr.insert_or_assign(KEY_IAM_HOST, CLUSTER_ID);
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.ap-south-1.on.aws", conn_attr.at(KEY_IAM_HOST));
}

TEST_F(DsqlAuthPluginTest, ClusterId_BlankRegionKey_UsesEnvRegion) {
    SetEnvVar("AWS_REGION", "ca-central-1");
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_REGION, BLANK_REGION);
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.ca-central-1.on.aws", conn_attr.at(KEY_SERVER));
}

TEST_F(DsqlAuthPluginTest, ClusterId_BlankAwsRegion_UsesAwsDefaultRegion) {
    SetEnvVar("AWS_REGION", BLANK_REGION);
    SetEnvVar("AWS_DEFAULT_REGION", "ap-northeast-1");
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_REGION, BLANK_REGION);
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.ap-northeast-1.on.aws", conn_attr.at(KEY_SERVER));
}

TEST_F(DsqlAuthPluginTest, ClusterId_BlankRegionAndEnv_UsesProfileRegion) {
    SetEnvVar("AWS_REGION", BLANK_REGION);
    SetEnvVar("AWS_DEFAULT_REGION", BLANK_REGION);
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_REGION, BLANK_REGION);
    conn_attr.insert_or_assign(KEY_AWS_PROFILE, "dev");  // region = us-west-2 in the fixture
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.us-west-2.on.aws", conn_attr.at(KEY_SERVER));
}

// With every source blank or unset, it's the missing-region error, not the invalid-region one.
TEST_F(DsqlAuthPluginTest, ClusterId_BlankRegionEverywhere_Fails) {
    SetEnvVar("AWS_REGION", BLANK_REGION);
    SetEnvVar("AWS_DEFAULT_REGION", BLANK_REGION);
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_REGION, BLANK_REGION);
    std::string error;
    EXPECT_FALSE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(NO_REGION_ERROR, error);
    EXPECT_EQ(CLUSTER_ID, conn_attr.at(KEY_SERVER));
}

// .on.aws is the endpoint suffix of the commercial partition, where Aurora DSQL's documented endpoints are.
TEST_F(DsqlAuthPluginTest, ClusterId_CommercialRegion_Expands) {
    for (const std::string region : {"us-east-1", "ap-southeast-7", "il-central-1"}) {
        std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
        conn_attr.insert_or_assign(KEY_REGION, region);
        std::string error;
        EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << region;
        EXPECT_EQ(CLUSTER_ID + ".dsql." + region + ".on.aws", conn_attr.at(KEY_SERVER)) << region;
    }
}

// The region names of the other partitions, GovCloud, China, the ISO partitions and the European Sovereign Cloud, pass
// the region check too. Only that is checked: their endpoint suffix may not be .on.aws.
TEST_F(DsqlAuthPluginTest, ClusterId_OtherPartitionRegion_PassesTheCheck) {
    for (const std::string region : {"us-gov-west-1", "cn-northwest-1", "us-iso-east-1", "us-isob-east-1",
                                     "eu-isoe-west-1", "us-isof-south-1", "eusc-de-east-1"}) {
        std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
        conn_attr.insert_or_assign(KEY_REGION, region);
        std::string error;
        EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << region;
        EXPECT_EQ("", error) << region;
    }
}

// DNS is case-insensitive and region names are lowercase, so an uppercase ID or region still names the cluster.
TEST_F(DsqlAuthPluginTest, ClusterId_UppercaseIdAndRegion_AreLowercased) {
    std::map<std::string, std::string> conn_attr = DsqlAttributes("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    conn_attr.insert_or_assign(KEY_REGION, "EU-WEST-1");
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", conn_attr.at(KEY_SERVER));
}

// Hosts such as Python set LC_CTYPE from the environment, and under tr_TR.UTF-8 glibc's std::tolower leaves I as I:
// the ID and the region, from REGION or from an endpoint, are folded in ASCII. Needs the tr_TR.UTF-8 locale.
TEST_F(DsqlAuthPluginTest, ClusterId_TurkishCtype_IdAndRegionAreLowercased) {
    const ScopedCtypeLocale ctype("tr_TR.UTF-8");
    if (!ctype.IsSet()) {
        SKIP_OR_FAIL_WITHOUT_TURKISH_LOCALE();
    }
    std::map<std::string, std::string> conn_attr = DsqlAttributes("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    conn_attr.insert_or_assign(KEY_REGION, "IL-CENTRAL-1");
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.il-central-1.on.aws", conn_attr.at(KEY_SERVER));

    conn_attr = DsqlAttributes("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    conn_attr.insert_or_assign(KEY_IAM_HOST, "ABCDEFGHIJKLMNOPQRSTUVWXYZ.DSQL.US-ISO-EAST-1.ON.AWS");
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.us-iso-east-1.on.aws", conn_attr.at(KEY_SERVER));
}

// Only exactly 26 letters or digits make a cluster ID. Anything else is a host name, left as typed, with no region.
TEST_F(DsqlAuthPluginTest, ClusterId_OtherHosts_Unchanged) {
    const std::vector<std::string> hosts = {CLUSTER_ID.substr(1), CLUSTER_ID + "0", "abcdefghijklmnopqrstuvwxy-",
                                            "localhost", "10.0.0.1", "::1", CLUSTER_ID + ".dsql.us-east-1.on.aws", ""};
    for (const std::string& host : hosts) {
        std::map<std::string, std::string> conn_attr = DsqlAttributes(host);
        std::string error;
        EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << host;
        EXPECT_EQ(host, conn_attr.at(KEY_SERVER)) << host;
        EXPECT_EQ("", error) << host;
    }
}

// A cluster ID means a DSQL endpoint only under RDS_AUTH_TYPE=DSQL; under any other auth type it's a host name.
TEST_F(DsqlAuthPluginTest, ClusterId_OtherAuthType_Unchanged) {
    for (const std::string auth_type : {"IAM", "DATABASE", ""}) {
        std::map<std::string, std::string> conn_attr = {{KEY_SERVER, CLUSTER_ID}, {KEY_REGION, "eu-west-1"}};
        if (!auth_type.empty()) {
            conn_attr.insert_or_assign(KEY_AUTH_TYPE, auth_type);
        }
        std::string error;
        EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << auth_type;
        EXPECT_EQ(CLUSTER_ID, conn_attr.at(KEY_SERVER)) << auth_type;
    }
}

// PrivateLink without private DNS: SERVER is the endpoint's own name and the token is signed for IAM_HOST, which can be
// a cluster ID too.
TEST_F(DsqlAuthPluginTest, ClusterId_InIamHost_Expanded) {
    const std::string vpce_host = "vpce-0123456789abcdef0-abcdefgh.dsql-fnh4.ap-southeast-2.vpce.amazonaws.com";
    std::map<std::string, std::string> conn_attr = DsqlAttributes(vpce_host);
    conn_attr.insert_or_assign(KEY_IAM_HOST, CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_REGION, "ap-southeast-2");
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.ap-southeast-2.on.aws", conn_attr.at(KEY_IAM_HOST));
    EXPECT_EQ(vpce_host, conn_attr.at(KEY_SERVER));
}

// The Windows dialog shows IAM Host for Aurora DSQL, so the cluster ID can end up in both fields.
TEST_F(DsqlAuthPluginTest, ClusterId_InServerAndIamHost_BothExpanded) {
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_IAM_HOST, CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_REGION, "eu-west-1");
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", conn_attr.at(KEY_SERVER));
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", conn_attr.at(KEY_IAM_HOST));
}

// A reused connection handle keeps the expanded SERVER when the new connection string has none, so a second pass must
// leave it alone.
TEST_F(DsqlAuthPluginTest, ClusterId_AlreadyExpanded_Unchanged) {
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_REGION, "eu-west-1");
    std::string error;
    ASSERT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    conn_attr.erase(KEY_REGION);
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", conn_attr.at(KEY_SERVER));
}

// The plugin service takes its host, cluster ID and dialect from SERVER, so the expansion runs first in
// RDS_InitializeConnection: SERVER is expanded even though loading the base driver fails afterwards.
TEST_F(DsqlAuthPluginTest, InitializeConnection_ExpandsClusterIdFirst) {
    ENV env;
    dbc_->env = &env;
    dbc_->conn_attr = DsqlAttributes(CLUSTER_ID);
    dbc_->conn_attr.insert_or_assign(KEY_REGION, "eu-west-1");
    dbc_->conn_attr.insert_or_assign(KEY_BASE_DRIVER, "/nonexistent/path/no-such-driver.so");

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-west-1.on.aws", dbc_->conn_attr.at(KEY_SERVER));
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("IM003", dbc_->err->sqlstate);
    EXPECT_EQ(nullptr, dbc_->plugin_service);
    dbc_->env = nullptr;
}

// Without a region the connect fails with 08001 before the base driver is loaded (that would be IM003 here).
TEST_F(DsqlAuthPluginTest, InitializeConnection_ClusterIdWithoutRegion_FailsBeforeLoadingTheDriver) {
    ENV env;
    dbc_->env = &env;
    dbc_->conn_attr = DsqlAttributes(CLUSTER_ID);
    dbc_->conn_attr.insert_or_assign(KEY_BASE_DRIVER, "/nonexistent/path/no-such-driver.so");

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ(NO_REGION_ERROR, dbc_->err->error_msg);
    EXPECT_FALSE(env.driver_lib_loader);
    dbc_->env = nullptr;
}

TEST_F(DsqlAuthPluginTest, InitializeConnection_ClusterIdWithInvalidRegion_FailsBeforeLoadingTheDriver) {
    ENV env;
    dbc_->env = &env;
    dbc_->conn_attr = DsqlAttributes(CLUSTER_ID);
    dbc_->conn_attr.insert_or_assign(KEY_REGION, "us east-1");
    dbc_->conn_attr.insert_or_assign(KEY_BASE_DRIVER, "/nonexistent/path/no-such-driver.so");

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ(InvalidRegionError("us east-1"), dbc_->err->error_msg);
    EXPECT_EQ(CLUSTER_ID, dbc_->conn_attr.at(KEY_SERVER));
    EXPECT_FALSE(env.driver_lib_loader);
    dbc_->env = nullptr;
}

// The expansion also runs before the attribute check: a region-less cluster ID with an invalid TOKEN_EXPIRATION reports
// the 08001, not the validator's 01S00.
TEST_F(DsqlAuthPluginTest, InitializeConnection_ClusterIdWithoutRegion_FailsBeforeTheAttributeCheck) {
    ENV env;
    dbc_->env = &env;
    dbc_->conn_attr = DsqlAttributes(CLUSTER_ID);
    dbc_->conn_attr.insert_or_assign(KEY_TOKEN_EXPIRATION, "-1");
    dbc_->conn_attr.insert_or_assign(KEY_BASE_DRIVER, "/nonexistent/path/no-such-driver.so");

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("08001", dbc_->err->sqlstate);
    EXPECT_EQ(NO_REGION_ERROR, dbc_->err->error_msg);
    dbc_->env = nullptr;
}

// libpq keywords are whole words: fallback_application_name isn't application_name, and a path that mentions
// sslrootcert isn't the sslrootcert option.
TEST_F(DsqlAuthPluginTest, Defaults_PqoptMatchesWholeKeywords) {
    std::map<std::string, std::string> conn_attr = {
        {"PQOPT", "{fallback_application_name=etl sslcert=/certs/sslrootcert-client.pem}"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("{fallback_application_name=etl sslcert=/certs/sslrootcert-client.pem sslrootcert=system "
              "application_name=" + EXPECTED_APP_NAME + "}", conn_attr.at("PQOPT"));
}

// A single-quoted value can hold spaces and '=' (PrivateLink: options='-c amzn-cluster-id=<id>'); nothing inside it
// is a keyword. Spaces around '=' are allowed.
TEST_F(DsqlAuthPluginTest, Defaults_PqoptSkipsQuotedValues) {
    std::map<std::string, std::string> conn_attr = {
        {"PQOPT", "{options='-c sslrootcert=x' application_name = 'billing app'}"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("{options='-c sslrootcert=x' application_name = 'billing app' sslrootcert=system}", conn_attr.at("PQOPT"));
}

// A connection string value keeps the spaces typed around it; they must not stop the braces from being unwrapped.
TEST_F(DsqlAuthPluginTest, Defaults_PqoptTrimsSpaces) {
    std::map<std::string, std::string> conn_attr = {{"PQOPT", "  { sslrootcert=/etc/ssl/corp-ca.pem }  "}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("{sslrootcert=/etc/ssl/corp-ca.pem application_name=" + EXPECTED_APP_NAME + "}", conn_attr.at("PQOPT"));
}

// A Windows path typed with backslashes stays as typed (libpq reads a backslash as an escape; the page says to use
// forward slashes) and is still the user's sslrootcert: a sslrootcert=system after it would win, because libpq keeps
// the last of a repeated keyword.
TEST_F(DsqlAuthPluginTest, Defaults_PqoptKeepsBackslashPath) {
    std::map<std::string, std::string> conn_attr = {{"PQOPT", "{sslrootcert=C:\\certs\\AmazonRootCA1.pem}"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("{sslrootcert=C:\\certs\\AmazonRootCA1.pem application_name=" + EXPECTED_APP_NAME + "}",
              conn_attr.at("PQOPT"));
}

// The hint needs the sslrootcert value to be exactly "system", however the option is spaced.
TEST_F(DsqlAuthPluginTest, RootCertHint_MatchesTheSystemValueOnly) {
    const std::string error = "SSL error: certificate verify failed";
    EXPECT_EQ("", DsqlAuthPlugin::GetRootCertHint({{"PQOPT", "{sslrootcert=system_roots.pem}"}}, error));
    EXPECT_NE("", DsqlAuthPlugin::GetRootCertHint({{"PQOPT", "{sslrootcert = system}"}}, error));
}

// On Windows, libpq can report OpenSSL's "unregistered scheme" for sslrootcert=system (DSQL user guide, Configuring
// SSL/TLS certificates, Windows).
TEST_F(DsqlAuthPluginTest, RootCertHint_WindowsUnregisteredScheme) {
    std::map<std::string, std::string> conn_attr;
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    const std::string_view hint = DsqlAuthPlugin::GetRootCertHint(conn_attr,
        "connection to server at \"x.dsql.us-east-1.on.aws\" (10.0.0.1), port 5432 failed: SSL error: unregistered scheme");
    EXPECT_NE(std::string_view::npos, hint.find("pqopt={sslrootcert="));
}

namespace {
    std::string OverrideWarning(const std::string& mode) {
        return "SSLmode=" + mode + " was changed to verify-full: Aurora DSQL requires TLS. Set SSLmode to verify-full, "
               "verify-ca or require.";
    }
}  // namespace

// The override reaches the app, not only the log: SQL_SUCCESS_WITH_INFO with 01S02 (option value changed).
TEST_F(DsqlAuthPluginTest, Connect_SslModeOverridden_ReturnsOptionValueChanged) {
    dbc_->conn_attr.insert_or_assign("SSLMODE", "disable");
    ExpectTokenForRegion("us-west-2");
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, ConnectWithMockProvider());
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("01S02", dbc_->err->sqlstate);
    EXPECT_EQ(WARN_OPTION_VALUE_CHANGED, dbc_->err->native_err);
    EXPECT_EQ(OverrideWarning("disable"), dbc_->err->error_msg);
    EXPECT_EQ("verify-full", dbc_->conn_attr.at("SSLMODE"));
}

TEST_F(DsqlAuthPluginTest, Connect_KeptOrDefaultSslMode_NoWarning) {
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(SQL_SUCCESS));
    for (const std::string mode : {"", "verify-full", "VERIFY-FULL", "Verify-CA", "require"}) {
        dbc_->conn_attr.erase("SSLMODE");
        dbc_->conn_attr.erase("PQOPT");
        if (!mode.empty()) {
            dbc_->conn_attr.insert_or_assign("SSLMODE", mode);
        }
        EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider()) << mode;
        EXPECT_EQ(nullptr, dbc_->err) << mode;
    }
}

// The wrapper keeps one record per handle, and its record hides the base driver's. When psqlODBC has its own
// warnings (a short output buffer gets 01004), they stay; the override is only logged.
TEST_F(DsqlAuthPluginTest, Connect_SslModeOverridden_KeepsBaseDriverWarnings) {
    dbc_->conn_attr.insert_or_assign("SSLMODE", "prefer");
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(SQL_SUCCESS_WITH_INFO));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, ConnectWithMockProvider());
    EXPECT_EQ(nullptr, dbc_->err);
}

TEST_F(DsqlAuthPluginTest, Connect_SslModeOverridden_ConnectFails_NoWarning) {
    dbc_->conn_attr.insert_or_assign("SSLMODE", "disable");
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(SQL_ERROR));
    EXPECT_EQ(SQL_ERROR, ConnectWithMockProvider());
    EXPECT_EQ(nullptr, dbc_->err);
}

// Only the three long names are kept; psqlODBC's short forms (d, a, p, r, vc, vf) are replaced too.
TEST_F(DsqlAuthPluginTest, TlsOverrideWarning_OnlyForReplacedModes) {
    for (const std::string mode : {"disable", "allow", "prefer", "d", "vf", "verify-fulll"}) {
        EXPECT_EQ(OverrideWarning(mode), DsqlAuthPlugin::GetTlsOverrideWarning({{"SSLMODE", mode}})) << mode;
    }
    EXPECT_EQ(OverrideWarning("allow"), DsqlAuthPlugin::GetTlsOverrideWarning({{"CA", "allow"}}));
    for (const std::string mode : {"verify-full", "VERIFY-FULL", "VERIFY-CA", "require", "Require"}) {
        EXPECT_EQ("", DsqlAuthPlugin::GetTlsOverrideWarning({{"SSLMODE", mode}})) << mode;
    }
    EXPECT_EQ("", DsqlAuthPlugin::GetTlsOverrideWarning({}));
}

// The mode names are compared in ASCII. ICU's default locale comes from the environment, and under tr it upper-cases
// the i of verify-full to U+0130 (capital I with dot), so an upper-case VERIFY-FULL, VERIFY-CA or REQUIRE didn't match
// and was replaced, with a 01S02.
TEST_F(DsqlAuthPluginTest, SslMode_TurkishIcuLocale_UpperCaseModesKept) {
    struct IcuLocaleGuard {
        std::string saved;
        ~IcuLocaleGuard() {
            UErrorCode status = U_ZERO_ERROR;
            uloc_setDefault(saved.c_str(), &status);
        }
    };
    const IcuLocaleGuard guard{.saved = uloc_getDefault()};
    UErrorCode status = U_ZERO_ERROR;
    uloc_setDefault("tr_TR", &status);
    ASSERT_TRUE(U_SUCCESS(status)) << u_errorName(status);

    CapturingLogSink sink;
    nglog::AddLogSink(&sink);
    for (const auto& [requested, expected] : std::map<std::string, std::string>{
             {"VERIFY-FULL", "verify-full"}, {"VERIFY-CA", "verify-ca"}, {"REQUIRE", "require"}}) {
        EXPECT_EQ("", DsqlAuthPlugin::GetTlsOverrideWarning({{"SSLMODE", requested}})) << requested;
        std::map<std::string, std::string> conn_attr = {{"SSLMODE", requested}};
        DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
        EXPECT_EQ(expected, conn_attr.at("SSLMODE")) << requested;
    }
    nglog::RemoveLogSink(&sink);
    EXPECT_EQ(0, sink.Count("Overriding SSLmode"));
}

// An expired AWS IAM Identity Center (SSO) session is the usual cause with a named profile, so the error says how to
// renew it.
TEST_F(DsqlAuthPluginTest, Connect_UnresolvedCredentials_NamesTheProfileToLogIn) {
    mock_auth_provider_->mock_has_resolved_credentials = false;
    dbc_->conn_attr.insert_or_assign(KEY_AWS_PROFILE, "dev");
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));

    EXPECT_EQ(SQL_ERROR, ConnectWithMockProvider());
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("Unable to resolve AWS credentials for DSQL authentication with the 'AWS_PROFILE' profile 'dev'. If it "
              "is an AWS IAM Identity Center (SSO) profile, run 'aws sso login --profile dev'.", dbc_->err->error_msg);
}

// The default chain reads the AWS_PROFILE variable too, so that profile is named.
TEST_F(DsqlAuthPluginTest, Connect_UnresolvedCredentials_EnvProfileInHint) {
    mock_auth_provider_->mock_has_resolved_credentials = false;
    SetEnvVar("AWS_PROFILE", "env_profile");
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));

    EXPECT_EQ(SQL_ERROR, ConnectWithMockProvider());
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_NE(std::string::npos, dbc_->err->error_msg.find("run 'aws sso login --profile env_profile'"));
}

// AWS_PROFILE=default exported in the shell names a profile too: an SSO profile called default needs the same login.
TEST_F(DsqlAuthPluginTest, Connect_UnresolvedCredentials_DefaultProfileFromEnvIsNamed) {
    mock_auth_provider_->mock_has_resolved_credentials = false;
    SetEnvVar("AWS_PROFILE", "default");
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));

    EXPECT_EQ(SQL_ERROR, ConnectWithMockProvider());
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("Unable to resolve AWS credentials for DSQL authentication with the 'AWS_PROFILE' profile 'default'. "
              "If it is an AWS IAM Identity Center (SSO) profile, run 'aws sso login --profile default'.",
              dbc_->err->error_msg);
}

// AWS_PROFILE, the key and the variable, is trimmed like the region sources: a blank one counts as unset.
TEST_F(DsqlAuthPluginTest, ClusterId_BlankProfileKey_UsesEnvProfileRegion) {
    SetEnvVar("AWS_PROFILE", "dev");  // region = us-west-2 in the fixture
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_AWS_PROFILE, " \t");
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.us-west-2.on.aws", conn_attr.at(KEY_SERVER));
}

TEST_F(DsqlAuthPluginTest, ClusterId_PaddedProfile_UsesItsRegion) {
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    conn_attr.insert_or_assign(KEY_AWS_PROFILE, " dev ");
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.us-west-2.on.aws", conn_attr.at(KEY_SERVER));

    SetEnvVar("AWS_PROFILE", "dev\r");
    conn_attr = DsqlAttributes(CLUSTER_ID);
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.us-west-2.on.aws", conn_attr.at(KEY_SERVER));
}

// A blank AWS_PROFILE key names no profile, so the error is the default chain's.
TEST_F(DsqlAuthPluginTest, Connect_UnresolvedCredentials_BlankProfileKeyIsTheDefaultChain) {
    mock_auth_provider_->mock_has_resolved_credentials = false;
    dbc_->conn_attr.insert_or_assign(KEY_AWS_PROFILE, "  ");
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));

    EXPECT_EQ(SQL_ERROR, ConnectWithMockProvider());
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("Unable to resolve AWS credentials for DSQL authentication from the default credential chain (such as "
              "environment variables, ~/.aws/credentials, an AWS IAM Identity Center (SSO) session, or a container "
              "or instance role)", dbc_->err->error_msg);
}

namespace {
    // Only the static keys in the credentials fixture can sign: aws_credentials has them under [dsql_static],
    // aws_credentials_default under [default]. CI exports role credentials, which the environment provider would
    // answer with first; a container (ECS, CodeBuild) or EKS web identity host would answer through its endpoint or
    // STS; and IMDS is off.
    ScopedEnvVars OnlyProfileCredentials(const std::string& fixture = "aws_credentials") {
        return ScopedEnvVars({{"AWS_SHARED_CREDENTIALS_FILE",
                               std::string(AWS_PROFILE_TEST_RESOURCES_DIR) + "/" + fixture},
                              {"AWS_ACCESS_KEY_ID", std::nullopt},
                              {"AWS_SECRET_ACCESS_KEY", std::nullopt},
                              {"AWS_SESSION_TOKEN", std::nullopt},
                              {"AWS_CONTAINER_CREDENTIALS_RELATIVE_URI", std::nullopt},
                              {"AWS_CONTAINER_CREDENTIALS_FULL_URI", std::nullopt},
                              {"AWS_ROLE_ARN", std::nullopt},
                              {"AWS_WEB_IDENTITY_TOKEN_FILE", std::nullopt},
                              {"AWS_EC2_METADATA_DISABLED", "true"}});
    }
}  // namespace

// The real provider gets the trimmed profile, from the key or from the variable. The SDK would read the variable as
// is and look for a profile named "dsql_static ".
TEST_F(DsqlAuthPluginTest, Connect_PaddedProfile_SignsWithThatProfile) {
    const ScopedEnvVars env = OnlyProfileCredentials();
    for (const bool from_key : {true, false}) {
        dbc_->conn_attr.erase(KEY_AWS_PROFILE);
        UnsetEnvVar("AWS_PROFILE");
        if (from_key) {
            dbc_->conn_attr.insert_or_assign(KEY_AWS_PROFILE, " dsql_static ");
        } else {
            SetEnvVar("AWS_PROFILE", "dsql_static ");
        }
        EXPECT_EQ(SQL_SUCCESS, ConnectWithRealProvider()) << from_key;
        EXPECT_EQ(nullptr, dbc_->err) << (dbc_->err ? dbc_->err->error_msg : "");
    }
}

// With the AWS_PROFILE key only that profile signs: one that is in neither file fails the connect and is named,
// though the environment has keys.
TEST_F(DsqlAuthPluginTest, Connect_MissingProfileKey_FailsWithItsName) {
    const ScopedEnvVars env = OnlyProfileCredentials();
    const ScopedEnvVars env_keys({
        {"AWS_ACCESS_KEY_ID", "AKIDENVEXAMPLE"},
        {"AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYENVEXAMPLE"},
    });
    dbc_->conn_attr.insert_or_assign(KEY_AWS_PROFILE, "no_such_profile");
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));

    DsqlAuthPlugin plugin(dbc_, mock_base_plugin_);
    EXPECT_EQ(SQL_ERROR, plugin.Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("28000", dbc_->err->sqlstate);
    EXPECT_EQ("Unable to resolve AWS credentials for DSQL authentication: the 'AWS_PROFILE' profile 'no_such_profile' "
              "isn't in the AWS config or credentials file (~/.aws/config, ~/.aws/credentials, or the files "
              "AWS_CONFIG_FILE and AWS_SHARED_CREDENTIALS_FILE name). Check the name, or remove AWS_PROFILE to use the "
              "default credential chain.", dbc_->err->error_msg);
}

// A blank AWS_PROFILE variable counts as unset, so the provider is given [default] explicitly: the SDK would read
// the variable as is and look for a profile named " \t".
TEST_F(DsqlAuthPluginTest, Connect_BlankEnvProfile_SignsWithTheDefaultProfile) {
    const ScopedEnvVars env = OnlyProfileCredentials("aws_credentials_default");
    SetEnvVar("AWS_PROFILE", " \t");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithRealProvider());
    EXPECT_EQ(nullptr, dbc_->err) << (dbc_->err ? dbc_->err->error_msg : "");
}

TEST_F(DsqlAuthPluginTest, ClusterId_BlankEnvProfile_UsesDefaultProfileRegion) {
    UseConfigFile("aws_config_with_default");  // [default] region = eu-north-1
    SetEnvVar("AWS_PROFILE", " ");
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-north-1.on.aws", conn_attr.at(KEY_SERVER));
}

#ifndef _WIN32
// An empty AWS_PROFILE variable (export AWS_PROFILE=) counts as unset too: the SDK would look up a profile named "".
// POSIX only, because on Windows _putenv_s with "" deletes the variable.
TEST_F(DsqlAuthPluginTest, Connect_EmptyEnvProfile_SignsWithTheDefaultProfile) {
    const ScopedEnvVars env = OnlyProfileCredentials("aws_credentials_default");
    SetEnvVar("AWS_PROFILE", "");
    ASSERT_EQ(std::optional<std::string>(""), test_env::GetEnvVar("AWS_PROFILE"));
    EXPECT_EQ(SQL_SUCCESS, ConnectWithRealProvider());
    EXPECT_EQ(nullptr, dbc_->err) << (dbc_->err ? dbc_->err->error_msg : "");
}

TEST_F(DsqlAuthPluginTest, ClusterId_EmptyEnvProfile_UsesDefaultProfileRegion) {
    UseConfigFile("aws_config_with_default");  // [default] region = eu-north-1
    SetEnvVar("AWS_PROFILE", "");
    std::map<std::string, std::string> conn_attr = DsqlAttributes(CLUSTER_ID);
    std::string error;
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error)) << error;
    EXPECT_EQ(CLUSTER_ID + ".dsql.eu-north-1.on.aws", conn_attr.at(KEY_SERVER));
}
#endif
