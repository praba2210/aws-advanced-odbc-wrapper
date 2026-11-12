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

#ifndef DSQL_AUTH_PROVIDER_H_
#define DSQL_AUTH_PROVIDER_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <aws/core/auth/AWSCredentials.h>

#include "auth_provider.h"

// Forward Declarations
namespace Aws {
    namespace Auth {
        class AWSCredentialsProvider;
        class AWSCredentialsProviderChain;
    }
    namespace Client {
        struct GenericClientConfiguration;
    }
    namespace Crt::Auth {
        class ICredentialsProvider;
    }
    namespace DSQL {
        class DSQLClient;
    }
}  // namespace Aws

// Generates Aurora DSQL IAM tokens. Tokens are presigned locally, so every GetToken call mints a fresh one and
// nothing is cached. DSQL tokens don't include the port.
class DsqlAuthProvider : public AuthProvider {
public:
    // Where the profile name comes from. With the AWS_PROFILE key, only that profile's credentials are used.
    enum class ProfileSource : std::uint8_t { DEFAULT_CHAIN, KEY };

    DsqlAuthProvider(const std::string &region, const std::string &profile,
                     ProfileSource source = ProfileSource::DEFAULT_CHAIN);
    DsqlAuthProvider(const std::string &region, const Aws::Auth::AWSCredentials& credentials);
    // Reads the credentials from the given source. Public for tests.
    DsqlAuthProvider(const std::string &region, std::shared_ptr<Aws::Auth::AWSCredentialsProvider> source);

    std::pair<std::string, bool> GetToken(
        const std::string &server,
        const std::string &region,
        const std::string &port,
        const std::string &username,
        bool use_cache = true,
        bool extra_url_encode = false,
        std::chrono::milliseconds time_to_expire_ms = DEFAULT_EXPIRATION_MS) override;
    // Reads the credentials source once and keeps the result for the next GetToken, which then doesn't read again, so a
    // connect reads once. Checked against the source on every call, so a handle recovers after `aws sso login` and
    // stops at expiry. A failed check keeps nothing.
    [[nodiscard]] bool HasResolvedCredentials() const override;

    static inline const std::string ADMIN_USER = "admin";

    // The DSQL client's configuration. Public for tests.
    static Aws::Client::GenericClientConfiguration MakeClientConfiguration(const std::string &region);
    // Whether the AWS config file ("[profile p]", "[default]") or the credentials file ("[p]") has the profile.
    static bool ProfileExists(const std::string& profile);
    // The credentials source for the AWS_PROFILE key. Public for tests.
    static std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> MakeProfileOnlyChain(const std::string& profile,
                                                                                      const std::string& region);
    // The CRT's `aws login` provider for the profile, which reads that profile's login_session and endpoint settings.
    // Null when the config file has no such profile or it has no login_session. Public for tests.
    static std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> MakeLoginCrtProvider(const std::string& profile,
                                                                                    const std::string& region);

private:
    class SnapshotCredentialsProvider;
    struct ProfileFiles;

    // MakeProfileOnlyChain and MakeLoginCrtProvider for the profile as the files have it, read once for a build.
    static std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> BuildProfileOnlyChain(const std::string& profile,
        const std::string& region, const ProfileFiles& files);
    static std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> BuildLoginCrtProvider(const std::string& profile,
        const std::string& region, const ProfileFiles& files);

    void SetUpDsqlClient(const std::string &region);
    // The credentials HasResolvedCredentials kept, which are then gone; else one read of the source.
    Aws::Auth::AWSCredentials TakeCheckedCredentials() const;

    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> source_;
    // What the DSQL client signs with: the checked credentials for the length of one signing call.
    std::shared_ptr<SnapshotCredentialsProvider> signing_;
    std::shared_ptr<Aws::DSQL::DSQLClient> dsql_client_;
    std::mutex sign_mutex_;  // one signing call at a time, so the snapshot is not replaced under it
    mutable std::mutex pending_mutex_;
    mutable std::optional<Aws::Auth::AWSCredentials> pending_;
};

#endif // DSQL_AUTH_PROVIDER_H_
