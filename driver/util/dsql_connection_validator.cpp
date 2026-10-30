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

#include "dsql_connection_validator.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>

#include "../dialect/dialect.h"
#include "../driver.h"
#include "../error.h"
#include "connection_string_keys.h"
#include "logger_wrapper.h"
#include "map_utils.h"
#include "rds_strings.h"
#include "rds_utils.h"

namespace {
    // The plugins that need Aurora or RDS features Aurora DSQL doesn't have, in the plugin builder's order: Limitless
    // routers, cluster topology (failover, read/write splitting), reader endpoints (simple read/write splitting), the
    // RDS API (custom endpoints), Blue/Green metadata. The Aurora initial connection strategy isn't here: it steps
    // aside for any host that isn't an Aurora cluster endpoint.
    constexpr std::array<const char*, 6> AURORA_ONLY_PLUGIN_KEYS = {
        KEY_ENABLE_LIMITLESS,
        KEY_ENABLE_FAILOVER,
        KEY_ENABLE_SRW_SPLIT,
        KEY_ENABLE_RW_SPLIT,
        KEY_ENABLE_CUSTOM_ENDPOINT,
        KEY_ENABLE_BLUE_GREEN,
    };

    // The auth types whose plugins sign an RDS IAM token, which Aurora DSQL rejects.
    constexpr std::array<std::string_view, 4> RDS_TOKEN_AUTH_TYPES = {
        VALUE_AUTH_IAM,
        VALUE_AUTH_ADFS,
        VALUE_AUTH_OKTA,
        VALUE_AUTH_SSO,
    };

    // Why the connection counts as Aurora DSQL, or "" when it doesn't. The auth type comes first, so the reason
    // doesn't depend on whether a cluster ID in SERVER was expanded yet.
    std::string_view DsqlReason(const std::map<std::string, std::string>& conn_attr)
    {
        if (RDS_STR_UPPER(MapUtils::GetStringValue(conn_attr, KEY_AUTH_TYPE, "")) == VALUE_AUTH_DSQL) {
            return "RDS_AUTH_TYPE=DSQL";
        }
        if (RdsUtils::IsDsqlDns(MapUtils::GetStringValue(conn_attr, KEY_SERVER, ""))) {
            return "SERVER is an Aurora DSQL endpoint";
        }
        if (RDS_STR_UPPER(MapUtils::GetStringValue(conn_attr, KEY_DATABASE_DIALECT, ""))
            == VALUE_DB_DIALECT_AURORA_DSQL) {
            return "DATABASE_DIALECT=AURORA_DSQL";
        }
        return "";
    }

    // The key holding an Aurora DSQL endpoint that a token auth plugin would send its token to (SERVER) or sign it
    // for (IAM_HOST), or "".
    std::string_view DsqlHostKey(const std::map<std::string, std::string>& conn_attr)
    {
        if (RdsUtils::IsDsqlDns(MapUtils::GetStringValue(conn_attr, KEY_SERVER, ""))) {
            return KEY_SERVER;
        }
        if (RdsUtils::IsDsqlDns(MapUtils::GetStringValue(conn_attr, KEY_IAM_HOST, ""))) {
            return KEY_IAM_HOST;
        }
        return "";
    }
}  // namespace

bool DsqlConnectionValidator::IsDsqlTarget(const std::map<std::string, std::string>& conn_attr)
{
    return !DsqlReason(conn_attr).empty();
}

std::string DsqlConnectionValidator::Validate(const std::map<std::string, std::string>& conn_attr)
{
    // An RDS token can't work when it goes to, or is signed for, an Aurora DSQL endpoint, whatever the dialect.
    const std::string auth_type = RDS_STR_UPPER(MapUtils::GetStringValue(conn_attr, KEY_AUTH_TYPE, ""));
    if (std::ranges::any_of(RDS_TOKEN_AUTH_TYPES, [&auth_type](std::string_view type) { return auth_type == type; })) {
        if (const std::string_view host_key = DsqlHostKey(conn_attr); !host_key.empty()) {
            return "RDS_AUTH_TYPE=" + auth_type + " doesn't work with Aurora DSQL: it signs an RDS IAM token, which "
                   "Aurora DSQL rejects (" + std::string(host_key) + " is an Aurora DSQL endpoint). Use "
                   "RDS_AUTH_TYPE=DSQL, which signs an Aurora DSQL token with the credentials of AWS_PROFILE or the "
                   "default credential chain; for an AWS IAM Identity Center (SSO) profile, run 'aws sso login' first.";
        }
    }

    const std::string_view reason = DsqlReason(conn_attr);
    if (reason.empty()) {
        return "";
    }
    if (auth_type == VALUE_AUTH_SECRETS) {
        LOG(INFO) << "[DSQL] With RDS_AUTH_TYPE=SECRETS_MANAGER, the secret's password must be a current Aurora DSQL "
                     "token: DSQL accepts no other password. RDS_AUTH_TYPE=DSQL generates the token instead.";
    }
    const std::string dialect = RDS_STR_UPPER(MapUtils::GetStringValue(conn_attr, KEY_DATABASE_DIALECT, ""));
    if (const DatabaseDialectType type = Dialect::DatabaseDialectFromString(dialect);
        type != DatabaseDialectType::UNKNOWN_DIALECT && type != DatabaseDialectType::AURORA_DSQL) {
        LOG(WARNING) << "[DSQL] DATABASE_DIALECT=" << dialect << " on an Aurora DSQL connection (" << reason
                     << "): the driver uses that dialect, which expects Aurora or RDS topology. Remove "
                        "DATABASE_DIALECT or set it to AURORA_DSQL.";
    }

    // Enabled exactly as the plugin builder enables them: the value 1.
    std::string enabled_keys;
    for (const char* key : AURORA_ONLY_PLUGIN_KEYS) {
        if (MapUtils::GetStringValue(conn_attr, key, "") == VALUE_BOOL_TRUE) {
            enabled_keys += enabled_keys.empty() ? "" : ", ";
            enabled_keys += key;
        }
    }
    if (enabled_keys.empty()) {
        return "";
    }
    return "Aurora DSQL doesn't support these plugins, which need Aurora or RDS cluster features: " + enabled_keys
           + ". Set them to 0. The driver treats this connection as Aurora DSQL because " + std::string(reason) + ".";
}

bool DsqlConnectionValidator::CheckSettings(DBC* dbc)
{
    const std::string error = Validate(dbc->conn_attr);
    if (error.empty()) {
        return true;
    }
    LOG(ERROR) << "[DSQL] " << error;
    ClearError(dbc);
    dbc->err = std::make_unique<ErrInfo>(error.c_str(), ERR_CLIENT_UNABLE_TO_ESTABLISH_CONNECTION);
    return false;
}
