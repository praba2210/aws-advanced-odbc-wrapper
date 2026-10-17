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
#include <aws/core/utils/DateTime.h>

#include <chrono>
#include <string>

#include "../../driver/util/aws_sdk_helper.h"
#include "../../driver/util/dsql_auth_provider.h"

namespace {
    const std::string HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string REGION = "us-east-1";
    const std::string PORT = "5432";
    const std::chrono::milliseconds EXPIRATION = std::chrono::seconds(900);
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
