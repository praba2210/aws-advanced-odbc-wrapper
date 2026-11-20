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

    // Sets SSLMODE, dropping its short form CA: verify-full, verify-ca and require stay; anything else becomes
    // verify-full. Merges pqopt: root_cert as sslrootcert for verify-full (none when it is ""), and the application
    // name, each only when the user's pqopt doesn't set it. Returns the 01S02 text for a mode it replaced with
    // verify-full, or "". ApplyConnectionDefaults calls it.
    static std::string ApplyTlsDefaults(std::map<std::string, std::string>& conn_attr, std::string_view root_cert);

    // Sets DATABASE to postgres, Aurora DSQL's only database, when neither DATABASE nor its short form DX is set:
    // libpq would use the user name. ApplyConnectionDefaults calls it.
    static void ApplyDatabaseDefault(std::map<std::string, std::string>& conn_attr);

    // Applies the Aurora DSQL connection defaults to the upper-cased connection attributes, the TLS ones with
    // ApplyTlsDefaults, and returns its 01S02 text. root_cert is the sslrootcert value a verify-full
    // connection gets when pqopt sets none (DefaultRootCert), or "" for no default. Public for tests.
    static std::string ApplyConnectionDefaults(std::map<std::string, std::string>& conn_attr,
                                               std::string_view root_cert = "system");

    // The sslrootcert default for the base driver's libpq version: "system" for libpq 16 or later; before 16 that value
    // is a file name in the working directory, so the first system CA bundle that is_trusted_file accepts, or "" when
    // there is none (libpq then looks for ~/.postgresql/root.crt and fails closed). An unknown version (logged at INFO)
    // counts as older than 16, except on Windows, where it gets "system". Public for tests.
    static std::string DefaultRootCert(std::optional<int> libpq_version,
                                       const std::function<bool(const char*)>& is_trusted_file = IsRootOwnedFile);

    // A regular file only root can change (symbolic links are followed). Never true on Windows. Public for tests.
    static bool IsRootOwnedFile(const char* path);

    // A hint for a connect that failed on the server certificate while relying on a default sslrootcert (system or a
    // system CA bundle) or on none, or "" when the error has nothing to do with it. Public for tests.
    static std::string_view GetRootCertHint(const std::map<std::string, std::string>& conn_attr,
                                            const std::string& error_message);

protected:
    // The region Connect resolved for this connect.
    std::string ResolveRegion(DBC* dbc) override;
    bool EnsureCredentials(DBC* dbc, const std::string& region, std::string& out_error) override;
    void OnConnectError(DBC* dbc, const std::string& sql_state, const std::string& error_message) override;

private:
    // What decides the provider: the AWS_PROFILE key (trimmed), else the AWS_PROFILE variable as is, the region the
    // chain is built with, and the environment variables that chain reads when it is built, as set (nullopt: unset).
    struct ProviderInputs {
        std::string key_profile;
        std::optional<std::string> env_profile;
        std::string region;
        std::map<std::string, std::optional<std::string>> build_env;
        bool operator==(const ProviderInputs&) const = default;
    };
    static ProviderInputs ReadProviderInputs(const std::map<std::string, std::string>& conn_attr, std::string region);

    // Builds the provider for the first connect, and a new one when this connect's inputs differ from the last build's,
    // the last connect found no credentials, or the server rejected its token. Returns false, with the error set on
    // dbc, when it can't.
    bool RebuildProviderIfNeeded(DBC* dbc, const std::string& region);

    // Connect and RebuildProviderIfNeeded change these with no lock of their own: a plugin belongs to one connection
    // handle, and a connect holds its dbc->lock (RDS_SQLConnect, RDS_SQLDriverConnect), so one thread at a time.
    bool owns_provider_;                       // false: the provider was given to the constructor (tests), and stays
    std::optional<ProviderInputs> built_for_;  // what the provider was built from; nullopt before the first build
    bool rebuild_provider_ = false;            // set when the last connect found no usable credentials, or the
                                               // server rejected its token
    std::string connect_region_;               // this connect's region, resolved once by Connect
};

#endif // DSQL_AUTH_PLUGIN_H_
