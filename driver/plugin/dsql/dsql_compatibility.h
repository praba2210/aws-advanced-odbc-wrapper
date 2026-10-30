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

#include <map>
#include <string>

struct DBC;

// What the wrapper changes so that psqlODBC works on Aurora DSQL. Everything here applies only to connections
// DsqlConnectionValidator::IsDsqlTarget calls Aurora DSQL.
namespace DsqlCompatibility {
    // True when the connection targets Aurora DSQL. False before the first connect, when conn_attr is still empty.
    bool IsDsql(const DBC* dbc);

    // Sets the psqlODBC options Aurora DSQL needs in the upper-cased connection attributes. A value the user set is
    // replaced, with a warning when it differs. Public for tests.
    void ApplyBaseDriverOptions(std::map<std::string, std::string>& conn_attr);

    // Called by DefaultPlugin::Connect before it builds the base driver's connection string. Does nothing unless the
    // connection targets Aurora DSQL.
    void PrepareConnect(DBC* dbc);
}  // namespace DsqlCompatibility

#endif // DSQL_COMPATIBILITY_H_
