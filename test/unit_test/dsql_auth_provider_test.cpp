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

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/auth/CrtCredentialsProvider.h>
#include <aws/core/auth/ProfileCredentialsProvider.h>
#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/auth/STSCredentialsProvider.h>
#include <aws/core/utils/DateTime.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

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
    const std::chrono::milliseconds EXPIRATION = std::chrono::seconds(600);
    const std::string RESOURCES_DIR = AWS_PROFILE_TEST_RESOURCES_DIR;
    // AWS documentation example keys. The aws_credentials fixture's [dsql_static] has AKIDEXAMPLE.
    const std::string SECRET_KEY = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    // Fake keys in the environment.
    const std::string ENV_ACCESS_KEY = "AKIDENVEXAMPLE";

    // Profiles come only from the fixture files, the environment has keys, which the default chain answers with
    // first, and no container, EKS web identity or IMDS source.
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

    // The profiles of the aws_config_dsql_profiles fixture as the config file.
    ScopedEnvVars DsqlProfiles() {
        return ProfileFilesAndEnvironmentKeys(RESOURCES_DIR + "/aws_config_dsql_profiles");
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

    // Returns the given credentials, one per read, and the last one after that.
    class SequenceCredentialsProvider final : public Aws::Auth::AWSCredentialsProvider {
    public:
        explicit SequenceCredentialsProvider(std::vector<Aws::Auth::AWSCredentials> credentials)
            : credentials_(std::move(credentials)) {}
        Aws::Auth::AWSCredentials GetAWSCredentials() override {
            return credentials_[std::min(reads_++, credentials_.size() - 1)];
        }

    private:
        std::vector<Aws::Auth::AWSCredentials> credentials_;
        size_t reads_ = 0;
    };
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

    static Aws::Auth::AWSCredentials TestCredentials() {
        return {"AKIDEXAMPLE", SECRET_KEY};
    }
};

TEST_F(DsqlAuthProviderTest, GetToken_AdminUser_UsesAdminAction) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::pair<std::string, bool> token = provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION);
    EXPECT_FALSE(token.second);
    EXPECT_TRUE(token.first.starts_with(HOST + "?Action=DbConnectAdmin&"));
    EXPECT_NE(std::string::npos, token.first.find("X-Amz-Expires=600"));
}

TEST_F(DsqlAuthProviderTest, GetToken_NonAdminUser_UsesConnectAction) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::pair<std::string, bool> token = provider.GetToken(HOST, "eu-west-1", PORT, "app_user", true, false, EXPIRATION);
    EXPECT_TRUE(token.first.starts_with(HOST + "?Action=DbConnect&"));
    EXPECT_NE(std::string::npos, token.first.find("%2Feu-west-1%2Fdsql%2Faws4_request"));
}

TEST_F(DsqlAuthProviderTest, GetToken_ExtraUrlEncode) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::string plain = provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first;
    const std::string encoded = provider.GetToken(HOST, REGION, PORT, "admin", true, true, EXPIRATION).first;
    // X-Amz-Credential holds "/" separators, which presigning encodes as %2F.
    EXPECT_NE(std::string::npos, plain.find("%2F"));
    EXPECT_NE(std::string::npos, encoded.find("%252F"));
}

// DNS is case-insensitive but the signature covers the host, so always sign the lowercase form.
TEST_F(DsqlAuthProviderTest, GetToken_UppercaseHost_SignsLowercase) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::string token =
        provider.GetToken("ABCDEFGHIJKLMNOPQRSTUVWXYZ.DSQL.US-EAST-1.ON.AWS", REGION, PORT, "admin", true, false, EXPIRATION).first;
    EXPECT_TRUE(token.starts_with(HOST));
}

// TOKEN_EXPIRATION=0 means the default lifetime: X-Amz-Expires=0 would make a token that has already expired.
TEST_F(DsqlAuthProviderTest, GetToken_ZeroExpiration_UsesDefault) {
    DsqlAuthProvider provider(REGION, TestCredentials());
    const std::string token =
        provider.GetToken(HOST, REGION, PORT, "admin", true, false, std::chrono::milliseconds(0)).first;
    EXPECT_NE(std::string::npos, token.find("X-Amz-Expires=900"));
}

// Expired credentials aren't empty, but the server rejects what they sign. This is what an SSO-backed provider returns
// after the session expires.
TEST_F(DsqlAuthProviderTest, GetToken_ExpiredCredentials_ReturnsEmpty) {
    const Aws::Auth::AWSCredentials expired("AKIDEXAMPLE", SECRET_KEY, "",
        Aws::Utils::DateTime::Now() - std::chrono::hours(1));
    DsqlAuthProvider provider(REGION, expired);
    EXPECT_FALSE(provider.HasResolvedCredentials());
    EXPECT_EQ("", provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first);
}

// The SigV4 presigner returns an unsigned URL when either key is empty, not only when both are.
TEST_F(DsqlAuthProviderTest, GetToken_MissingSecretKey_ReturnsEmpty) {
    DsqlAuthProvider provider(REGION, Aws::Auth::AWSCredentials("AKIDEXAMPLE", ""));
    EXPECT_FALSE(provider.HasResolvedCredentials());
    EXPECT_EQ("", provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first);
}

// The token is signed with the credentials the connect's check read, and those sign only that token.
TEST_F(DsqlAuthProviderTest, CheckedCredentials_AreUsedByOneToken) {
    DsqlAuthProvider provider(REGION, std::make_shared<SequenceCredentialsProvider>(std::vector{
        Aws::Auth::AWSCredentials("AKIDKEYA", SECRET_KEY), Aws::Auth::AWSCredentials("AKIDKEYB", SECRET_KEY)}));
    ASSERT_TRUE(provider.HasResolvedCredentials());
    EXPECT_TRUE(SignsWith(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first, "AKIDKEYA"));
    EXPECT_TRUE(SignsWith(provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first, "AKIDKEYB"));
}

// With the AWS_PROFILE key only that profile signs, though the environment has keys.
TEST_F(DsqlAuthProviderTest, ProfileKey_IgnoresEnvironmentKeys) {
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys();
    DsqlAuthProvider provider(REGION, "dsql_static", DsqlAuthProvider::ProfileSource::KEY);
    const std::string token = provider.GetToken(HOST, REGION, PORT, "admin", true, false, EXPIRATION).first;
    EXPECT_TRUE(SignsWith(token, "AKIDEXAMPLE"));
}

// The key's chain has only the profile's own sources: the profile and SSO. sso_p has no login_session, so there is no
// login provider, though [default] has one.
TEST_F(DsqlAuthProviderTest, ProfileKey_ChainHasOnlyProfileProviders) {
    const ScopedEnvVars env = DsqlProfiles();
    const std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> chain =
        DsqlAuthProvider::MakeProfileOnlyChain("sso_p", REGION);
    ASSERT_NE(nullptr, chain);
    EXPECT_TRUE(HasProvider<Aws::Auth::ProfileCredentialsProvider>(*chain));
    EXPECT_TRUE(HasProvider<Aws::Auth::SSOCredentialsProvider>(*chain));
    EXPECT_EQ(2U, chain->GetProviders().size());
}

// role_arn with web_identity_token_file and no source_profile or credential_source: the CRT profile provider can't
// read it, so the profile gets its own web identity provider.
TEST_F(DsqlAuthProviderTest, ProfileKey_WebIdentityProfile_GetsItsOwnProvider) {
    const ScopedEnvVars env = DsqlProfiles();
    const std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> chain =
        DsqlAuthProvider::MakeProfileOnlyChain("wi", REGION);
    ASSERT_NE(nullptr, chain);
    EXPECT_TRUE(HasProvider<Aws::Auth::STSAssumeRoleWebIdentityCredentialsProvider>(*chain));
}

// An `aws login` profile (login_session) gets a login provider, built from the CRT's for that profile.
TEST_F(DsqlAuthProviderTest, ProfileKey_LoginProfile_GetsTheLoginProvider) {
    const ScopedEnvVars env = DsqlProfiles();
    const std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> chain =
        DsqlAuthProvider::MakeProfileOnlyChain("login_p", REGION);
    ASSERT_NE(nullptr, chain);
    EXPECT_TRUE(HasExactProvider<Aws::Auth::CrtCredentialsProvider>(*chain));
}

// The CRT login provider reads the key's profile: a profile without a login_session gets nothing, though [default]
// and the AWS_PROFILE variable's profile have one. The CRT rejects a profile without a session when it builds the
// provider, so a null provider shows which profile it read. Building the provider doesn't connect.
TEST_F(DsqlAuthProviderTest, LoginCrtProvider_ReadsOnlyTheKeyProfile) {
    const ScopedEnvVars env = DsqlProfiles();
    EXPECT_NE(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("login_p", REGION));
    EXPECT_EQ(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("plain", REGION));
    const ScopedEnvVars env_profile("AWS_PROFILE", "env_p");
    EXPECT_EQ(nullptr, DsqlAuthProvider::MakeLoginCrtProvider("plain", REGION));
}

// The config file names profiles "[profile p]", the credentials file "[p]".
TEST_F(DsqlAuthProviderTest, ProfileExists_ReadsBothFiles) {
    const ScopedEnvVars env = ProfileFilesAndEnvironmentKeys();
    EXPECT_TRUE(DsqlAuthProvider::ProfileExists("dev"));          // aws_config
    EXPECT_TRUE(DsqlAuthProvider::ProfileExists("dsql_static"));  // aws_credentials
    EXPECT_FALSE(DsqlAuthProvider::ProfileExists("nope"));
}
