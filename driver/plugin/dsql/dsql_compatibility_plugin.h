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

#ifndef DSQL_COMPATIBILITY_PLUGIN_H_
#define DSQL_COMPATIBILITY_PLUGIN_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "../../driver.h"
#include "../base_plugin.h"

// What the wrapper changes so that psqlODBC works on Aurora DSQL. It sits directly above DefaultPlugin, and only in the
// chain of a connection that targets Aurora DSQL (AddToChain).
class DsqlCompatibilityPlugin : public BasePlugin {
public:
    DsqlCompatibilityPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin);

    // next wrapped in this plugin when the connection targets Aurora DSQL (DsqlConnectionValidator::IsDsqlTarget),
    // else next unchanged.
    static std::shared_ptr<BasePlugin> AddToChain(DBC* dbc, std::shared_ptr<BasePlugin> next);

    // Sets the psqlODBC options Aurora DSQL needs in the upper-cased connection attributes. A value the user set is
    // replaced, with a warning when it differs. Public for tests.
    static void ApplyBaseDriverOptions(std::map<std::string, std::string>& conn_attr);

    // Sets the forced options and drops psqlODBC attributes set before connect that would undo them. With an auth type
    // other than DSQL it also sets the postgres database when none is set and, for an SSLmode psqlODBC would run as
    // disable, the TLS defaults (DsqlAuthPlugin::ApplyTlsDefaults, OTHER_AUTH). A connect that succeeds returns 01S02
    // for that SSLmode and for a dropped isolation level; one that fails puts the attributes back.
    SQLRETURN Connect(
        SQLHDBC        ConnectionHandle,
        SQLHWND        WindowHandle,
        SQLTCHAR *     OutConnectionString,
        SQLSMALLINT    BufferLength,
        SQLSMALLINT *  StringLengthPtr,
        SQLUSMALLINT   DriverCompletion) override;

    // Answers, without calling psqlODBC, the attributes that would undo a forced psqlODBC option or make psqlODBC send
    // a statement Aurora DSQL rejects: the psqlODBC PGOPT attributes, a query timeout and the isolation level. An
    // autocommit change on a connected connection commits first (SQLEndTran); if that fails, it returns the commit's
    // result and leaves autocommit unchanged.
    std::optional<SQLRETURN> TrySetConnectAttr(
        SQLHDBC        ConnectionHandle,
        SQLINTEGER     Attribute,
        SQLPOINTER     ValuePtr,
        SQLINTEGER     StringLength) override;

    // SQL_TXN_ISOLATION_OPTION on a connection whose wrapped handle is allocated: SQL_TXN_REPEATABLE_READ.
    std::optional<SQLRETURN> TryGetInfo(
        SQLHDBC        ConnectionHandle,
        SQLUSMALLINT   InfoType,
        SQLPOINTER     InfoValuePtr,
        SQLSMALLINT    BufferLength,
        SQLSMALLINT *  StringLengthPtr) override;

    // A non-zero SQL_ATTR_QUERY_TIMEOUT becomes 0 with 01S02, without calling psqlODBC.
    std::optional<SQLRETURN> TrySetStmtAttr(
        SQLHSTMT       StatementHandle,
        SQLINTEGER     Attribute,
        SQLPOINTER     ValuePtr,
        SQLINTEGER     StringLength) override;

    // SQL_COMMIT after a statement failed in the transaction: checks with SELECT 1 whether Aurora DSQL ended it. 25P02
    // means it did: rolls it back and returns SQL_ERROR with 25S03. Otherwise the commit goes ahead.
    std::optional<SQLRETURN> TryEndTran(
        SQLHDBC        ConnectionHandle,
        SQLSMALLINT    CompletionType) override;
};

#endif // DSQL_COMPATIBILITY_PLUGIN_H_
