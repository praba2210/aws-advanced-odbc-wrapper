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

#ifndef DSQL_CONNECTION_VALIDATOR_H_
#define DSQL_CONNECTION_VALIDATOR_H_

#include <map>
#include <string>

struct DBC;

// Connect-time checks for settings that can't work with Aurora DSQL. They read the upper-cased connection attributes
// after the BASE_DSN merge.
namespace DsqlConnectionValidator {
    // True when the connection targets Aurora DSQL: RDS_AUTH_TYPE=DSQL, SERVER is an Aurora DSQL endpoint, or
    // DATABASE_DIALECT=AURORA_DSQL (any case). IAM_HOST doesn't count: only the token auth plugins read it, and an RDS
    // token type with a DSQL IAM_HOST is rejected on its own.
    bool IsDsqlTarget(const std::map<std::string, std::string>& conn_attr);

    // Why these settings can't work with Aurora DSQL, or "" when they can. On an Aurora DSQL connection it also logs
    // an INFO line for RDS_AUTH_TYPE=SECRETS_MANAGER and a WARNING for an explicit non-DSQL DATABASE_DIALECT.
    std::string Validate(const std::map<std::string, std::string>& conn_attr);

    // Validate, plus ENABLE_OCC_RETRY=1 and a MAX_CONNECTION_LIFETIME other than 0 needing Aurora DSQL, for
    // RDS_InitializeConnection: on a problem, logs it, sets the 08001 diagnostic on dbc and returns false.
    bool CheckSettings(DBC* dbc);
}  // namespace DsqlConnectionValidator

#endif // DSQL_CONNECTION_VALIDATOR_H_
