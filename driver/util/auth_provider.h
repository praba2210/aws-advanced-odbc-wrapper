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

#ifndef AUTH_PROVIDER_H_
#define AUTH_PROVIDER_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "aws_sdk_helper.h"
#include "connection_string_keys.h"
#include "rds_strings.h"

// Forward Declarations
namespace Aws {
    namespace Auth {
        class AWSCredentials;
    }
    namespace RDS {
        class RDSClient;
    }
}

enum AuthType : std::uint8_t {
    DATABASE,
    IAM,
    SECRETS_MANAGER,
    ADFS,
    OKTA,
    AWS_SSO,
    DSQL,
    INVALID,
};

static std::map<std::string, AuthType> const AUTH_TABLE = {
    {VALUE_AUTH_DATABASE,   AuthType::DATABASE},
    {VALUE_AUTH_IAM,        AuthType::IAM},
    {VALUE_AUTH_SECRETS,    AuthType::SECRETS_MANAGER},
    {VALUE_AUTH_ADFS,       AuthType::ADFS},
    {VALUE_AUTH_OKTA,       AuthType::OKTA},
    {VALUE_AUTH_SSO,        AuthType::AWS_SSO},
    {VALUE_AUTH_DSQL,       AuthType::DSQL},
};

struct TokenInfo {
    std::string token;
    std::chrono::time_point<std::chrono::steady_clock> expiration_point;
}; // TokenInfo;

struct DBC;

class AuthProvider {
public:
    AuthProvider() = default;
    explicit AuthProvider(const std::string &region);
    AuthProvider(const std::string &region, const std::string &profile);
    AuthProvider(const std::string &region, const Aws::Auth::AWSCredentials& credentials);
    explicit AuthProvider(const std::shared_ptr<Aws::RDS::RDSClient>& rds_client);
    virtual ~AuthProvider();

    virtual std::pair<std::string, bool> GetToken(
        const std::string &server,
        const std::string &region,
        const std::string &port,
        const std::string &username,
        bool use_cache = true,
        bool extra_url_encode = false,
        std::chrono::milliseconds time_to_expire_ms = DEFAULT_EXPIRATION_MS);
    virtual void UpdateAwsCredential(const Aws::Auth::AWSCredentials& credentials, const std::string &region = "");
    static std::string ExtraUrlEncodeString(const std::string &url_str);

    static std::string GetPort(DBC* dbc);
    static std::string GetRegionForProfile(const std::string &profile);
    // Credentials read from a chain also count as unresolved once they expire.
    [[nodiscard]] virtual bool HasResolvedCredentials() const { return CredentialsUsable(); }
    // Returns whether the credentials are resolved. If they aren't, first reads them again from the chain this
    // provider was built with, the default chain or a profile's. A provider built from explicit credentials, a client
    // or the default constructor has no chain and never reads.
    // Reads at most once per interval, counting the constructor's read. The interval starts at MIN_RELOAD_INTERVAL_MS,
    // doubles after each read that finds no usable credentials, up to MAX_RELOAD_INTERVAL_MS, and starts over after a
    // read that finds them. While another thread reads, returns false without reading.
    // Credentials it finds update this class's RDS client; an override of UpdateAwsCredential isn't called.
    [[nodiscard]] virtual bool ReloadCredentials(const std::string &region);

    static inline const std::chrono::milliseconds
        DEFAULT_EXPIRATION_MS = std::chrono::minutes(15);
    static AuthType AuthTypeFromString(const std::string& auth_type);
    static std::string BuildCacheKey(
        const std::string &server,
        const std::string &region,
        const std::string &port,
        const std::string &username);
    static void ClearCache();

protected:
    // For tests. Setting the minimum also restarts the current interval from it.
    void SetMinReloadInterval(std::chrono::milliseconds interval);
    void SetMaxReloadInterval(std::chrono::milliseconds interval);

private:
    // A chain read can wait seconds on IMDS, STS or credential_process, and the monitors connect every 100 ms.
    static inline const std::chrono::milliseconds MIN_RELOAD_INTERVAL_MS = std::chrono::seconds(5);
    // A read can run credential_process or sign in to STS or SSO again, and a monitor connects for as long as the
    // connection handle lives. Reads don't stop, so a handle recovers once the credentials are back, for example after
    // aws sso login or an IMDS outage.
    static inline const std::chrono::milliseconds MAX_RELOAD_INTERVAL_MS = std::chrono::minutes(2);

    [[nodiscard]] bool CredentialsUsable() const;
    void SetUpRdsClient(const Aws::Auth::AWSCredentials& credentials, const std::string &region);
    static inline std::unordered_map<std::string, TokenInfo> token_cache_;
    static inline std::recursive_mutex token_cache_mutex_;
    // The monitor threads share one provider, so the client is swapped and read under this lock.
    std::mutex rds_client_mutex_;
    std::shared_ptr<Aws::RDS::RDSClient> rds_client_;
    std::atomic<bool> credentials_resolved_{true};
    // The profile of the chain the credentials came from ("" for the default chain), if they came from one.
    std::optional<std::string> chain_profile_;
    // When the credentials read from the chain expire, in milliseconds since the epoch. The RDS client keeps a copy,
    // and chain credentials are often temporary (SSO, assume-role, IMDS).
    std::atomic<std::int64_t> chain_expiration_ms_{0};
    // One chain read at a time. Guards the members below.
    std::mutex reload_mutex_;
    std::chrono::steady_clock::time_point last_chain_read_;
    std::chrono::milliseconds min_reload_interval_ = MIN_RELOAD_INTERVAL_MS;
    std::chrono::milliseconds max_reload_interval_ = MAX_RELOAD_INTERVAL_MS;
    std::chrono::milliseconds reload_interval_ = MIN_RELOAD_INTERVAL_MS;
};

#endif // AUTH_PROVIDER_H_
