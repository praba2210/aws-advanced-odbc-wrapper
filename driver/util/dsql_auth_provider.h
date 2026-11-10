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
#include <string>
#include <utility>

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

    std::pair<std::string, bool> GetToken(
        const std::string &server,
        const std::string &region,
        const std::string &port,
        const std::string &username,
        bool use_cache = true,
        bool extra_url_encode = false,
        std::chrono::milliseconds time_to_expire_ms = DEFAULT_EXPIRATION_MS) override;
    // Checked against the provider on every call, so a handle recovers after `aws sso login` and stops at expiry.
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
    void SetUpDsqlClient(const std::string &region);

    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentials_provider_;
    std::shared_ptr<Aws::DSQL::DSQLClient> dsql_client_;
};

#endif // DSQL_AUTH_PROVIDER_H_
