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

#include <cstdlib>

#include "../../util/connection_string_keys.h"
#include "../../util/dsql_auth_provider.h"
#include "../../util/map_utils.h"
#include "../../util/rds_utils.h"

namespace {
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
