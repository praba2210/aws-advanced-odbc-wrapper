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

#include "dsql_auth_plugin.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>

#include "../../util/connection_string_keys.h"
#include "../../util/dsql_auth_provider.h"
#include "../../util/logger_wrapper.h"
#include "../../util/map_utils.h"
#include "../../util/rds_strings.h"
#include "../../util/rds_utils.h"

namespace {
    // psqlODBC keywords, upper-cased as in conn_attr. They aren't wrapper keys, so they reach the base driver.
    const std::string PG_KEY_SSLMODE = "SSLMODE";
    const std::string PG_KEY_SSLMODE_SHORT = "CA";
    const std::string PG_KEY_DATABASE_SHORT = "DX";
    const std::string PG_KEY_PQOPT = "PQOPT";
    const std::string PG_KEY_PQOPT_SHORT = "D5";

    const std::string SSLMODE_VERIFY_FULL = "verify-full";
    const std::string SSLMODE_VERIFY_CA = "verify-ca";
    const std::string SSLMODE_REQUIRE = "require";
    const std::string SYSTEM_ROOT_CERT = "sslrootcert=system";
    const std::string DEFAULT_PROFILE = "default";
    const std::string DEFAULT_DATABASE = "postgres";
    const std::string APPLICATION_NAME = std::string("aurora-dsql-odbc/") + DRIVER_VERSION;
    // An Aurora DSQL cluster ID, as .NET, Go, PHP, Ruby and Rust test it: 26 letters or digits.
    constexpr size_t CLUSTER_ID_LENGTH = 26;

    bool ContainsAny(const std::map<std::string, std::string>& conn_attr, std::initializer_list<std::string> keys)
    {
        return std::ranges::any_of(keys, [&conn_attr](const std::string& key) { return conn_attr.contains(key); });
    }

    std::string ToLower(std::string text)
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // Letters match in any case, as the endpoint pattern does; the expansion lowercases them.
    bool IsClusterId(const std::string& host)
    {
        return host.size() == CLUSTER_ID_LENGTH && std::ranges::all_of(host, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        });
    }

    // Aurora DSQL rejects plaintext. Verify the server by default, as the Go, .NET, PHP, Ruby and Rust connectors do.
    // verify-ca and require still encrypt, and a PrivateLink endpoint's own DNS name can't pass verify-full, so they
    // stay. Any other mode becomes verify-full. psqlODBC reads the mode's first letter case-sensitively, so the
    // result is lowercase.
    std::string ResolveSslMode(const std::string& requested_mode)
    {
        const std::string requested_upper = RDS_STR_UPPER(requested_mode);
        for (const std::string& mode : {SSLMODE_VERIFY_FULL, SSLMODE_VERIFY_CA, SSLMODE_REQUIRE}) {
            if (requested_upper == RDS_STR_UPPER(mode)) {
                if (mode == SSLMODE_REQUIRE) {
                    LOG(WARNING) << "[DSQL] SSLmode=require encrypts but doesn't verify the server certificate";
                } else if (mode == SSLMODE_VERIFY_CA) {
                    LOG(WARNING) << "[DSQL] SSLmode=verify-ca verifies the server certificate but not its host name";
                }
                return mode;
            }
        }
        if (!requested_mode.empty()) {
            LOG(WARNING) << "[DSQL] Overriding SSLmode=" << requested_mode << " with " << SSLMODE_VERIFY_FULL;
        }
        return SSLMODE_VERIFY_FULL;
    }

    // pqopt is a brace-quoted libpq conninfo string. Add each option the user didn't set; keep theirs. libpq rejects
    // sslrootcert=system with any mode weaker than verify-full, so it's only a default for verify-full.
    std::string MergePqopt(std::string pqopt, const std::string& ssl_mode)
    {
        if (pqopt.size() >= 2 && pqopt.front() == '{' && pqopt.back() == '}') {
            pqopt = pqopt.substr(1, pqopt.size() - 2);
        }
        const auto add_if_missing = [&pqopt](const std::string& name, const std::string& value) {
            if (pqopt.find(name) == std::string::npos) {
                pqopt += (pqopt.empty() ? "" : " ") + name + "=" + value;
            }
        };
        if (ssl_mode == SSLMODE_VERIFY_FULL) {
            add_if_missing("sslrootcert", "system");
        }
        add_if_missing("application_name", APPLICATION_NAME);
        return "{" + pqopt + "}";
    }

    // pqopt, or its short form D5 when only that one is set.
    const std::string& PqoptKey(const std::map<std::string, std::string>& conn_attr)
    {
        return (!conn_attr.contains(PG_KEY_PQOPT) && conn_attr.contains(PG_KEY_PQOPT_SHORT)) ? PG_KEY_PQOPT_SHORT
                                                                                             : PG_KEY_PQOPT;
    }

    std::string GetEnvRegion()
    {
        for (const char* name : {"AWS_REGION", "AWS_DEFAULT_REGION"}) {
            if (const char* value = std::getenv(name); value && value[0] != '\0') {
                return value;
            }
        }
        return "";
    }

    // The profile the credentials come from: the AWS_PROFILE key, else the AWS_PROFILE variable, else "default". This
    // is the SDK's profile provider order; it doesn't read AWS_DEFAULT_PROFILE.
    std::string GetCredentialsProfile(const std::map<std::string, std::string>& conn_attr)
    {
        if (std::string profile = MapUtils::GetStringValue(conn_attr, KEY_AWS_PROFILE, ""); !profile.empty()) {
            return profile;
        }
        if (const char* value = std::getenv("AWS_PROFILE"); value && value[0] != '\0') {
            return value;
        }
        return DEFAULT_PROFILE;
    }

    // REGION, then the region in the DSQL endpoint being signed (IAM_HOST, then SERVER: behind a PrivateLink
    // endpoint without private DNS, SERVER is the endpoint's own name), then AWS_REGION / AWS_DEFAULT_REGION (the
    // order most DSQL connectors use), then the region of the credentials' profile. There is no default region: an
    // empty result fails the connect with "Missing required parameters: Region". The result is lowercase: SigV4 signs
    // the region as given, and region names are lowercase.
    std::string ResolveDsqlRegion(const std::map<std::string, std::string>& conn_attr)
    {
        std::string region = MapUtils::GetStringValue(conn_attr, KEY_REGION, "");
        for (const char* host_key : {KEY_IAM_HOST, KEY_SERVER}) {
            if (region.empty()) {
                region = RdsUtils::GetDsqlRegion(MapUtils::GetStringValue(conn_attr, host_key, ""));
            }
        }
        if (region.empty()) {
            region = GetEnvRegion();
        }
        if (region.empty()) {
            region = AuthProvider::GetRegionForProfile(GetCredentialsProfile(conn_attr));
        }
        return ToLower(region);
    }

    std::shared_ptr<AuthProvider> CreateDsqlAuthProvider(DBC* dbc, const std::shared_ptr<AuthProvider>& auth_provider)
    {
        if (auth_provider) {
            return auth_provider;
        }
        const std::string profile = MapUtils::GetStringValue(dbc->conn_attr, KEY_AWS_PROFILE, "");
        return std::make_shared<DsqlAuthProvider>(ResolveDsqlRegion(dbc->conn_attr), profile);
    }
}  // namespace

DsqlAuthPlugin::DsqlAuthPlugin(DBC *dbc, std::shared_ptr<BasePlugin> next_plugin)
    : DsqlAuthPlugin(dbc, next_plugin, nullptr, nullptr, nullptr) {}

DsqlAuthPlugin::DsqlAuthPlugin(DBC *dbc, std::shared_ptr<BasePlugin> next_plugin, const std::shared_ptr<AuthProvider>& auth_provider)
    : DsqlAuthPlugin(dbc, next_plugin, auth_provider, nullptr, nullptr) {}

DsqlAuthPlugin::DsqlAuthPlugin(DBC *dbc, std::shared_ptr<BasePlugin> next_plugin,
                               const std::shared_ptr<AuthProvider>& auth_provider,
                               std::shared_ptr<Dialect> dialect,
                               std::shared_ptr<OdbcHelper> odbc_helper)
    : BaseTokenAuthPlugin(dbc, next_plugin, CreateDsqlAuthProvider(dbc, auth_provider), dialect, odbc_helper)
{
    this->plugin_name_ = "DSQL";
}

std::string DsqlAuthPlugin::ResolveRegion(DBC* dbc)
{
    return ResolveDsqlRegion(dbc->conn_attr);
}

SQLRETURN DsqlAuthPlugin::Connect(
    SQLHDBC        ConnectionHandle,
    SQLHWND        WindowHandle,
    SQLTCHAR *     OutConnectionString,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr,
    SQLUSMALLINT   DriverCompletion)
{
    ApplyConnectionDefaults(static_cast<DBC*>(ConnectionHandle)->conn_attr);
    return BaseTokenAuthPlugin::Connect(
        ConnectionHandle, WindowHandle, OutConnectionString, BufferLength, StringLengthPtr, DriverCompletion);
}

void DsqlAuthPlugin::OnConnectError(DBC* dbc, const std::string& sql_state, const std::string& error_message)
{
    if (const std::string_view hint = GetRootCertHint(dbc->conn_attr, error_message); !hint.empty()) {
        LOG(WARNING) << hint;
    }
}

void DsqlAuthPlugin::ApplyConnectionDefaults(std::map<std::string, std::string>& conn_attr)
{
    const std::string ssl_mode = ResolveSslMode(MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE,
        MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE_SHORT, "")));
    conn_attr.erase(PG_KEY_SSLMODE_SHORT);
    conn_attr.insert_or_assign(PG_KEY_SSLMODE, ssl_mode);

    // A connection string keeps USER and USERNAME as typed (only DSN entries are mapped to UID), but the token is
    // signed for UID. Move an alias to UID, so psqlODBC also gets a single user name.
    for (const char* alias : {ALIAS_KEY_USERNAME_1, ALIAS_KEY_USERNAME_2}) {
        if (const auto it = conn_attr.find(alias); it != conn_attr.end()) {
            conn_attr.try_emplace(KEY_DB_USERNAME, it->second);
            conn_attr.erase(it);
        }
    }
    conn_attr.try_emplace(KEY_DB_USERNAME, DsqlAuthProvider::ADMIN_USER);
    if (!ContainsAny(conn_attr, {KEY_DATABASE, PG_KEY_DATABASE_SHORT})) {
        conn_attr.insert_or_assign(KEY_DATABASE, DEFAULT_DATABASE);
    }

    const std::string& pqopt_key = PqoptKey(conn_attr);
    conn_attr.insert_or_assign(pqopt_key, MergePqopt(MapUtils::GetStringValue(conn_attr, pqopt_key, ""), ssl_mode));
}

std::string_view DsqlAuthPlugin::GetRootCertHint(const std::map<std::string, std::string>& conn_attr,
                                                 const std::string& error_message)
{
    // sslrootcert=system needs libpq 16 or later: older libpq looks for a file named "system". On Windows it means
    // OpenSSL's default paths, not the Windows certificate store, which can leave no root to verify against. A host
    // name mismatch isn't matched: the roots worked there.
    const std::string pqopt = MapUtils::GetStringValue(conn_attr, PqoptKey(conn_attr), "");
    const bool root_cert_error = error_message.find("root certificate file \"system\"") != std::string::npos
        || error_message.find("certificate verify failed") != std::string::npos;
    if (pqopt.find(SYSTEM_ROOT_CERT) == std::string::npos || !root_cert_error) {
        return "";
    }
    return "[DSQL] The server certificate couldn't be verified with sslrootcert=system, which needs libpq 16 or "
           "later and, on Windows, OpenSSL's default certificate paths. Set pqopt={sslrootcert=<path to "
           "AmazonRootCA1.pem>} instead.";
}

bool DsqlAuthPlugin::ExpandClusterId(std::map<std::string, std::string>& conn_attr, std::string& out_error)
{
    if (AuthProvider::AuthTypeFromString(MapUtils::GetStringValue(conn_attr, KEY_AUTH_TYPE, "")) != AuthType::DSQL) {
        return true;
    }
    std::string region;
    for (const char* key : {KEY_SERVER, KEY_IAM_HOST}) {
        const auto it = conn_attr.find(key);
        if (it == conn_attr.end() || !IsClusterId(it->second)) {
            continue;
        }
        if (region.empty()) {
            region = ResolveDsqlRegion(conn_attr);
        }
        if (region.empty()) {
            out_error = "DSQL Authentication failed. Region is required when host is a cluster ID (set '"
                + std::string(KEY_REGION) + "')";
            return false;
        }
        it->second = ToLower(it->second) + ".dsql." + region + ".on.aws";
        LOG(INFO) << "[DSQL] Expanded the cluster ID in " << key << " to " << it->second;
    }
    return true;
}

bool DsqlAuthPlugin::ExpandClusterId(DBC* dbc)
{
    std::string error;
    if (ExpandClusterId(dbc->conn_attr, error)) {
        return true;
    }
    LOG(ERROR) << "[DSQL] " << error;
    ClearError(dbc);
    dbc->err = std::make_unique<ErrInfo>(error.c_str(), ERR_CLIENT_UNABLE_TO_ESTABLISH_CONNECTION);
    return false;
}
