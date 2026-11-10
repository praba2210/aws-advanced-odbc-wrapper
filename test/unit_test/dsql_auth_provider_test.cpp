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

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/auth/CrtCredentialsProvider.h>
#include <aws/core/auth/GeneralHTTPCredentialsProvider.h>
#include <aws/core/auth/LoginCredentialsProvider.h>
#include <aws/core/auth/ProfileCredentialsProvider.h>
#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/auth/STSCredentialsProvider.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/core/utils/DateTime.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include "auth_mock_objects.h"
#include "scoped_ctype_locale.h"
#include "scoped_env_vars.h"

#include "../../driver/util/aws_sdk_helper.h"
#include "../../driver/util/dsql_auth_provider.h"

#ifndef AWS_PROFILE_TEST_RESOURCES_DIR
    #error "AWS_PROFILE_TEST_RESOURCES_DIR must be defined by the build"
#endif

namespace {
    const std::string HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string REGION = "us-east-1";
    const std::string PORT = "5432";
    const std::chrono::milliseconds EXPIRATION = std::chrono::seconds(900);
    const std::string RESOURCES_DIR = AWS_PROFILE_TEST_RESOURCES_DIR;
    // Fake keys in the environment. The aws_credentials fixture's [dsql_static] has AKIDEXAMPLE.
    const std::string ENV_ACCESS_KEY = "AKIDENVEXAMPLE";

    // Profiles come only from the fixture files (or the config file a test writes), the environment has keys, which
    // the default chain answers with first, and no container, EKS web identity or IMDS source.
    ScopedEnvVars ProfileFilesAndEnvironmentKeys(const std::string& config_file = RESOURCES_DIR + "/aws_config") {
        return ScopedEnvVars({
            {"AWS_CONFIG_FILE", config_file},
            {"AWS_SHARED_CREDENTIALS_FILE", RESOURCES_DIR + "/aws_credentials"},
            {"AWS_PROFILE", std::nullopt},
            {"AWS_DEFAULT_PROFILE", std::nullopt},
            {"AWS_ACCESS_KEY_ID", ENV_ACCESS_KEY},
            {"AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYENVEXAMPLE"},
            {"AWS_SESSION_TOKEN", std::nullopt},
            {"AWS_CONTAINER_CREDENTIALS_RELATIVE_URI", std::nullopt},
            {"AWS_CONTAINER_CREDENTIALS_FULL_URI", std::nullopt},
            {"AWS_ROLE_ARN", std::nullopt},
            {"AWS_WEB_IDENTITY_TOKEN_FILE", std::nullopt},
            {"AWS_EC2_METADATA_DISABLED", "true"},
        });
    }

    // Whether the chain has a provider of type T. Only the composition is checked: the providers are never asked for
    // credentials, which could reach SSO, STS or the sign-in service.
    template <typename T>
    bool HasProvider(const Aws::Auth::AWSCredentialsProviderChain& chain) {
        return std::ranges::any_of(chain.GetProviders(), [](const auto& provider) {
            return dynamic_cast<const T*>(provider.get()) != nullptr;
        });
    }

    // Whether the chain has a provider of exactly type T, not of a subclass.
    template <typename T>
    bool HasExactProvider(const Aws::Auth::AWSCredentialsProviderChain& chain) {
        return std::ranges::any_of(chain.GetProviders(), [](const auto& provider) {
            const Aws::Auth::AWSCredentialsProvider& object = *provider;
            return typeid(object) == typeid(T);
        });
    }

    bool SignsWith(const std::string& token, const std::string& access_key) {
        return token.find("X-Amz-Credential=" + access_key + "%2F") != std::string::npos;
    }

    const std::string SECRET_KEY = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
}  // namespace

// DSQL tokens are presigned locally, so these tests run the real SDK with fake keys and never reach AWS.
class DsqlAuthProviderTest : public testing::Test {
protected:
    static void SetUpTestSuite() {
        AwsSdkHelper::Init();
    }
    static void TearDownTestSuite() {
        AwsSdkHelper::Shutdown();
    }

    // AWS documentation example keys.
    static Aws::Auth::AWSCredentials TestCredentials() {
        return {"AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"};
    }
};

TEST_F(DsqlAuthProviderTest, GetToken_AdminUser_UsesAdminAction) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::pair<std::string, bool> token = provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION);
    EXPECT_FALSE(token.second);
    EXPECT_TRUE(token.first.starts_with(HOST));
    EXPECT_NE(std::string::npos, token.first.find("Action=DbConnectAdmin"));
    EXPECT_NE(std::string::npos, token.first.find("X-Amz-Expires=900"));
}

TEST_F(DsqlAuthProviderTest, GetToken_NonAdminUser_UsesConnectAction) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::pair<std::string, bool> token = provider.GetToken(HOST, REGION, PORT, "app_user", true, false, EXPIRATION);
    EXPECT_NE(std::string::npos, token.first.find("Action=DbConnect"));
    EXPECT_EQ(std::string::npos, token.first.find("DbConnectAdmin"));
}

// Same rule as all 9 DSQL connectors: only the exact name "admin" maps to the admin role.
TEST_F(DsqlAuthProviderTest, GetToken_AdminRuleIsCaseSensitive) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::pair<std::string, bool> token = provider.GetToken(HOST, REGION, PORT, "Admin", true, false, EXPIRATION);
    EXPECT_NE(std::string::npos, token.first.find("Action=DbConnect"));
    EXPECT_EQ(std::string::npos, token.first.find("DbConnectAdmin"));
}

TEST_F(DsqlAuthProviderTest, GetToken_PassesExpiration) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::pair<std::string, bool> token =
        provider.GetToken(HOST, REGION, PORT, "admin", true, false, std::chrono::seconds(300));
    EXPECT_NE(std::string::npos, token.first.find("X-Amz-Expires=300"));
}

TEST_F(DsqlAuthProviderTest, GetToken_ExtraUrlEncode) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::string plain = provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first;
    const std::string encoded = provider.GetToken(HOST, REGION, PORT, "admin", true, true, EXPIRATION).first;
    // X-Amz-Credential holds "/" separators, which presigning encodes as %2F.
    EXPECT_NE(std::string::npos, plain.find("%2F"));
    EXPECT_NE(std::string::npos, encoded.find("%252F"));
}

TEST_F(DsqlAuthProviderTest, GetToken_NeverReportsCached) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    EXPECT_FALSE(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).second);
    EXPECT_FALSE(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).second);
}

TEST_F(DsqlAuthProviderTest, GetToken_EmptyHost_ReturnsEmptyToken) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    EXPECT_EQ("", provider.GetToken("", REGION, PORT, "admin", true, false, EXPIRATION).first);
}

TEST_F(DsqlAuthProviderTest, EmptyCredentials_NotResolved) {
    DsqlAuthProvider provider(REGION, Aws::Auth::AWSCredentials());
    EXPECT_FALSE(provider.HasResolvedCredentials());
}

// DNS is case-insensitive but the signature covers the host, so always sign the lowercase form.
TEST_F(DsqlAuthProviderTest, GetToken_UppercaseHost_SignsLowercase) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::string token =
        provider.GetToken("ABCDEFGHIJKLMNOPQRSTUVWXYZ.DSQL.US-EAST-1.ON.AWS", REGION, PORT, "admin", true, false, EXPIRATION).first;
    EXPECT_TRUE(token.starts_with(HOST));
}

// Under tr_TR.UTF-8, glibc's std::tolower leaves I as I, so the host is folded in ASCII. Needs the tr_TR.UTF-8 locale.
TEST_F(DsqlAuthProviderTest, GetToken_TurkishCtype_SignsLowercaseHost) {
    const ScopedCtypeLocale ctype("tr_TR.UTF-8");
    if (!ctype.IsSet()) {
        SKIP_OR_FAIL_WITHOUT_TURKISH_LOCALE();
    }
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::string token =
        provider.GetToken("ABCDEFGHIJKLMNOPQRSTUVWXYZ.DSQL.US-EAST-1.ON.AWS", REGION, PORT, "admin", true, false, EXPIRATION).first;
    // Only the host part of the token is printed.
    EXPECT_TRUE(token.starts_with(HOST)) << token.substr(0, HOST.size());
}

// With empty credentials the C++ SigV4 presigner skips signing ("don't sign anonymous requests") and returns an
// unsigned URL, which the server would reject as a bad password. The provider must refuse instead.
TEST_F(DsqlAuthProviderTest, GetToken_NoCredentials_ReturnsEmpty) {
    DsqlAuthProvider provider(REGION, Aws::Auth::AWSCredentials());
    EXPECT_EQ("", provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first);
}

// A failed signing returns an empty token and doesn't affect the next call.
TEST_F(DsqlAuthProviderTest, GetToken_SigningFails_ReturnsEmpty) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    EXPECT_EQ("", provider.GetToken("", REGION, PORT, "admin", true, false, EXPIRATION).first);
    EXPECT_FALSE(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first.empty());
}

// Expired credentials aren't empty, so IsEmpty() alone lets the SDK sign a token the server rejects. This is what an
// SSO-backed provider returns after the session expires.
TEST_F(DsqlAuthProviderTest, GetToken_ExpiredCredentials_ReturnsEmpty) {
    const Aws::Auth::AWSCredentials expired("AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "",
        Aws::Utils::DateTime::Now() - std::chrono::hours(1));
    DsqlAuthProvider provider(REGION, expired);
    EXPECT_FALSE(provider.HasResolvedCredentials());
    EXPECT_EQ("", provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first);
}

// The SigV4 presigner skips signing when either key is empty, not only when both are.
TEST_F(DsqlAuthProviderTest, GetToken_MissingSecretKey_ReturnsEmpty) {
    DsqlAuthProvider provider(REGION, Aws::Auth::AWSCredentials("AKIDEXAMPLE", ""));
    EXPECT_FALSE(provider.HasResolvedCredentials());
    EXPECT_EQ("", provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first);
}

// Profile keys come through the CRT-backed ProfileCredentialsProvider, which reports "never expires" as UINT64_MAX
// seconds. The SDK's DateTime overflows on that value, so the expiration reads as at or before 1970. Those
// credentials are valid and must still sign.
TEST_F(DsqlAuthProviderTest, ProfileStaticCredentials_AreResolved) {
    const ScopedEnvVars credentials_file("AWS_SHARED_CREDENTIALS_FILE",
        std::string(AWS_PROFILE_TEST_RESOURCES_DIR) + "/aws_credentials");
    // CI exports role credentials; the environment provider would answer before the profile one.
    const ScopedEnvVars access_key("AWS_ACCESS_KEY_ID", std::nullopt);
    const ScopedEnvVars secret_key("AWS_SECRET_ACCESS_KEY", std::nullopt);
    const ScopedEnvVars session_token("AWS_SESSION_TOKEN", std::nullopt);

    DsqlAuthProvider provider(REGION, "dsql_static");
    EXPECT_TRUE(provider.HasResolvedCredentials());
    EXPECT_FALSE(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first.empty());
}

// With the AWS_PROFILE key only that profile signs, though the environment has keys.
TEST_F(DsqlAuthProviderTest, ProfileKey_IgnoresEnvironmentKeys) {
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys();
    DsqlAuthProvider provider(REGION, "dsql_static", DsqlAuthProvider::ProfileSource::KEY);
    const std::string token = provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first;
    EXPECT_TRUE(SignsWith(token, "AKIDEXAMPLE"));
    EXPECT_FALSE(SignsWith(token, ENV_ACCESS_KEY));
}

// A key profile that is in neither file resolves nothing: no fallback to the environment's keys.
TEST_F(DsqlAuthProviderTest, ProfileKey_MissingProfile_ResolvesNothing) {
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys();
    DsqlAuthProvider provider(REGION, "no_such_profile", DsqlAuthProvider::ProfileSource::KEY);
    EXPECT_FALSE(provider.HasResolvedCredentials());
    EXPECT_EQ("", provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first);
}

// The key's chain has only the profile's own sources: no environment keys, no EKS (IRSA) web identity from the
// environment, no container endpoint and no instance role, though the environment names them. sso_p has no
// login_session, so there is no login provider either, though [default] has one.
TEST_F(DsqlAuthProviderTest, ProfileKey_ChainHasOnlyProfileProviders) {
    const TempConfigFile config(
        "[profile sso_p]\nsso_session = s\nsso_account_id = 111122223333\nsso_role_name = ReadOnly\n\n"
        "[sso-session s]\nsso_start_url = https://example.awsapps.com/start\nsso_region = us-east-1\n\n"
        "[default]\nlogin_session = arn:aws:iam::111122223333:user/default\nregion = us-east-1\n");
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys(config.Path());
    const ScopedEnvVars irsa_and_container({
        {"AWS_ROLE_ARN", "arn:aws:iam::111122223333:role/pod-role"},
        {"AWS_WEB_IDENTITY_TOKEN_FILE", "no_such_web_identity_token"},
        {"AWS_CONTAINER_CREDENTIALS_FULL_URI", "http://127.0.0.1:1/credentials"},
    });

    const std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> chain =
        DsqlAuthProvider::MakeProfileOnlyChain("sso_p", REGION);
    ASSERT_NE(nullptr, chain);
    EXPECT_TRUE(HasProvider<Aws::Auth::ProfileCredentialsProvider>(*chain));
    EXPECT_TRUE(HasProvider<Aws::Auth::SSOCredentialsProvider>(*chain));
    EXPECT_FALSE(HasProvider<Aws::Auth::EnvironmentAWSCredentialsProvider>(*chain));
    EXPECT_FALSE(HasProvider<Aws::Auth::STSAssumeRoleWebIdentityCredentialsProvider>(*chain));
    EXPECT_FALSE(HasProvider<Aws::Auth::InstanceProfileCredentialsProvider>(*chain));
    EXPECT_FALSE(HasProvider<Aws::Auth::GeneralHTTPCredentialsProvider>(*chain));
    EXPECT_FALSE(HasProvider<Aws::Auth::LoginCredentialsProvider>(*chain));
    EXPECT_EQ(2U, chain->GetProviders().size());  // profile and SSO
}

// role_arn with web_identity_token_file and no source_profile or credential_source: the CRT profile provider can't
// read it, so the profile gets its own web identity provider.
TEST_F(DsqlAuthProviderTest, ProfileKey_WebIdentityProfile_GetsItsOwnProvider) {
    const TempConfigFile config("[profile wi]\nrole_arn = arn:aws:iam::111122223333:role/wi\n"
                                "web_identity_token_file = no_such_web_identity_token\n");
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys(config.Path());

    const std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> chain =
        DsqlAuthProvider::MakeProfileOnlyChain("wi", REGION);
    ASSERT_NE(nullptr, chain);
    EXPECT_TRUE(HasProvider<Aws::Auth::STSAssumeRoleWebIdentityCredentialsProvider>(*chain));
    EXPECT_FALSE(HasProvider<Aws::Auth::EnvironmentAWSCredentialsProvider>(*chain));
}

// An `aws login` profile (login_session) gets a login provider, built from the CRT's for that profile, though there is
// no [default]. Not the SDK's LoginCredentialsProvider: it lets the CRT read the AWS_PROFILE variable's profile or
// [default] instead.
TEST_F(DsqlAuthProviderTest, ProfileKey_LoginProfile_GetsTheLoginProvider) {
    const TempConfigFile config("[profile login_p]\nlogin_session = arn:aws:iam::111122223333:user/dev\n"
                                "region = us-west-2\n");
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys(config.Path());

    const std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> chain =
        DsqlAuthProvider::MakeProfileOnlyChain("login_p", REGION);
    ASSERT_NE(nullptr, chain);
    EXPECT_TRUE(HasExactProvider<Aws::Auth::CrtCredentialsProvider>(*chain));
    EXPECT_FALSE(HasProvider<Aws::Auth::LoginCredentialsProvider>(*chain));
    EXPECT_FALSE(HasProvider<Aws::Auth::EnvironmentAWSCredentialsProvider>(*chain));
}

// The CRT login provider reads the key's profile (the one it also takes endpoint settings from): that profile's
// session counts though [default] is missing, and a profile without one gets nothing, though [default] or the
// AWS_PROFILE variable's profile has one. The CRT rejects a profile without a session when it builds the provider, so
// a null provider shows which profile it read. Nothing is signed in: building the provider doesn't connect.
TEST_F(DsqlAuthProviderTest, LoginCrtProvider_ReadsOnlyTheKeyProfile) {
    {
        const TempConfigFile config("[profile login_p]\nlogin_session = arn:aws:iam::111122223333:user/login_p\n"
                                    "region = us-west-2\n");
        const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys(config.Path());
        EXPECT_NE(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("login_p", REGION));
    }
    const TempConfigFile config("[default]\nlogin_session = arn:aws:iam::111122223333:user/default\n"
                                "region = us-east-1\n\n"
                                "[profile env_p]\nlogin_session = arn:aws:iam::111122223333:user/env_p\n"
                                "region = us-east-1\n\n"
                                "[profile plain]\nregion = us-east-1\n");
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys(config.Path());
    EXPECT_NE(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("default", REGION));
    EXPECT_EQ(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("plain", REGION));
    const ScopedEnvVars env_profile("AWS_PROFILE", "env_p");
    EXPECT_NE(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("env_p", REGION));
    EXPECT_EQ(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("plain", REGION));
    EXPECT_EQ(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("no_such_profile", REGION));
    EXPECT_EQ(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("", REGION));
}

// With only the AWS_PROFILE variable (the plugin passes "" or the trimmed name) the SDK's default chain runs, as
// documented: the environment's keys come before the profile.
TEST_F(DsqlAuthProviderTest, EnvProfileOnly_KeepsTheDefaultChain) {
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys();
    const ScopedEnvVars env_profile("AWS_PROFILE", "dsql_static");
    for (const char* profile : {"", "dsql_static"}) {
        DsqlAuthProvider provider(REGION, profile);
        const std::string token = provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first;
        EXPECT_TRUE(SignsWith(token, ENV_ACCESS_KEY)) << profile;
    }
}

// The config file names profiles "[profile p]" (and "[default]"), the credentials file "[p]".
TEST_F(DsqlAuthProviderTest, ProfileExists_ReadsBothFiles) {
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys();
    EXPECT_TRUE(DsqlAuthProvider::ProfileExists("dev"));          // aws_config
    EXPECT_TRUE(DsqlAuthProvider::ProfileExists("dsql_static"));  // aws_credentials
    EXPECT_FALSE(DsqlAuthProvider::ProfileExists("nope"));
    EXPECT_FALSE(DsqlAuthProvider::ProfileExists("default"));

    const ScopedEnvVars config_with_default("AWS_CONFIG_FILE", RESOURCES_DIR + "/aws_config_with_default");
    EXPECT_TRUE(DsqlAuthProvider::ProfileExists("default"));
}

// AuthProvider has subclasses (DsqlAuthProvider, the test mocks) and is held through base pointers. make_shared's
// control block destroys the right type anyway; a unique_ptr<AuthProvider> or a delete through the base would not.
TEST_F(DsqlAuthProviderTest, AuthProvider_HasVirtualDestructor) {
    EXPECT_TRUE(std::has_virtual_destructor_v<AuthProvider>);
}

// The token is signed for the region GetToken is given. Off EC2, the client's own IMDS region lookup only waits for
// its timeouts on every new provider.
TEST_F(DsqlAuthProviderTest, ClientConfiguration_DisablesImds) {
    const Aws::Client::GenericClientConfiguration config = DsqlAuthProvider::MakeClientConfiguration("eu-west-1");
    EXPECT_TRUE(config.disableIMDS);
    EXPECT_EQ("eu-west-1", config.region);
}

// TOKEN_EXPIRATION=0 means the default lifetime, as in the Go connector: X-Amz-Expires=0 would make a token that has
// already expired. Negative values can only come from a direct caller; the connection string validator rejects them.
TEST_F(DsqlAuthProviderTest, GetToken_NonPositiveExpiration_UsesDefault) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    for (const std::chrono::milliseconds expiration : {std::chrono::milliseconds(0), std::chrono::milliseconds(-5000)}) {
        const std::string token = provider.GetToken(HOST, REGION, PORT, "admin", true, false, expiration).first;
        EXPECT_NE(std::string::npos, token.find("X-Amz-Expires=900")) << expiration.count();
    }
}

// A connect checks the credentials, then signs. That is one read of the source, not a read per step.
TEST_F(DsqlAuthProviderTest, OneCredentialsReadPerConnect) {
    const auto source = std::make_shared<CountingCredentialsProvider>(
        std::vector{Aws::Auth::AWSCredentials("AKIDEXAMPLE", SECRET_KEY)});
    DsqlAuthProvider provider(REGION, source);
    EXPECT_EQ(0, source->Reads());

    EXPECT_TRUE(provider.HasResolvedCredentials());
    EXPECT_TRUE(SignsWith(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first, "AKIDEXAMPLE"));
    EXPECT_EQ(1, source->Reads());

    // Without a check first, GetToken reads once itself. The check's read was used up by the token before.
    EXPECT_TRUE(SignsWith(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first, "AKIDEXAMPLE"));
    EXPECT_EQ(2, source->Reads());
}

// The token is signed with the credentials the check read, not with a second read that may differ.
TEST_F(DsqlAuthProviderTest, SignsWithTheCheckedCredentials) {
    const auto source = std::make_shared<CountingCredentialsProvider>(std::vector{
        Aws::Auth::AWSCredentials("AKIDKEYA", SECRET_KEY), Aws::Auth::AWSCredentials("AKIDKEYB", SECRET_KEY),});
    DsqlAuthProvider provider(REGION, source);
    ASSERT_TRUE(provider.HasResolvedCredentials());
    const std::string token = provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first;
    EXPECT_TRUE(SignsWith(token, "AKIDKEYA"));
    EXPECT_FALSE(SignsWith(token, "AKIDKEYB"));
}

// A check that failed keeps nothing: the next token reads the source again and signs with what it finds.
TEST_F(DsqlAuthProviderTest, FailedCheck_DoesNotLeaveCredentialsForTheNextToken) {
    const Aws::Auth::AWSCredentials expired("AKIDOLD", SECRET_KEY, "", Aws::Utils::DateTime::Now() - std::chrono::hours(1));
    const auto source = std::make_shared<CountingCredentialsProvider>(
        std::vector{expired, Aws::Auth::AWSCredentials("AKIDNEW", SECRET_KEY)});
    DsqlAuthProvider provider(REGION, source);
    EXPECT_FALSE(provider.HasResolvedCredentials());
    EXPECT_EQ(1, source->Reads());

    const std::string token = provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first;
    EXPECT_EQ(2, source->Reads());
    EXPECT_TRUE(SignsWith(token, "AKIDNEW"));
}

// A snapshot signs one token. A later connect that skips the check does not sign with an earlier connect's credentials.
TEST_F(DsqlAuthProviderTest, CheckedCredentials_AreUsedByOneToken) {
    const auto source = std::make_shared<CountingCredentialsProvider>(std::vector{
        Aws::Auth::AWSCredentials("AKIDKEYA", SECRET_KEY), Aws::Auth::AWSCredentials("AKIDKEYB", SECRET_KEY),});
    DsqlAuthProvider provider(REGION, source);
    ASSERT_TRUE(provider.HasResolvedCredentials());
    EXPECT_TRUE(SignsWith(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first, "AKIDKEYA"));
    EXPECT_TRUE(SignsWith(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first, "AKIDKEYB"));
}

// Each token reads its own credentials: once the source's have expired, the next token isn't signed, though the one
// before it was, with credentials that were valid then.
TEST_F(DsqlAuthProviderTest, GetToken_SourceExpiredAfterAToken_ReturnsEmpty) {
    const Aws::Auth::AWSCredentials expired("AKIDGONE", SECRET_KEY, "", Aws::Utils::DateTime::Now() - std::chrono::hours(1));
    const auto source = std::make_shared<CountingCredentialsProvider>(std::vector{
        Aws::Auth::AWSCredentials("AKIDKEYA", SECRET_KEY), expired,});
    DsqlAuthProvider provider(REGION, source);
    ASSERT_FALSE(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first.empty());
    EXPECT_EQ("", provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first);
}

#ifndef _WIN32
// A credential_process without Expiration runs on every read of its provider (the CRT's "never expires" reads as
// already expired), so each extra read is an extra run. Building the provider runs it no times, and a connect, the
// check and then the token, runs it once.
TEST_F(DsqlAuthProviderTest, CredentialProcess_RunsOncePerConnect) {
    const TempConfigFile runs("");
    const TempConfigFile credentials("[cp]\ncredential_process = echo run >> " + runs.Path()
        + " && echo '{\"Version\": 1, \"AccessKeyId\": \"AKIDPROCESS\", \"SecretAccessKey\": \"" + SECRET_KEY
        + "\"}'\n");
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys();
    const ScopedEnvVars credentials_file("AWS_SHARED_CREDENTIALS_FILE", credentials.Path());
    const auto run_count = [&runs] {
        std::ifstream log(runs.Path());
        return std::count(std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>(), '\n');
    };

    DsqlAuthProvider provider(REGION, "cp", DsqlAuthProvider::ProfileSource::KEY);
    EXPECT_EQ(0, run_count());
    for (const int connect : {1, 2}) {
        ASSERT_TRUE(provider.HasResolvedCredentials()) << connect;
        EXPECT_TRUE(SignsWith(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first,
                              "AKIDPROCESS")) << connect;
        EXPECT_EQ(connect, run_count());
    }
}
#endif
