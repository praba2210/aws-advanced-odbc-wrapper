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

#include "dsql_auth_provider.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/GenericClientConfiguration.h>
#include <aws/dsql/DSQLClient.h>

#include "aws_sdk_helper.h"
#include "logger_wrapper.h"

namespace {
    // Why these credentials can't sign a DSQL token, or "" when they can. The C++ SigV4 presigner skips signing when
    // either key is empty and returns an unsigned URL, and it signs with expired credentials; the server rejects both
    // as a bad password.
    std::string_view CredentialsProblem(const Aws::Auth::AWSCredentials& credentials)
    {
        if (credentials.GetAWSAccessKeyId().empty() || credentials.GetAWSSecretKey().empty()) {
            return "No AWS credentials available for Aurora DSQL token generation";
        }
        // CRT-backed providers (profile, process) report "never expires" as UINT64_MAX seconds, which overflows the
        // SDK's DateTime and reads as at or before 1970. Only an expiration after the epoch is a real one.
        if (credentials.GetExpiration().Millis() > 0 && credentials.IsExpired()) {
            return "AWS credentials for Aurora DSQL token generation have expired. If they come from AWS IAM Identity "
                   "Center (SSO), run 'aws sso login'";
        }
        return "";
    }
}  // namespace

DsqlAuthProvider::DsqlAuthProvider(const std::string &region, const std::string &profile) {
    // The base default constructor skips Init, but the base destructor always calls Shutdown.
    AwsSdkHelper::Init();
    if (profile.empty()) {
        credentials_provider_ = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>();
    } else {
        Aws::Client::ClientConfiguration::CredentialProviderConfiguration credential_config;
        credential_config.profile = profile;
        credentials_provider_ = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>(credential_config);
    }
    // Keep the provider, not a snapshot, so expiring SSO and assume-role credentials refresh on their own.
    // Qualified: a virtual call from a constructor never reaches an override anyway.
    if (!DsqlAuthProvider::HasResolvedCredentials()) {
        LOG(ERROR) << "Unable to resolve AWS credentials for Aurora DSQL authentication"
                   << (profile.empty() ? "" : " with profile '" + profile + "'");
    }
    SetUpDsqlClient(region);
}

DsqlAuthProvider::DsqlAuthProvider(const std::string &region, const Aws::Auth::AWSCredentials& credentials) {
    AwsSdkHelper::Init();
    credentials_provider_ = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(credentials);
    SetUpDsqlClient(region);
}

std::pair<std::string, bool> DsqlAuthProvider::GetToken(
    const std::string &server,
    const std::string &region,
    const std::string & /*port*/,
    const std::string &username,
    bool /*use_cache*/,
    bool extra_url_encode,
    std::chrono::milliseconds time_to_expire_ms)
{
    // Check before signing, so an unusable token is never sent. Credentials can disappear or expire after
    // construction, for example when an SSO session ends.
    if (const std::string_view problem = CredentialsProblem(credentials_provider_->GetAWSCredentials()); !problem.empty()) {
        LOG(ERROR) << problem;
        return {"", false};
    }

    // The signature covers the host; DNS is case-insensitive, so sign the lowercase form.
    std::string host = server;
    std::ranges::transform(host, host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto expires_in_s = std::chrono::duration_cast<std::chrono::seconds>(time_to_expire_ms).count();
    const bool is_admin = username == ADMIN_USER;
    const auto outcome = is_admin
        ? dsql_client_->GenerateDBConnectAdminAuthToken(host, region, expires_in_s)
        : dsql_client_->GenerateDBConnectAuthToken(host, region, expires_in_s);
    if (!outcome.IsSuccess()) {
        LOG(ERROR) << "Failed to generate an Aurora DSQL token: " << outcome.GetError().GetMessage();
        return {"", false};
    }
    const std::string& aws_token = outcome.GetResult();
    const std::string token = extra_url_encode ? ExtraUrlEncodeString(aws_token) : aws_token;
    LOG(INFO) << "Generated new Aurora DSQL " << (is_admin ? "admin " : "") << "token length: " << token.length();
    return {token, false};
}

Aws::Client::GenericClientConfiguration DsqlAuthProvider::MakeClientConfiguration(const std::string &region)
{
    // The token is signed for the region GetToken is given, so the client's own region never needs an IMDS lookup,
    // which off EC2 waits for its timeouts. The credentials chain is separate and still reads IMDS for an instance role.
    const Aws::Client::ClientConfigurationInitValues init_values{.shouldDisableIMDS = true};
    Aws::DSQL::DSQLClientConfiguration client_config(init_values);
    if (!region.empty()) {
        client_config.region = region;
    }
    return client_config;
}

void DsqlAuthProvider::SetUpDsqlClient(const std::string &region)
{
    dsql_client_ = std::make_shared<Aws::DSQL::DSQLClient>(
        credentials_provider_, nullptr, MakeClientConfiguration(region));
}

bool DsqlAuthProvider::HasResolvedCredentials() const
{
    return CredentialsProblem(credentials_provider_->GetAWSCredentials()).empty();
}
