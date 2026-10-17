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

#include <map>
#include <memory>
#include <string>

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

    // Applies the Aurora DSQL connection defaults to the upper-cased connection attributes. Public for tests.
    static void ApplyConnectionDefaults(std::map<std::string, std::string>& conn_attr);

protected:
    std::string ResolveRegion(DBC* dbc) override;
};

#endif // DSQL_AUTH_PLUGIN_H_
