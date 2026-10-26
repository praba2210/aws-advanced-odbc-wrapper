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

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <aws/core/config/ConfigAndCredentialsCacheManager.h>

#include "auth_mock_objects.h"

#include "../../driver/util/auth_provider.h"
#include "../../driver/util/aws_sdk_helper.h"

// AWS_PROFILE_TEST_RESOURCES_DIR is defined by CMake and points at the
// test/unit_test/resources directory, which holds the committed aws_config
#ifndef AWS_PROFILE_TEST_RESOURCES_DIR
    #error "AWS_PROFILE_TEST_RESOURCES_DIR must be defined by the build"
#endif

namespace {
    // Set environment var in a cross-platform way to point the SDK at the test config.
    void SetEnvVar(const char* name, const std::string& value) {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    void UnsetEnvVar(const char* name) {
#ifdef _WIN32
        _putenv_s(name, "");  // An empty value removes the variable on Windows.
#else
        unsetenv(name);
#endif
    }

    const std::string TEST_RESOURCE_DIR = AWS_PROFILE_TEST_RESOURCES_DIR;
    const std::string TEST_CONFIG_FILE = TEST_RESOURCE_DIR + "/aws_config";

    constexpr const char* DEFAULT_CHAIN_VARS[] = {
        "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN", "AWS_PROFILE", "AWS_DEFAULT_PROFILE",
        "AWS_WEB_IDENTITY_TOKEN_FILE", "AWS_ROLE_ARN", "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI",
        "AWS_CONTAINER_CREDENTIALS_FULL_URI", "AWS_SHARED_CREDENTIALS_FILE", "AWS_CONFIG_FILE",
        "AWS_EC2_METADATA_DISABLED"};

    // Leaves the default chain nothing to find while in scope: no credential variables, no credentials or config
    // file, and no IMDS. Restores every variable when the scope ends, unsetting the ones that weren't set.
    class NoDefaultChainCredentials {
    public:
        NoDefaultChainCredentials() {
            for (const char* name : DEFAULT_CHAIN_VARS) {
                const char* value = std::getenv(name);
                saved_.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
                UnsetEnvVar(name);
            }
            SetEnvVar("AWS_SHARED_CREDENTIALS_FILE", TEST_RESOURCE_DIR + "/does_not_exist");
            SetEnvVar("AWS_CONFIG_FILE", TEST_RESOURCE_DIR + "/does_not_exist");
            SetEnvVar("AWS_EC2_METADATA_DISABLED", "true");
            ReloadCachedFiles();
        }
        ~NoDefaultChainCredentials() {
            for (const auto& [name, value] : saved_) {
                if (value) {
                    SetEnvVar(name.c_str(), *value);
                } else {
                    UnsetEnvVar(name.c_str());
                }
            }
            ReloadCachedFiles();
        }
        NoDefaultChainCredentials(const NoDefaultChainCredentials&) = delete;
        NoDefaultChainCredentials& operator=(const NoDefaultChainCredentials&) = delete;

    private:
        // The SSO and web identity providers read the files cached when the SDK started, so a developer's own
        // ~/.aws/config could still supply credentials. Reload the cache from the files the variables now name.
        static void ReloadCachedFiles() {
            Aws::Config::ReloadCachedConfigFile();
            Aws::Config::ReloadCachedCredentialsFile();
        }

        std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
    };
}

class AuthProviderTest : public testing::Test {
protected:
    std::shared_ptr<MockRdsClient> mock_rds_client_;
    // Runs once per suite
    static void SetUpTestSuite() {
        AwsSdkHelper::Init();
    }
    static void TearDownTestSuite() {
        AwsSdkHelper::Shutdown();
    }

    void SetUp() override {
        mock_rds_client_ = std::make_shared<MockRdsClient>();
        AuthProvider::ClearCache();
        SetEnvVar("AWS_CONFIG_FILE", TEST_CONFIG_FILE);
    }
    void TearDown() override {
        if (mock_rds_client_) mock_rds_client_.reset();
    }
};

TEST_F(AuthProviderTest, AuthTypeFromString_Test) {
    EXPECT_EQ(AuthType::ADFS, AuthProvider::AuthTypeFromString("adfs"));
    EXPECT_EQ(AuthType::ADFS, AuthProvider::AuthTypeFromString("ADFS"));
    EXPECT_EQ(AuthType::IAM, AuthProvider::AuthTypeFromString("iam"));
    EXPECT_EQ(AuthType::IAM, AuthProvider::AuthTypeFromString("IAM"));
    EXPECT_EQ(AuthType::OKTA, AuthProvider::AuthTypeFromString("okta"));
    EXPECT_EQ(AuthType::OKTA, AuthProvider::AuthTypeFromString("OKTA"));
    EXPECT_EQ(AuthType::DATABASE, AuthProvider::AuthTypeFromString("database"));
    EXPECT_EQ(AuthType::DATABASE, AuthProvider::AuthTypeFromString("DATABASE"));
    EXPECT_EQ(AuthType::INVALID, AuthProvider::AuthTypeFromString("garbage"));
    EXPECT_EQ(AuthType::INVALID, AuthProvider::AuthTypeFromString("unknown"));
}

TEST_F(AuthProviderTest, GetRegionForProfile_EmptyProfile_ReturnsEmpty) {
    EXPECT_EQ("", AuthProvider::GetRegionForProfile(""));
}

TEST_F(AuthProviderTest, GetRegionForProfile_Resolved) {
    EXPECT_EQ("us-west-2", AuthProvider::GetRegionForProfile("dev"));
}

TEST_F(AuthProviderTest, GetRegionForProfile_NoRegion_ReturnsEmpty) {
    EXPECT_EQ("", AuthProvider::GetRegionForProfile("no_region"));
}

TEST_F(AuthProviderTest, GetRegionForProfile_NonexistentProfile_ReturnsEmpty) {
    EXPECT_EQ("", AuthProvider::GetRegionForProfile("does_not_exist"));
}

TEST_F(AuthProviderTest, BuildCacheKey_Test) {
    std::string server = "host.com", region = "us-west-1", port = "1234", username = "my-user",
        expected = "host.com-us-west-1-1234-my-user";
    std::string built_cache_key = AuthProvider::BuildCacheKey(server, region, port, username);
    EXPECT_EQ(expected, built_cache_key);
}

TEST_F(AuthProviderTest, GetToken_CacheMiss) {
    std::string server = "host.com", region = "us-west-1", port = "1234", username = "my-user";

    EXPECT_CALL(*mock_rds_client_, GenerateConnectAuthToken(testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return("token"));

    AuthProvider auth_provider(mock_rds_client_);
    std::pair<std::string, bool> token_info = auth_provider.GetToken(server, region, port, username);
    EXPECT_EQ("token", token_info.first);
    EXPECT_FALSE(token_info.second);
}

TEST_F(AuthProviderTest, GetToken_CacheHit) {
    std::string server = "host.com", region = "us-west-1", port = "1234", username = "my-user";

    EXPECT_CALL(*mock_rds_client_, GenerateConnectAuthToken(testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return("token"));

    AuthProvider auth_provider(mock_rds_client_);
    std::pair<std::string, bool> token_info = auth_provider.GetToken(server, region, port, username);
    EXPECT_FALSE(token_info.second);
    std::pair<std::string, bool> token_info_cached = auth_provider.GetToken(server, region, port, username);
    EXPECT_TRUE(token_info_cached.second);
    EXPECT_EQ(token_info_cached.first, token_info.first);
}

TEST_F(AuthProviderTest, GetToken_Expired) {
    std::string server = "host.com", region = "us-west-1", port = "1234", username = "my-user";

    EXPECT_CALL(*mock_rds_client_, GenerateConnectAuthToken(testing::_, testing::_, testing::_, testing::_))
        .WillOnce(testing::Return("token-1"))
        .WillOnce(testing::Return("token-2"));

    AuthProvider auth_provider(mock_rds_client_);
    std::pair<std::string, bool> token_info_first = auth_provider.GetToken(server, region, port, username, true, false, std::chrono::milliseconds(-99999));
    EXPECT_FALSE(token_info_first.second);
    std::pair<std::string, bool> token_info_second = auth_provider.GetToken(server, region, port, username);
    EXPECT_FALSE(token_info_second.second);

    EXPECT_NE(token_info_first.first, token_info_second.first);
}

// An empty token from the SDK must not be cached: the next connect asks the SDK again instead of failing for the
// token's whole lifetime.
TEST_F(AuthProviderTest, GetToken_EmptySdkToken_IsNotCached) {
    std::string server = "host.com", region = "us-west-1", port = "1234", username = "my-user";
    EXPECT_CALL(*mock_rds_client_, GenerateConnectAuthToken(testing::_, testing::_, testing::_, testing::_))
        .Times(testing::Exactly(2))
        .WillOnce(testing::Return(""))
        .WillOnce(testing::Return("token"));

    AuthProvider auth_provider(mock_rds_client_);
    EXPECT_EQ(std::make_pair(std::string(""), false), auth_provider.GetToken(server, region, port, username));
    EXPECT_EQ(std::make_pair(std::string("token"), false), auth_provider.GetToken(server, region, port, username));
}

// With nothing in the default chain the SDK signs nothing, and the server would reject the unsigned token as a bad
// password. The provider must say the credentials are missing instead.
TEST_F(AuthProviderTest, Constructor_DefaultChainWithoutCredentials_IsUnresolved) {
    const NoDefaultChainCredentials no_credentials;
    const AuthProvider auth_provider("us-west-1", "");
    EXPECT_FALSE(auth_provider.HasResolvedCredentials());
}

// The region-only constructor reads the same default chain.
TEST_F(AuthProviderTest, Constructor_RegionOnlyWithoutCredentials_IsUnresolved) {
    const NoDefaultChainCredentials no_credentials;
    const AuthProvider auth_provider("us-west-1");
    EXPECT_FALSE(auth_provider.HasResolvedCredentials());
}

// Credentials anywhere in the default chain, here the environment, still count as resolved.
TEST_F(AuthProviderTest, Constructor_DefaultChainWithEnvCredentials_IsResolved) {
    const NoDefaultChainCredentials no_credentials;
    SetEnvVar("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
    SetEnvVar("AWS_SECRET_ACCESS_KEY", "test-secret-key");
    const AuthProvider auth_provider("us-west-1", "");
    EXPECT_TRUE(auth_provider.HasResolvedCredentials());
}

// A first read that found nothing isn't final: reading the chain again picks up credentials that appeared since.
TEST_F(AuthProviderTest, ReloadCredentials_DefaultChain_ResolvesOnceCredentialsAppear) {
    const NoDefaultChainCredentials no_credentials;
    AuthProvider auth_provider("us-west-1", "");
    auth_provider.SetMinReloadInterval(std::chrono::milliseconds(0));
    ASSERT_FALSE(auth_provider.HasResolvedCredentials());
    EXPECT_FALSE(auth_provider.ReloadCredentials("us-west-1"));
    EXPECT_FALSE(auth_provider.HasResolvedCredentials());

    SetEnvVar("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
    SetEnvVar("AWS_SECRET_ACCESS_KEY", "test-secret-key");
    EXPECT_TRUE(auth_provider.ReloadCredentials("us-west-1"));
    EXPECT_TRUE(auth_provider.HasResolvedCredentials());
    // The token is signed with the credentials just read, not with the empty ones from the first read.
    const std::string token = auth_provider.GetToken("host.com", "us-west-1", "5432", "my-user", false).first;
    EXPECT_NE(std::string::npos, token.find("X-Amz-Credential=AKIDEXAMPLE"));
}

// Connect reloads on every attempt while the credentials are missing, so reads are spaced out. The constructor's
// read counts: the first connect right after it doesn't read the chain again.
TEST_F(AuthProviderTest, ReloadCredentials_WithinInterval_DoesNotReadTheChain) {
    const NoDefaultChainCredentials no_credentials;
    AuthProvider auth_provider("us-west-1", "");
    SetEnvVar("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
    SetEnvVar("AWS_SECRET_ACCESS_KEY", "test-secret-key");
    EXPECT_FALSE(auth_provider.ReloadCredentials("us-west-1"));
    EXPECT_FALSE(auth_provider.HasResolvedCredentials());

    auth_provider.SetMinReloadInterval(std::chrono::milliseconds(0));
    EXPECT_TRUE(auth_provider.ReloadCredentials("us-west-1"));
}

// Threads that share a provider can all find the credentials missing. Once one of them resolves them, the others
// don't read the chain again.
TEST_F(AuthProviderTest, ReloadCredentials_AlreadyResolved_DoesNotReadTheChain) {
    const NoDefaultChainCredentials no_credentials;
    SetEnvVar("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
    SetEnvVar("AWS_SECRET_ACCESS_KEY", "test-secret-key");
    AuthProvider auth_provider("us-west-1", "");
    ASSERT_TRUE(auth_provider.HasResolvedCredentials());

    UnsetEnvVar("AWS_ACCESS_KEY_ID");
    UnsetEnvVar("AWS_SECRET_ACCESS_KEY");
    EXPECT_TRUE(auth_provider.ReloadCredentials("us-west-1"));  // Within the interval.
    auth_provider.SetMinReloadInterval(std::chrono::milliseconds(0));
    EXPECT_TRUE(auth_provider.ReloadCredentials("us-west-1"));  // Past it.
    EXPECT_TRUE(auth_provider.HasResolvedCredentials());
}

// The monitor threads share one provider, so a thread can sign while another updates the credentials. Every token
// must come from a complete client, the old one or the new one.
TEST_F(AuthProviderTest, GetToken_WhileCredentialsUpdate_SignsWithACompleteClient) {
    const NoDefaultChainCredentials no_credentials;  // Keeps the client configuration away from IMDS.
    AuthProvider auth_provider("us-west-1", Aws::Auth::AWSCredentials("AKIDOLD", "old-secret-key"));
    std::atomic<bool> done = false;
    std::atomic<int> tokens = 0;
    std::atomic<int> unsigned_tokens = 0;
    std::vector<std::thread> signers;
    for (int i = 0; i < 4; ++i) {
        signers.emplace_back([&] {
            while (!done) {
                const std::string token =
                    auth_provider.GetToken("host.com", "us-west-1", "5432", "my-user", false).first;
                ++tokens;
                if (token.find("X-Amz-Credential=AKID") == std::string::npos) {
                    ++unsigned_tokens;
                }
            }
        });
    }
    while (tokens == 0) {
        std::this_thread::yield();
    }
    const int tokens_before_updates = tokens;
    for (int i = 0; i < 50; ++i) {
        auth_provider.UpdateAwsCredential(
            Aws::Auth::AWSCredentials(i % 2 == 0 ? "AKIDNEW" : "AKIDOLD", "test-secret-key"), "us-west-1");
    }
    const int tokens_during_updates = tokens - tokens_before_updates;
    done = true;
    for (std::thread& signer : signers) {
        signer.join();
    }
    EXPECT_GT(tokens_during_updates, 0);
    EXPECT_EQ(0, unsigned_tokens.load());
}

// The federated plugins build the provider from credentials they fetched themselves, so there is no chain to read.
// The chain has credentials here, so a provider that read it by mistake would resolve.
TEST_F(AuthProviderTest, ReloadCredentials_ExplicitCredentials_ReturnsFalse) {
    const NoDefaultChainCredentials no_credentials;
    SetEnvVar("AWS_ACCESS_KEY_ID", "AKIDEXAMPLE");
    SetEnvVar("AWS_SECRET_ACCESS_KEY", "test-secret-key");
    AuthProvider auth_provider("us-west-1", Aws::Auth::AWSCredentials());
    EXPECT_FALSE(auth_provider.ReloadCredentials("us-west-1"));
    EXPECT_FALSE(auth_provider.HasResolvedCredentials());
}
