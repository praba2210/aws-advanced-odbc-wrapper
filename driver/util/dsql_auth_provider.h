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
#include <memory>
#include <string>
#include <utility>

#include "auth_provider.h"

// Forward Declarations
namespace Aws {
    namespace Auth {
        class AWSCredentialsProvider;
    }
    namespace DSQL {
        class DSQLClient;
    }
}

// Generates Aurora DSQL IAM tokens. Tokens are presigned locally, so every GetToken call mints a fresh one and
// nothing is cached. DSQL tokens don't include the port.
class DsqlAuthProvider : public AuthProvider {
public:
    DsqlAuthProvider(const std::string &region, const std::string &profile);
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

private:
    void SetUpDsqlClient(const std::string &region);

    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentials_provider_;
    std::shared_ptr<Aws::DSQL::DSQLClient> dsql_client_;
};

#endif // DSQL_AUTH_PROVIDER_H_
