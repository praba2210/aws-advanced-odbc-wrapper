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
#include <cstdlib>
#include <initializer_list>
#include <map>

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
    const std::string DEFAULT_DATABASE = "postgres";
    const std::string APPLICATION_NAME = std::string("aurora-dsql-odbc/") + DRIVER_VERSION;

    bool ContainsAny(const std::map<std::string, std::string>& conn_attr, std::initializer_list<std::string> keys)
    {
        return std::ranges::any_of(keys, [&conn_attr](const std::string& key) { return conn_attr.contains(key); });
    }

    // pqopt is a brace-quoted libpq conninfo string. Add each option the user didn't set; keep theirs.
    std::string MergePqopt(std::string pqopt)
    {
        if (pqopt.size() >= 2 && pqopt.front() == '{' && pqopt.back() == '}') {
            pqopt = pqopt.substr(1, pqopt.size() - 2);
        }
        const auto add_if_missing = [&pqopt](const std::string& name, const std::string& value) {
            if (pqopt.find(name) == std::string::npos) {
                pqopt += (pqopt.empty() ? "" : " ") + name + "=" + value;
            }
        };
        add_if_missing("sslrootcert", "system");
        add_if_missing("application_name", APPLICATION_NAME);
        return "{" + pqopt + "}";
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

    // REGION, then the region in a DSQL endpoint, then AWS_REGION / AWS_DEFAULT_REGION (the order most DSQL
    // connectors use), then the AWS_PROFILE region. There is no default region: an empty result fails the connect
    // with "Missing required parameters: Region".
    std::string ResolveDsqlRegion(DBC* dbc)
    {
        std::string region = MapUtils::GetStringValue(dbc->conn_attr, KEY_REGION, "");
        if (region.empty()) {
            region = RdsUtils::GetDsqlRegion(MapUtils::GetStringValue(dbc->conn_attr, KEY_SERVER, ""));
        }
        if (region.empty()) {
            region = GetEnvRegion();
        }
        if (region.empty()) {
            region = AuthProvider::GetRegionForProfile(MapUtils::GetStringValue(dbc->conn_attr, KEY_AWS_PROFILE, ""));
        }
        return region;
    }

    std::shared_ptr<AuthProvider> CreateDsqlAuthProvider(DBC* dbc, const std::shared_ptr<AuthProvider>& auth_provider)
    {
        if (auth_provider) {
            return auth_provider;
        }
        const std::string profile = MapUtils::GetStringValue(dbc->conn_attr, KEY_AWS_PROFILE, "");
        return std::make_shared<DsqlAuthProvider>(ResolveDsqlRegion(dbc), profile);
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
    return ResolveDsqlRegion(dbc);
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

void DsqlAuthPlugin::ApplyConnectionDefaults(std::map<std::string, std::string>& conn_attr)
{
    // Aurora DSQL rejects plaintext. Always verify the server, as the Go, .NET, PHP, Ruby and Rust connectors do;
    // only the root certificate stays user-settable, through pqopt.
    const std::string requested_mode = MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE,
        MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE_SHORT, ""));
    if (!requested_mode.empty() && RDS_STR_UPPER(requested_mode) != RDS_STR_UPPER(SSLMODE_VERIFY_FULL)) {
        LOG(WARNING) << "[DSQL] Overriding SSLmode=" << requested_mode << " with " << SSLMODE_VERIFY_FULL;
    }
    conn_attr.erase(PG_KEY_SSLMODE_SHORT);
    conn_attr.insert_or_assign(PG_KEY_SSLMODE, SSLMODE_VERIFY_FULL);

    if (!ContainsAny(conn_attr, {KEY_DB_USERNAME, ALIAS_KEY_USERNAME_1, ALIAS_KEY_USERNAME_2})) {
        conn_attr.insert_or_assign(KEY_DB_USERNAME, DsqlAuthProvider::ADMIN_USER);
    }
    if (!ContainsAny(conn_attr, {KEY_DATABASE, PG_KEY_DATABASE_SHORT})) {
        conn_attr.insert_or_assign(KEY_DATABASE, DEFAULT_DATABASE);
    }

    const std::string& pqopt_key =
        (!conn_attr.contains(PG_KEY_PQOPT) && conn_attr.contains(PG_KEY_PQOPT_SHORT)) ? PG_KEY_PQOPT_SHORT : PG_KEY_PQOPT;
    conn_attr.insert_or_assign(pqopt_key, MergePqopt(MapUtils::GetStringValue(conn_attr, pqopt_key, "")));
}
