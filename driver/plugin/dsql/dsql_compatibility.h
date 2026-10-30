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

#ifndef DSQL_COMPATIBILITY_H_
#define DSQL_COMPATIBILITY_H_

#include "../../util/windows_headers.h"

#include <sql.h>

#include <map>
#include <optional>
#include <string>

struct DBC;
struct STMT;

// What the wrapper changes so that psqlODBC works on Aurora DSQL. Everything here applies only to connections
// DsqlConnectionValidator::IsDsqlTarget calls Aurora DSQL.
namespace DsqlCompatibility {
    // True when the connection targets Aurora DSQL. False before the first connect, when conn_attr is still empty.
    bool IsDsql(const DBC* dbc);

    // Sets the psqlODBC options Aurora DSQL needs in the upper-cased connection attributes. A value the user set is
    // replaced, with a warning when it differs. Public for tests.
    void ApplyBaseDriverOptions(std::map<std::string, std::string>& conn_attr);

    // Called by DefaultPlugin::Connect before it builds the base driver's connection string: sets the forced options
    // and drops psqlODBC attributes set before connect that would undo them. Does nothing unless the connection
    // targets Aurora DSQL.
    void PrepareConnect(DBC* dbc);

    // SQLSetConnectAttr on an Aurora DSQL connection, for the attributes that would undo a forced psqlODBC option or
    // make psqlODBC send a statement Aurora DSQL rejects: the result to return without calling psqlODBC, or nullopt to
    // call it as usual.
    std::optional<SQLRETURN> TrySetConnectAttr(DBC* dbc, SQLINTEGER attribute, SQLPOINTER value);

    // SQLSetStmtAttr on an Aurora DSQL connection: a non-zero SQL_ATTR_QUERY_TIMEOUT becomes 0 with 01S02, without
    // calling psqlODBC. nullopt for anything else.
    std::optional<SQLRETURN> TrySetStmtAttr(STMT* stmt, SQLINTEGER attribute, SQLPOINTER value);
}  // namespace DsqlCompatibility

#endif // DSQL_COMPATIBILITY_H_
