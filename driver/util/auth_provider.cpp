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

#include "auth_provider.h"

#include <algorithm>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/config/AWSConfigFileProfileConfigLoader.h>
#include <aws/core/config/AWSProfileConfig.h>
#include <aws/core/utils/DateTime.h>
#include <aws/rds/RDSClient.h>

#include "../dialect/dialect.h"
#include "../driver.h"
#include "connection_string_keys.h"
#include "logger_wrapper.h"
#include "map_utils.h"
#include "number_utils.h"
#include "plugin_service.h"

namespace {
    // The default chain, or the chain for one profile: static access keys, AWS IAM Identity Center (SSO),
    // assume-role (source_profile/role_arn), and credential_process.
    Aws::Auth::AWSCredentials ReadChainCredentials(const std::string &profile) {
        if (profile.empty()) {
            return Aws::Auth::DefaultAWSCredentialsProviderChain().GetAWSCredentials();
        }
        Aws::Client::ClientConfiguration::CredentialProviderConfiguration credential_config;
        credential_config.profile = profile;
        return Aws::Auth::DefaultAWSCredentialsProviderChain(credential_config).GetAWSCredentials();
    }

    // CRT-backed providers (profile, process) report "never expires" as UINT64_MAX seconds, which overflows the
    // SDK's DateTime and reads as at or before 1970. Only an expiration after the epoch is a real one.
    bool IsExpired(const std::int64_t expiration_ms) {
        return expiration_ms > 0 && Aws::Utils::DateTime::Now().Millis() >= expiration_ms;
    }
}  // namespace

AuthProvider::AuthProvider(const std::string &region) : AuthProvider(region, "") {}

AuthProvider::AuthProvider(const std::string &region, const std::string &profile) : chain_profile_(profile) {
    AwsSdkHelper::Init();
    // With no credentials the SDK returns an unsigned token, which the server rejects as a bad password.
    const Aws::Auth::AWSCredentials credentials = ReadChainCredentials(profile);
    last_chain_read_ = std::chrono::steady_clock::now();
    chain_expiration_ms_ = credentials.GetExpiration().Millis();
    if (credentials.IsEmpty()) {
        credentials_resolved_ = false;
        if (profile.empty()) {
            LOG(ERROR) << "Unable to resolve AWS credentials from the default credential chain";
        } else {
            LOG(ERROR) << "Unable to resolve AWS credentials for profile '" << profile
                       << "'. If it is an AWS IAM Identity Center (SSO) profile, run 'aws sso login --profile "
                       << profile << "'.";
        }
    } else if (IsExpired(chain_expiration_ms_)) {
        LOG(ERROR) << "The AWS credentials from the credential chain have expired";
    }
    SetUpRdsClient(credentials, region);
}

AuthProvider::AuthProvider(
    const std::string &region,
    const Aws::Auth::AWSCredentials& credentials)
{
    AwsSdkHelper::Init();
    if (credentials.IsEmpty()) {
        credentials_resolved_ = false;
        LOG(INFO) << "AuthProvider created without resolved AWS credentials. Must be resolved before token generation";
    }
    SetUpRdsClient(credentials, region);
}

AuthProvider::AuthProvider(const std::shared_ptr<Aws::RDS::RDSClient>& rds_client)
{
    AwsSdkHelper::Init();
    this->rds_client_ = rds_client;
}

AuthProvider::~AuthProvider()
{
    if (rds_client_) {
        rds_client_ = nullptr;
    }
    AwsSdkHelper::Shutdown();
}

std::string AuthProvider::GetRegionForProfile(const std::string &profile) {
    if (profile.empty()) {
        return "";
    }

    Aws::Config::AWSConfigFileProfileConfigLoader loader(Aws::Auth::GetConfigProfileFilename(), true);
    if (!loader.Load()) {
        LOG(WARNING) << "Unable to load AWS config file while resolving region for profile '" << profile << "'";
        return "";
    }

    const auto &profiles = loader.GetProfiles();
    const auto it = profiles.find(profile);
    if (it == profiles.end()) {
        return "";
    }
    return it->second.GetRegion();
}

std::pair<std::string, bool> AuthProvider::GetToken(
    const std::string &server,
    const std::string &region,
    const std::string &port,
    const std::string &username,
    bool use_cache,
    bool extra_url_encode,
    std::chrono::milliseconds time_to_expire_ms)
{
    const std::string cache_key = BuildCacheKey(server, region, port, username);
    const std::chrono::time_point<std::chrono::steady_clock> curr_time = std::chrono::steady_clock::now();
    TokenInfo token_info{.token = "", .expiration_point = curr_time + time_to_expire_ms};

    if (use_cache) {
        const std::lock_guard<std::recursive_mutex> lock_guard(token_cache_mutex_);
        if (token_cache_.contains(cache_key)) {
            LOG(INFO) << "Found token in cache";
            token_info = token_cache_.at(cache_key);
            if (curr_time < token_info.expiration_point) {
                return {token_info.token, true};
            }
            LOG(INFO) << "Existing token expired";
            token_cache_.erase(cache_key);
        }
    }

    const std::optional<int> parsed_port = NumberUtils::ParseInt(port);
    if (!parsed_port.has_value()) {
        // A token signed for a bogus port can never authenticate, fail fast instead.
        LOG(ERROR) << "Invalid port \"" << port << "\" for auth token generation";
        return {"", false};
    }
    std::shared_ptr<Aws::RDS::RDSClient> rds_client;
    {
        const std::lock_guard<std::mutex> lock_guard(rds_client_mutex_);
        rds_client = rds_client_;
    }
    const std::string aws_token =
        rds_client->GenerateConnectAuthToken(server.c_str(), region.c_str(), parsed_port.value(), username.c_str());
    if (aws_token.empty()) {
        // Not cached: the next connect asks the SDK again.
        LOG(ERROR) << "The AWS SDK returned an empty token for " << server << ":" << port << " in region " << region;
        return {"", false};
    }
    token_info.token = extra_url_encode ? ExtraUrlEncodeString(aws_token) : aws_token;
    {
        const std::lock_guard<std::recursive_mutex> lock_guard(token_cache_mutex_);
        token_cache_.insert_or_assign(cache_key, token_info);
    }
    LOG(INFO) << "Generated new token length: " << token_info.token.length();
    return {token_info.token, false};
}

bool AuthProvider::ReloadCredentials(const std::string &region) {
    if (!chain_profile_) {
        return HasResolvedCredentials();
    }
    // The monitor threads share a provider and connect every 100 ms. One of them reads the chain at a time; the
    // others fail this attempt instead of waiting.
    const std::unique_lock<std::mutex> lock(reload_mutex_, std::try_to_lock);
    if (CredentialsUsable()) {
        return true;
    }
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!lock.owns_lock() || now - last_chain_read_ < reload_interval_) {
        return false;
    }
    if (credentials_resolved_) {
        LOG(INFO) << "The AWS credentials from the credential chain have expired; reading the chain again";
    }
    const Aws::Auth::AWSCredentials credentials = ReadChainCredentials(*chain_profile_);
    // After the read, so a read that took longer than the interval doesn't start the next one right away.
    last_chain_read_ = std::chrono::steady_clock::now();
    if (credentials.IsEmpty() || IsExpired(credentials.GetExpiration().Millis())) {
        reload_interval_ = std::min<std::chrono::milliseconds>(reload_interval_ * 2, max_reload_interval_);
        LOG(WARNING) << "No usable AWS credentials from the credential chain. A connect reads it again after "
                     << std::chrono::duration_cast<std::chrono::seconds>(reload_interval_).count() << " s";
        return false;
    }
    reload_interval_ = min_reload_interval_;
    LOG(INFO) << "Resolved AWS credentials on a later read of the credential chain";
    // Not the virtual call: an override must not run while reload_mutex_ is held.
    AuthProvider::UpdateAwsCredential(credentials, region);
    // After the new client is in place, so a thread that sees the new expiration signs with the new credentials.
    chain_expiration_ms_ = credentials.GetExpiration().Millis();
    return true;
}

bool AuthProvider::CredentialsUsable() const {
    return credentials_resolved_ && !IsExpired(chain_expiration_ms_);
}

void AuthProvider::SetMinReloadInterval(const std::chrono::milliseconds interval) {
    const std::lock_guard<std::mutex> lock(reload_mutex_);
    min_reload_interval_ = interval;
    reload_interval_ = interval;
}

void AuthProvider::SetMaxReloadInterval(const std::chrono::milliseconds interval) {
    const std::lock_guard<std::mutex> lock(reload_mutex_);
    max_reload_interval_ = interval;
}

void AuthProvider::UpdateAwsCredential(const Aws::Auth::AWSCredentials& credentials, const std::string &region) {
    // Set the flag only after the new client is in place, so a thread that sees it turn true signs with the new
    // credentials. Clear it first when the new credentials are empty.
    const bool resolved = !credentials.IsEmpty();
    if (!resolved) {
        credentials_resolved_ = false;
    }
    SetUpRdsClient(credentials, region);
    if (resolved) {
        credentials_resolved_ = true;
    }
}

std::string AuthProvider::ExtraUrlEncodeString(const std::string &url_str) {
    LOG(INFO) << "Original Token Length: " << url_str.length();
    std::string result = url_str;
    size_t pos = 0;
    const std::string replacement = "%25";
    // Replace all instances of "%" in the url_str with "%25".
    while ((pos = result.find('%', pos)) != std::string::npos) {
        result.replace(pos, 1, replacement);
        pos += replacement.length();
    }
    LOG(INFO) << "URL Encoded Token Length: " << result.length();
    return result;
}

std::string AuthProvider::GetPort(DBC* dbc) {
    std::string port = MapUtils::GetStringValue(dbc->conn_attr, KEY_IAM_PORT, "");
    if (port.empty()) {
        port = MapUtils::GetStringValue(dbc->conn_attr, KEY_PORT, "");
    }
    if (port.empty() && dbc->plugin_service) {
        const std::shared_ptr<Dialect> dialect = dbc->plugin_service->GetDialect();
        if (dialect) {
            const int default_port = dialect->GetDefaultPort();
            if (default_port > 0) {
                port = std::to_string(default_port);
            }
        }
    }
    return port;
}

AuthType AuthProvider::AuthTypeFromString(const std::string &auth_type) {
    const std::string local_str_upper = RDS_STR_UPPER(auth_type);
    if (AUTH_TABLE.contains(local_str_upper)) {
        return AUTH_TABLE.at(local_str_upper);
    }
    return AuthType::INVALID;
}

std::string AuthProvider::BuildCacheKey(
    const std::string &server,
    const std::string &region,
    const std::string &port,
    const std::string &username)
{
    std::stringstream key_builder;
    key_builder << server << "-" << region << "-" << port << "-" << username;
    return key_builder.str();
}

void AuthProvider::ClearCache() {
    const std::lock_guard<std::recursive_mutex> lock_guard(token_cache_mutex_);
    token_cache_.clear();
}

void AuthProvider::SetUpRdsClient(const Aws::Auth::AWSCredentials& credentials, const std::string &region) {
    Aws::RDS::RDSClientConfiguration client_config;
    if (!region.empty()) {
        client_config.region = region;
    }

    // Only the swap is locked. Building the configuration above can wait on IMDS for a region, and the old client is
    // released after the lock.
    auto rds_client = std::make_shared<Aws::RDS::RDSClient>(
        credentials,
        client_config
    );
    const std::lock_guard<std::mutex> lock_guard(rds_client_mutex_);
    rds_client_.swap(rds_client);
}
