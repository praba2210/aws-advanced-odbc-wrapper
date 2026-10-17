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

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/dsql/DSQLClient.h>

#include "aws_sdk_helper.h"
#include "logger_wrapper.h"

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
    if (credentials_provider_->GetAWSCredentials().IsEmpty()) {
        dsql_credentials_resolved_ = false;
        LOG(ERROR) << "Unable to resolve AWS credentials for Aurora DSQL authentication"
                   << (profile.empty() ? "" : " with profile '" + profile + "'");
    }
    SetUpDsqlClient(region);
}

DsqlAuthProvider::DsqlAuthProvider(const std::string &region, const Aws::Auth::AWSCredentials& credentials) {
    AwsSdkHelper::Init();
    dsql_credentials_resolved_ = !credentials.IsEmpty();
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
    // The C++ SigV4 presigner skips signing when credentials are empty and returns an unsigned URL, which the server
    // rejects as a bad password. Check first, so no unsigned token is sent. Credentials can disappear after
    // construction, for example when an SSO session expires.
    if (credentials_provider_->GetAWSCredentials().IsEmpty()) {
        LOG(ERROR) << "No AWS credentials available for Aurora DSQL token generation";
        return {"", false};
    }

    // The signature covers the host; DNS is case-insensitive, so sign the lowercase form.
    std::string host = server;
    std::ranges::transform(host, host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto expires_in_s = std::chrono::duration_cast<std::chrono::seconds>(time_to_expire_ms).count();
    const bool is_admin = username == ADMIN_USER;
    const auto outcome = is_admin
        ? dsql_client_->GenerateDBConnectAdminAuthToken(host.c_str(), region.c_str(), expires_in_s)
        : dsql_client_->GenerateDBConnectAuthToken(host.c_str(), region.c_str(), expires_in_s);
    if (!outcome.IsSuccess()) {
        LOG(ERROR) << "Failed to generate an Aurora DSQL token: " << outcome.GetError().GetMessage();
        return {"", false};
    }
    const std::string aws_token = outcome.GetResult().c_str();
    const std::string token = extra_url_encode ? ExtraUrlEncodeString(aws_token) : aws_token;
    LOG(INFO) << "Generated new Aurora DSQL " << (is_admin ? "admin " : "") << "token length: " << token.length();
    return {token, false};
}

void DsqlAuthProvider::SetUpDsqlClient(const std::string &region)
{
    Aws::DSQL::DSQLClientConfiguration client_config;
    if (!region.empty()) {
        client_config.region = region;
    }
    dsql_client_ = std::make_shared<Aws::DSQL::DSQLClient>(credentials_provider_, nullptr, client_config);
}
