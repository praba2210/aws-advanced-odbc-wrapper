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
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "auth_mock_objects.h"
#include "common_mock_objects.h"

#include "../../driver/driver.h"
#include "../../driver/plugin/dsql/dsql_auth_plugin.h"
#include "../../driver/util/aws_sdk_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/logger_wrapper.h"

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
    // CI exports AWS_REGION and AWS_DEFAULT_REGION, and a developer's ~/.aws/config may set a default region. Each
    // test sees only the regions it sets, with the fixture config file in place of the user's.
    std::map<std::string, std::optional<std::string>> saved_env_;

    static void SetUpTestSuite() {
        AwsSdkHelper::Init();
    }
    static void TearDownTestSuite() {
        AwsSdkHelper::Shutdown();
    }

    void SetUp() override {
        for (const char* name : {"AWS_REGION", "AWS_DEFAULT_REGION", "AWS_PROFILE", "AWS_CONFIG_FILE"}) {
            saved_env_[name] = GetEnvVar(name);
            UnsetEnvVar(name);
        }
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
        for (const auto& [name, value] : saved_env_) {
            RestoreEnvVar(name.c_str(), value);
        }
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
    EXPECT_STREQ("Unable to resolve AWS credentials for DSQL authentication", dbc_->err->error_msg);
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

    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
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
        const std::string hint = DsqlAuthPlugin::GetRootCertHint(conn_attr, error);
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
