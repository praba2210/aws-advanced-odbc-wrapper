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

#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "auth_mock_objects.h"
#include "common_mock_objects.h"
#include "scoped_env_vars.h"

#include "../../driver/dialect/dialect_aurora_dsql.h"
#include "../../driver/driver.h"
#include "../../driver/plugin/dsql/dsql_auth_plugin.h"
#include "../../driver/plugin/dsql/dsql_plugin_helper.h"
#include "../../driver/util/aws_sdk_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/logger_wrapper.h"
#include "../../driver/util/rds_lib_loader.h"

#ifndef AWS_PROFILE_TEST_RESOURCES_DIR
    #error "AWS_PROFILE_TEST_RESOURCES_DIR must be defined by the build"
#endif

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-west-2.on.aws";
    const std::string PORT = "5432";
    const std::string USERNAME = "admin";
    const std::pair<std::string, bool> FRESH_TOKEN("dsql_token", false);

    // Whether a presigned token was signed with the access key.
    bool SignedWith(const std::string& token, const std::string& access_key) {
        return token.find("X-Amz-Credential=" + access_key + "%2F") != std::string::npos;
    }

    using test_env::SetEnvVar;
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
        dbc_->conn_attr.insert_or_assign(KEY_AUTH_TYPE, VALUE_AUTH_DSQL);
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

    // Connects through the plugin, whose base plugin is expected once and returns base_result, and returns the token
    // the base plugin got.
    std::string ConnectAndCaptureToken(DsqlAuthPlugin& plugin, const SQLRETURN base_result = SQL_SUCCESS) {
        std::string token;
        EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
            .WillOnce(testing::Invoke([this, &token, base_result](SQLHDBC, SQLHWND, SQLTCHAR*, SQLSMALLINT,
                                                                  SQLSMALLINT*, SQLUSMALLINT) {
                token = dbc_->conn_attr.at(KEY_DB_PASSWORD);
                return base_result;
            }));
        EXPECT_EQ(base_result, plugin.Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT))
            << (dbc_->err ? dbc_->err->error_msg : "");
        testing::Mock::VerifyAndClearExpectations(mock_base_plugin_.get());
        return token;
    }
};

TEST_F(DsqlAuthPluginTest, Connect_RegionFromDsqlHost) {
    EXPECT_CALL(*mock_auth_provider_, GetToken(DSQL_HOST, "us-west-2", PORT, USERNAME,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::Return(FRESH_TOKEN));
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(1))
        .WillOnce(testing::Invoke([this](SQLHDBC, SQLHWND, SQLTCHAR*, SQLSMALLINT, SQLSMALLINT*, SQLUSMALLINT) {
            // The token is dropped after the connect, so it's read while the base driver connects.
            EXPECT_EQ(FRESH_TOKEN.first, dbc_->conn_attr.at(KEY_DB_PASSWORD));
            return SQL_SUCCESS;
        }));

    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

// The token is for this connect only: a reconnect signs a new one, and nothing reads PWD after the connect.
TEST_F(DsqlAuthPluginTest, Connect_Dsql_ErasesThePasswordAfterTheConnect) {
    for (const SQLRETURN base_result : std::initializer_list<SQLRETURN>{SQL_SUCCESS, SQL_ERROR}) {
        dbc_->conn_attr.insert_or_assign(KEY_DB_PASSWORD, "app_password");
        EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
            testing::_, testing::_, testing::_))
            .WillOnce(testing::Return(FRESH_TOKEN));
        EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
            .WillOnce(testing::Invoke([this, base_result](SQLHDBC, SQLHWND, SQLTCHAR*, SQLSMALLINT, SQLSMALLINT*,
                                                          SQLUSMALLINT) {
                EXPECT_EQ(FRESH_TOKEN.first, dbc_->conn_attr.at(KEY_DB_PASSWORD));
                return base_result;
            }));

        EXPECT_EQ(base_result, ConnectWithMockProvider());
        EXPECT_FALSE(dbc_->conn_attr.contains(KEY_DB_PASSWORD));
        EXPECT_EQ(USERNAME, dbc_->conn_attr.at(KEY_DB_USERNAME));  // only PWD goes
        testing::Mock::VerifyAndClearExpectations(mock_auth_provider_.get());
        testing::Mock::VerifyAndClearExpectations(mock_base_plugin_.get());
    }
}

TEST_F(DsqlAuthPluginTest, Connect_RegionKeyOverridesHost) {
    dbc_->conn_attr.insert_or_assign(KEY_REGION, "eu-west-1");
    ExpectTokenForRegion("eu-west-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

TEST_F(DsqlAuthPluginTest, Connect_RegionKeyIsTrimmed) {
    dbc_->conn_attr.insert_or_assign(KEY_REGION, " EU-WEST-1\t");
    ExpectTokenForRegion("eu-west-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

// The token is signed for IAM_HOST, so its region wins over the one in SERVER.
TEST_F(DsqlAuthPluginTest, Connect_RegionFromIamHostBeforeServer) {
    dbc_->conn_attr.insert_or_assign(KEY_IAM_HOST, "abcdefghijklmnopqrstuvwxyz.dsql.eu-west-1.on.aws");
    ExpectTokenForRegion("eu-west-1");
    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
}

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

TEST_F(DsqlAuthPluginTest, Connect_RegionFromProfileWhenNoHostOrEnvRegion) {
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

// An expired AWS IAM Identity Center (SSO) session is the usual cause with a named profile, so the error says how to
// renew it.
TEST_F(DsqlAuthPluginTest, Connect_UnresolvedCredentials_NamesTheProfileToLogIn) {
    mock_auth_provider_->mock_has_resolved_credentials = false;
    dbc_->conn_attr.insert_or_assign(KEY_AWS_PROFILE, "dev");
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));

    EXPECT_EQ(SQL_ERROR, ConnectWithMockProvider());
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("Unable to resolve AWS credentials for DSQL authentication with the 'AWS_PROFILE' profile 'dev'. If it "
              "is an AWS IAM Identity Center (SSO) profile, run 'aws sso login --profile dev'.", dbc_->err->error_msg);
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

// psqlODBC reads Servername as SERVER (the later one in the string wins, and the wrapper writes SERVER first), so a
// SERVERNAME would send the token to a host it wasn't signed for. A DSQL connect folds it into SERVER.
TEST_F(DsqlAuthPluginTest, Defaults_ServernameOnly_BecomesServer) {
    std::map<std::string, std::string> conn_attr = {{"SERVERNAME", "abc.dsql.us-east-1.on.aws"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("abc.dsql.us-east-1.on.aws", conn_attr.at(KEY_SERVER));
    EXPECT_FALSE(conn_attr.contains("SERVERNAME"));
}

TEST_F(DsqlAuthPluginTest, Defaults_ServerAndServername_ServernameErased) {
    std::map<std::string, std::string> conn_attr = {{"SERVER", "a.dsql.us-east-1.on.aws"},
                                                    {"SERVERNAME", "b.example.com"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("a.dsql.us-east-1.on.aws", conn_attr.at(KEY_SERVER));
    EXPECT_FALSE(conn_attr.contains("SERVERNAME"));
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

// psqlODBC writes these short values under the short key CA: r, vc and vf are require, verify-ca and verify-full.
TEST_F(DsqlAuthPluginTest, Defaults_KeepsShortSslModes) {
    for (const auto& [requested, expected] : std::map<std::string, std::string>{
             {"r", "require"}, {"vc", "verify-ca"}, {"vf", "verify-full"}}) {
        std::map<std::string, std::string> conn_attr = {{"CA", requested}};
        EXPECT_EQ("", DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr)) << requested;
        EXPECT_EQ(expected, conn_attr.at("SSLMODE")) << requested;
    }
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

// A single-quoted value can hold spaces and '=' (PrivateLink: options='-c amzn-cluster-id=<id>'); nothing inside it
// is a keyword. Spaces around '=' are allowed.
TEST_F(DsqlAuthPluginTest, Defaults_PqoptSkipsQuotedValues) {
    std::map<std::string, std::string> conn_attr = {
        {"PQOPT", "{options='-c sslrootcert=x' application_name = 'billing app'}"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    EXPECT_EQ("{options='-c sslrootcert=x' application_name = 'billing app' sslrootcert=system}", conn_attr.at("PQOPT"));
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
    for (const std::string& error : {OLD_LIBPQ_ROOT_CERT_ERROR, std::string("SSL error: certificate verify failed"),
                                      std::string("SSL error: unregistered scheme")}) {
        const std::string_view hint = DsqlAuthPlugin::GetRootCertHint(conn_attr, error);
        EXPECT_NE(std::string::npos, hint.find("libpq 16")) << error;
        EXPECT_NE(std::string::npos, hint.find("pqopt={sslrootcert=")) << error;
    }
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

// The hint comes from the error the base plugin already read: the error is read once, and the hint logged once. The
// connect sets sslrootcert=system, so the hint doesn't depend on the host's CA bundles.
TEST_F(DsqlAuthPluginTest, Connect_RootCertError_LogsHintOnce) {
    dbc_->conn_attr.insert_or_assign("PQOPT", "{sslrootcert=system}");
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

// Two different hosts: the token is signed for one, and the other would be dropped silently (a DSN's SERVER would
// beat the connection string's SERVERNAME). The same host in any case folds quietly.
TEST_F(DsqlAuthPluginTest, ClusterId_ServerAndServernameDiffer_Fails) {
    std::map<std::string, std::string> conn_attr = {{KEY_AUTH_TYPE, VALUE_AUTH_DSQL}, {"SERVER", "a.example.com"},
                                                    {"SERVERNAME", "b.example.com"},};
    std::string error;
    EXPECT_FALSE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ("DSQL Authentication failed. SERVER and SERVERNAME name different hosts ('a.example.com' and "
              "'b.example.com'): set only one of them", error);

    conn_attr = {{KEY_AUTH_TYPE, VALUE_AUTH_DSQL}, {"SERVER", "A.Example.com"}, {"Servername", "a.example.COM"}};
    EXPECT_TRUE(DsqlAuthPlugin::ExpandClusterId(conn_attr, error));
    EXPECT_EQ("A.Example.com", conn_attr.at("SERVER"));
    EXPECT_FALSE(conn_attr.contains("Servername"));
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

    // Not a DNS host label: an inner space, a ';' (it would end SERVER in psqlODBC's connection string), a leading
    // hyphen.
    const std::vector<std::string> INVALID_REGIONS = {"us east-1", "us-east-1;port=1", "-us-east-1"};
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

// With OTHER_AUTH a mode psqlODBC runs, in any form or under either keyword, stays exactly as set, pqopt included, and
// no text is returned.
TEST_F(DsqlAuthPluginTest, TlsDefaults_OtherAuth_KeepsAModePsqlodbcRuns) {
    for (const auto& [key, mode] : std::vector<std::pair<std::string, std::string>>{
             {"SSLMODE", "allow"}, {"SSLMODE", "prefer"}, {"SSLMODE", "require"}, {"SSLMODE", "r"},
             {"SSLMODE", "verify-ca"}, {"SSLMODE", "verify-full"}, {"CA", "a"}, {"CA", "p"}, {"CA", "vf"},}) {
        std::map<std::string, std::string> conn_attr = {{key, mode}, {"PQOPT", "{application_name=billing}"}};
        const std::map<std::string, std::string> before = conn_attr;
        EXPECT_EQ("", DsqlAuthPlugin::ApplyTlsDefaults(conn_attr, DsqlAuthPlugin::TlsPolicy::OTHER_AUTH, "system"))
            << key << "=" << mode;
        EXPECT_EQ(before, conn_attr) << key << "=" << mode;
    }
}

// With OTHER_AUTH only a mode psqlODBC would run as disable changes: a mode name in another case is lowercased with no
// text, and anything else becomes verify-full with the 01S02 text, or none when no mode was set.
TEST_F(DsqlAuthPluginTest, TlsDefaults_OtherAuth_RaisesAModePsqlodbcRunsAsDisable) {
    for (const auto& [requested, expected, text] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"disable", "verify-full", OverrideWarning("disable")},
             {"on", "verify-full", OverrideWarning("on")},
             {"Prefer", "prefer", ""},
             {"Require", "require", ""},
             {"VERIFY-FULL", "verify-full", ""},}) {
        std::map<std::string, std::string> conn_attr = {{"SSLMODE", requested}};
        EXPECT_EQ(text, DsqlAuthPlugin::ApplyTlsDefaults(conn_attr, DsqlAuthPlugin::TlsPolicy::OTHER_AUTH, "system"))
            << requested;
        EXPECT_EQ(expected, conn_attr.at("SSLMODE")) << requested;
    }
    std::map<std::string, std::string> unset;
    EXPECT_EQ("", DsqlAuthPlugin::ApplyTlsDefaults(unset, DsqlAuthPlugin::TlsPolicy::OTHER_AUTH, "system"));
    EXPECT_EQ("verify-full", unset.at("SSLMODE"));
    EXPECT_EQ("{sslrootcert=system application_name=" + EXPECTED_APP_NAME + "}", unset.at("PQOPT"));
}

// Only the three kept modes get no warning; psqlODBC's short forms of the others (d, a, p) are replaced too.
TEST_F(DsqlAuthPluginTest, TlsOverrideWarning_OnlyForReplacedModes) {
    const auto warning = [](std::map<std::string, std::string> conn_attr) {
        return DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr);
    };
    for (const std::string mode : {"disable", "allow", "prefer", "d", "verify-fulll"}) {
        EXPECT_EQ(OverrideWarning(mode), warning({{"SSLMODE", mode}})) << mode;
    }
    EXPECT_EQ(OverrideWarning("allow"), warning({{"CA", "allow"}}));
    for (const std::string mode : {"verify-full", "VERIFY-FULL", "VERIFY-CA", "require", "Require"}) {
        EXPECT_EQ("", warning({{"SSLMODE", mode}})) << mode;
    }
    EXPECT_EQ("", warning({}));
}

namespace {
    // Only the static keys in the credentials fixture can sign: aws_credentials has them under [dsql_static]. CI
    // exports role credentials, which the environment provider would
    // answer with first; a container (ECS, CodeBuild) or EKS web identity host would answer through its endpoint or
    // STS; and IMDS is off.
    ScopedEnvVars OnlyProfileCredentials() {
        return ScopedEnvVars({{"AWS_SHARED_CREDENTIALS_FILE",
                               std::string(AWS_PROFILE_TEST_RESOURCES_DIR) + "/aws_credentials"},
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

namespace {
    // A connect on a reused handle starts from a fresh conn_attr.
    void ResetAttributes(DBC* dbc, const std::string& profile_key) {
        dbc->conn_attr.clear();
        dbc->conn_attr.insert_or_assign(KEY_SERVER, DSQL_HOST);
        dbc->conn_attr.insert_or_assign(KEY_PORT, PORT);
        dbc->conn_attr.insert_or_assign(KEY_DB_USERNAME, USERNAME);
        dbc->conn_attr.insert_or_assign(KEY_AUTH_TYPE, VALUE_AUTH_DSQL);
        dbc->conn_attr.insert_or_assign(KEY_AWS_PROFILE, profile_key);
    }
}  // namespace

// A handle keeps its first connect's plugin chain, so the plugin builds the provider again for another profile.
TEST_F(DsqlAuthPluginTest, Connect_ReusedPlugin_ProfileKeyChange_RebuildsTheProvider) {
    const ScopedEnvVars env = OnlyProfileCredentials();
    dbc_->conn_attr.insert_or_assign(KEY_AWS_PROFILE, "dsql_static");
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(SQL_SUCCESS));
    DsqlAuthPlugin plugin(dbc_, mock_base_plugin_);
    ASSERT_EQ(SQL_SUCCESS, plugin.Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    testing::Mock::VerifyAndClearExpectations(mock_base_plugin_.get());

    ResetAttributes(dbc_, "no_such_profile");
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));
    EXPECT_EQ(SQL_ERROR, plugin.Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("28000", dbc_->err->sqlstate);
    EXPECT_NE(std::string::npos, dbc_->err->error_msg.find("'no_such_profile'")) << dbc_->err->error_msg;
}

// A profile that was missing when the chain was built stays missing for that chain: the connect after the user
// created it gets a new one.
TEST_F(DsqlAuthPluginTest, Connect_AfterCredentialFailure_RebuildsTheProvider) {
    const TempConfigFile credentials("[other]\naws_access_key_id = AKIDOTHER\naws_secret_access_key = otherSecret\n");
    const ScopedEnvVars env = OnlyProfileCredentials();
    SetEnvVar("AWS_SHARED_CREDENTIALS_FILE", credentials.Path());
    dbc_->conn_attr.insert_or_assign(KEY_AWS_PROFILE, "p");
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return(SQL_SUCCESS));

    DsqlAuthPlugin plugin(dbc_, mock_base_plugin_);
    EXPECT_EQ(SQL_ERROR, plugin.Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_NE(nullptr, dbc_->err);
    EXPECT_EQ("28000", dbc_->err->sqlstate);

    credentials.Write("[p]\naws_access_key_id = AKIDEXAMPLE\n"
                      "aws_secret_access_key = wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY\n");
    ResetAttributes(dbc_, "p");
    EXPECT_EQ(SQL_SUCCESS, plugin.Connect(dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
}

// A connect without RDS_AUTH_TYPE=DSQL on the same handle gets no token and keeps the application's PWD, and the
// plugin's defaults aren't applied: the SSLmode stays as set and no pqopt is added.
TEST_F(DsqlAuthPluginTest, Connect_NotDsqlAuthType_PassesThrough) {
    dbc_->conn_attr.insert_or_assign(KEY_AUTH_TYPE, "DATABASE");
    dbc_->conn_attr.insert_or_assign(KEY_DB_PASSWORD, "app_password");
    dbc_->conn_attr.insert_or_assign("SSLMODE", "disable");
    EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_,
        testing::_, testing::_, testing::_))
        .Times(testing::Exactly(0));
    std::map<std::string, std::string> conn_attr_seen;
    EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Invoke([&conn_attr_seen](SQLHDBC handle, SQLHWND, SQLTCHAR*, SQLSMALLINT, SQLSMALLINT*,
                                                    SQLUSMALLINT) {
            conn_attr_seen = static_cast<DBC*>(handle)->conn_attr;
            return SQL_SUCCESS;
        }));

    EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider());
    EXPECT_EQ("app_password", conn_attr_seen.at(KEY_DB_PASSWORD));
    EXPECT_EQ("disable", conn_attr_seen.at("SSLMODE"));
    EXPECT_FALSE(conn_attr_seen.contains("PQOPT"));
    EXPECT_FALSE(conn_attr_seen.contains("D5"));
}

// The environment variables the chain reads when it is built are inputs of the provider too: with another credentials
// file the provider is built again, for the same profile and region. (A provider reads its profile's static keys from
// the file it was built with.)
TEST_F(DsqlAuthPluginTest, Connect_ReusedPlugin_CredentialsFileChange_RebuildsTheProvider) {
    const TempConfigFile first("[p]\naws_access_key_id = AKIDFIRST\naws_secret_access_key = firstSecret\n");
    const TempConfigFile second("[p]\naws_access_key_id = AKIDSECOND\naws_secret_access_key = secondSecret\n");
    const ScopedEnvVars env = OnlyProfileCredentials();
    SetEnvVar("AWS_SHARED_CREDENTIALS_FILE", first.Path());
    ResetAttributes(dbc_, "p");
    DsqlAuthPlugin plugin(dbc_, mock_base_plugin_);
    EXPECT_TRUE(SignedWith(ConnectAndCaptureToken(plugin), "AKIDFIRST"));

    SetEnvVar("AWS_SHARED_CREDENTIALS_FILE", second.Path());
    ResetAttributes(dbc_, "p");
    EXPECT_TRUE(SignedWith(ConnectAndCaptureToken(plugin), "AKIDSECOND"));
}

#ifndef _WIN32
namespace {
    // Credentials file text for profile p, with keys from a credential_process (which the provider built for it runs on
    // every read) or static ones (which it reads from the file every time). A provider keeps the kind it was built
    // for, so after the file changes from one to the other, the key that signs shows whether it was built again.
    std::string ProcessProfile(const std::string& access_key) {
        return "[p]\ncredential_process = echo '{\"Version\": 1, \"AccessKeyId\": \"" + access_key
            + "\", \"SecretAccessKey\": \"processSecret\"}'\n";
    }
}  // namespace

// When the server rejects the token (keys rotated or revoked since the chain read them), the next connect builds the
// provider again, so one that holds the old keys doesn't keep failing. psqlODBC reports Aurora DSQL's "access denied"
// as 08001, so that message counts as well as 28000 and 28P01; another error keeps the provider.
TEST_F(DsqlAuthPluginTest, Connect_AfterTheServerRejectsTheToken_RebuildsTheProvider) {
    struct BaseError {
        std::string sql_state;
        std::string message;
        bool rebuilt;
    };
    for (const BaseError& error : std::vector<BaseError>{
             {.sql_state = "28P01", .message = "FATAL:  password authentication failed", .rebuilt = true},
             {.sql_state = "28000", .message = "", .rebuilt = true},
             {.sql_state = "08001", .message = "FATAL:  unable to accept connection, access denied", .rebuilt = true},
             {.sql_state = "08001", .message = "could not connect to server: Connection refused", .rebuilt = false},}) {
        SCOPED_TRACE(error.sql_state + " " + error.message);
        const TempConfigFile credentials(ProcessProfile("AKIDPROCESS"));
        const ScopedEnvVars env = OnlyProfileCredentials();
        SetEnvVar("AWS_SHARED_CREDENTIALS_FILE", credentials.Path());
        ResetAttributes(dbc_, "p");
        EXPECT_CALL(*mock_odbc_helper_, GetSqlStateAndLogMessage(testing::An<DBC*>(), testing::_))
            .WillOnce(testing::DoAll(testing::SetArgReferee<1>(error.message), testing::Return(error.sql_state)));
        DsqlAuthPlugin plugin(dbc_, mock_base_plugin_, nullptr, std::make_shared<DialectAuroraDsql>(),
                              mock_odbc_helper_);
        EXPECT_TRUE(SignedWith(ConnectAndCaptureToken(plugin, SQL_ERROR), "AKIDPROCESS"));

        credentials.Write("[p]\naws_access_key_id = AKIDSTATIC\naws_secret_access_key = staticSecret\n");
        ResetAttributes(dbc_, "p");
        EXPECT_TRUE(SignedWith(ConnectAndCaptureToken(plugin), error.rebuilt ? "AKIDSTATIC" : "AKIDPROCESS"));
        testing::Mock::VerifyAndClearExpectations(mock_odbc_helper_.get());
    }
}
#endif

// libpq before 16 reads sslrootcert=system as a file named "system" in the working directory, so the plugin adds a
// system CA bundle there instead. An unknown version counts as older than 16, except on Windows, where no bundle is
// trusted, so it keeps system. The log says the version is unknown.
TEST_F(DsqlAuthPluginTest, DefaultRootCert_Libpq16OrUnknown) {
    CapturingLogSink sink;
    nglog::AddLogSink(&sink);
    const auto never_trusted = [](const char*) { return false; };
    EXPECT_EQ("system", DsqlAuthPlugin::DefaultRootCert(160000, never_trusted));
    EXPECT_EQ("system", DsqlAuthPlugin::DefaultRootCert(180001, never_trusted));
#ifdef _WIN32
    EXPECT_EQ("system", DsqlAuthPlugin::DefaultRootCert(std::nullopt, never_trusted));
    const std::string unknown_log = "[DSQL] The base driver's libpq version is unknown; the sslrootcert default is system.";
#else
    EXPECT_EQ("/etc/ssl/certs/ca-certificates.crt", DsqlAuthPlugin::DefaultRootCert(std::nullopt,
        [](const char*) { return true; }));
    const std::string unknown_log = "[DSQL] The base driver's libpq version is unknown; treating it as older than 16.";
#endif
    nglog::RemoveLogSink(&sink);
    EXPECT_EQ(1, sink.Count(unknown_log));
    EXPECT_EQ(1, sink.Count("libpq version"));
}

TEST_F(DsqlAuthPluginTest, DefaultRootCert_Libpq15_UsesTheFirstTrustedBundle) {
    EXPECT_EQ("/etc/pki/tls/certs/ca-bundle.crt", DsqlAuthPlugin::DefaultRootCert(150004,
        [](const char* path) { return std::string(path) == "/etc/pki/tls/certs/ca-bundle.crt"; }));
    EXPECT_EQ("/etc/ssl/certs/ca-certificates.crt", DsqlAuthPlugin::DefaultRootCert(150004,
        [](const char*) { return true; }));
}

#ifndef _WIN32
namespace {
    constexpr int FAKE_LIBPQ_VERSION = 150004;
    // What FakePQlibVersion returns, set by the loader that hands it out. One global for every loader: the tests run
    // one at a time, and each reads the version right after its loader set it.
    int g_fake_libpq_version = FAKE_LIBPQ_VERSION;
    int FakePQlibVersion() { return g_fake_libpq_version; }

    // A loader whose PQlibVersion returns the given version, or that has none.
    class FakeLibpqLoader : public RdsLibLoader {
    public:
        explicit FakeLibpqLoader(const std::optional<int> version) : RdsLibLoader(""), version_(version) {}

        RdsPlatform::FuncHandle GetFunction(const std::string& function_name) override {
            if (!version_ || function_name != "PQlibVersion") {
                return nullptr;
            }
            g_fake_libpq_version = *version_;
            return reinterpret_cast<RdsPlatform::FuncHandle>(&FakePQlibVersion);
        }

    private:
        std::optional<int> version_;
    };
}  // namespace

TEST_F(DsqlAuthPluginTest, BaseLibpqVersion_ReadsTheBaseDriversLibpq) {
    EXPECT_EQ(std::optional<int>(FAKE_LIBPQ_VERSION),
              DsqlPluginHelper::BaseLibpqVersion(std::make_shared<FakeLibpqLoader>(FAKE_LIBPQ_VERSION)));
    EXPECT_EQ(std::nullopt, DsqlPluginHelper::BaseLibpqVersion(std::make_shared<FakeLibpqLoader>(std::nullopt)));
    EXPECT_EQ(std::nullopt, DsqlPluginHelper::BaseLibpqVersion(nullptr));
}

// Connect picks the root certificate from the base driver's libpq version. The tests above run without an ENV, which
// reads as an unknown version, so only this one fails if Connect stops passing the choice on.
TEST_F(DsqlAuthPluginTest, Connect_RootCertFollowsTheBaseLibpqVersion) {
    for (const auto& [version, expect_system] : std::map<int, bool>{{150004, false}, {160004, true}}) {
        ENV env;
        env.driver_lib_loader = std::make_shared<FakeLibpqLoader>(version);
        dbc_->env = &env;
        dbc_->conn_attr.erase("PQOPT");
        EXPECT_CALL(*mock_auth_provider_, GetToken(testing::_, testing::_, testing::_, testing::_, testing::_,
                                                   testing::_, testing::_))
            .WillOnce(testing::Return(FRESH_TOKEN));
        std::string pqopt;
        EXPECT_CALL(*mock_base_plugin_, Connect(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
            .WillOnce(testing::Invoke([this, &pqopt](SQLHDBC, SQLHWND, SQLTCHAR*, SQLSMALLINT, SQLSMALLINT*,
                                                     SQLUSMALLINT) {
                pqopt = dbc_->conn_attr.at("PQOPT");
                return SQL_SUCCESS;
            }));

        EXPECT_EQ(SQL_SUCCESS, ConnectWithMockProvider()) << version;
        // Before libpq 16 this holds with a bundle or without one: "system" is never added.
        EXPECT_EQ(expect_system, pqopt.find("sslrootcert=system") != std::string::npos) << version << " " << pqopt;
        dbc_->env = nullptr;
    }
}

namespace {
    // A symbolic link to target in the temp directory, under a name no other one has (the process ID and a counter),
    // removed at the end of the scope.
    class TempLink {
    public:
        explicit TempLink(const std::string& target)
            : path_(std::filesystem::path(::testing::TempDir())
                    / ("dsql_test_link_" + std::to_string(getpid()) + "_" + std::to_string(next_id_++))) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);  // left by an earlier process with the same ID
            std::filesystem::create_symlink(target, path_);
        }
        ~TempLink() {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
        TempLink(const TempLink&) = delete;
        TempLink& operator=(const TempLink&) = delete;

        [[nodiscard]] const char* Path() const {
            return path_.c_str();
        }

    private:
        static inline unsigned next_id_ = 0;
        std::filesystem::path path_;
    };
}  // namespace

// Only a regular file that only root can change, found through symbolic links: /etc/passwd and a link to it, not the
// /etc directory, a file the test made, a link to that one, or a path that doesn't exist.
TEST_F(DsqlAuthPluginTest, IsRootOwnedFile_RootOwnedRegularFilesOnly) {
    EXPECT_TRUE(DsqlAuthPlugin::IsRootOwnedFile("/etc/passwd"));
    const TempLink link_to_root_file("/etc/passwd");
    EXPECT_TRUE(DsqlAuthPlugin::IsRootOwnedFile(link_to_root_file.Path()));
    EXPECT_FALSE(DsqlAuthPlugin::IsRootOwnedFile("/etc"));
    EXPECT_FALSE(DsqlAuthPlugin::IsRootOwnedFile("/nonexistent/dsql-ca-bundle.crt"));

    if (geteuid() == 0) {
        GTEST_SKIP() << "files made by root are root-owned";
    }
    const TempConfigFile own_file("x");
    const TempLink link_to_own_file(own_file.Path());
    EXPECT_FALSE(DsqlAuthPlugin::IsRootOwnedFile(own_file.Path().c_str()));
    EXPECT_FALSE(DsqlAuthPlugin::IsRootOwnedFile(link_to_own_file.Path()));
    EXPECT_FALSE(DsqlAuthPlugin::IsRootOwnedFile(::testing::TempDir().c_str()));
}
#endif

TEST_F(DsqlAuthPluginTest, RootCertHint_BundleRootCert) {
    std::map<std::string, std::string> conn_attr;
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr, "/etc/pki/tls/certs/ca-bundle.crt");
    const std::string_view hint = DsqlAuthPlugin::GetRootCertHint(conn_attr, "SSL error: certificate verify failed");
    EXPECT_NE(std::string::npos, hint.find("system CA bundle"));
    EXPECT_NE(std::string::npos, hint.find("pqopt={sslrootcert="));
}

TEST_F(DsqlAuthPluginTest, RootCertHint_OldLibpqWithoutRootCert) {
    std::map<std::string, std::string> conn_attr;
    DsqlAuthPlugin::ApplyConnectionDefaults(conn_attr, "");
    const std::string_view hint = DsqlAuthPlugin::GetRootCertHint(conn_attr,
        "root certificate file \"/home/u/.postgresql/root.crt\" does not exist");
    EXPECT_NE(std::string::npos, hint.find("no system CA bundle was found"));
    EXPECT_NE(std::string::npos, hint.find("pqopt={sslrootcert="));
    // A user's own certificate, or a mode that needs none, gets no hint.
    std::map<std::string, std::string> own_cert = {{"PQOPT", "{sslrootcert=/etc/ssl/corp-ca.pem}"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(own_cert, "");
    EXPECT_EQ("", DsqlAuthPlugin::GetRootCertHint(own_cert, "certificate verify failed"));
    std::map<std::string, std::string> verify_ca = {{"SSLMODE", "verify-ca"}};
    DsqlAuthPlugin::ApplyConnectionDefaults(verify_ca, "");
    EXPECT_EQ("", DsqlAuthPlugin::GetRootCertHint(verify_ca, "certificate verify failed"));
}
