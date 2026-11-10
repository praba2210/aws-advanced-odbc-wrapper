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

#ifndef DSQL_AUTH_PLUGIN_H_
#define DSQL_AUTH_PLUGIN_H_

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "../../dialect/dialect.h"
#include "../../driver.h"
#include "../../util/auth_provider.h"
#include "../../util/odbc_helper.h"
#include "../base_token_auth_plugin.h"

class DsqlAuthPlugin : public BaseTokenAuthPlugin {
public:
    DsqlAuthPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin);
    DsqlAuthPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin, const std::shared_ptr<AuthProvider>& auth_provider);
    DsqlAuthPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin,
                   const std::shared_ptr<AuthProvider>& auth_provider,
                   std::shared_ptr<Dialect> dialect,
                   std::shared_ptr<OdbcHelper> odbc_helper);

    SQLRETURN Connect(
        SQLHDBC        ConnectionHandle,
        SQLHWND        WindowHandle,
        SQLTCHAR *     OutConnectionString,
        SQLSMALLINT    BufferLength,
        SQLSMALLINT *  StringLengthPtr,
        SQLUSMALLINT   DriverCompletion) override;

    // Applies the Aurora DSQL connection defaults to the upper-cased connection attributes. root_cert is the
    // sslrootcert value a verify-full connection gets when pqopt sets none (DefaultRootCert), or "" for no default.
    // Public for tests.
    static void ApplyConnectionDefaults(std::map<std::string, std::string>& conn_attr,
                                        std::string_view root_cert = "system");

    // The sslrootcert default for the base driver's libpq version: "system" for libpq 16 or later or an unknown
    // version; before 16 that value is a file name in the working directory, so the first system CA bundle that
    // is_trusted_file accepts, or "" when there is none (libpq then looks for ~/.postgresql/root.crt and fails closed).
    // Public for tests.
    static std::string DefaultRootCert(std::optional<int> libpq_version,
                                       const std::function<bool(const char*)>& is_trusted_file = IsRootOwnedFile);

    // A regular file only root can change (symbolic links are followed). Never true on Windows. Public for tests.
    static bool IsRootOwnedFile(const char* path);

    // A hint for a connect that failed on the server certificate while relying on a default sslrootcert (system or a
    // system CA bundle) or on none, or "" when the error has nothing to do with it. Public for tests.
    static std::string_view GetRootCertHint(const std::map<std::string, std::string>& conn_attr,
                                            const std::string& error_message);

    // When RDS_AUTH_TYPE is DSQL, replaces an Aurora DSQL cluster ID in SERVER or IAM_HOST with the cluster endpoint,
    // <id>.dsql.<region>.on.aws. Returns false, with the connect error in out_error, when no region resolves or the
    // region isn't a DNS host label. Public for tests.
    static bool ExpandClusterId(std::map<std::string, std::string>& conn_attr, std::string& out_error);

    // The same for a connection handle; on false it has logged the error and set dbc->err (08001).
    // RDS_InitializeConnection calls it before anything reads the host.
    static bool ExpandClusterId(DBC* dbc);

    // The 01S02 message for an SSLmode that ApplyConnectionDefaults replaces with verify-full, or "" when the mode is
    // kept or not set. It reads the mode as the user set it, so call it before ApplyConnectionDefaults. Public for tests.
    static std::string GetTlsOverrideWarning(const std::map<std::string, std::string>& conn_attr);

protected:
    std::string ResolveRegion(DBC* dbc) override;
    bool EnsureCredentials(DBC* dbc, const std::string& region, std::string& out_error) override;
    void OnConnectError(DBC* dbc, const std::string& sql_state, const std::string& error_message) override;

private:
    // What decides the provider: the AWS_PROFILE key (trimmed), else the AWS_PROFILE variable as is, and the region
    // the chain is built with.
    struct ProviderInputs {
        std::string key_profile;
        std::optional<std::string> env_profile;
        std::string region;
        bool operator==(const ProviderInputs&) const = default;
    };
    static ProviderInputs ReadProviderInputs(const std::map<std::string, std::string>& conn_attr);

    // Builds a new provider when this connect's inputs differ from the last build's or the last connect found no
    // credentials. Returns false, with the error set on dbc, when it can't.
    bool RebuildProviderIfNeeded(DBC* dbc);

    std::optional<ProviderInputs> built_for_;  // nullopt: the provider was given to the constructor (tests)
    bool rebuild_provider_ = false;            // set when the last connect found no usable credentials
};

#endif // DSQL_AUTH_PLUGIN_H_
