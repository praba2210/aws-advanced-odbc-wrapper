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

#include "dsql_compatibility.h"

#include <array>
#include <initializer_list>
#include <map>
#include <string>

#include "../../driver.h"
#include "../../util/dsql_connection_validator.h"
#include "../../util/logger_wrapper.h"

namespace {
    // A psqlODBC option the wrapper sets on every Aurora DSQL connection. key and short_key are psqlODBC's keywords,
    // upper-cased as in conn_attr; they aren't wrapper keys, so they reach the base driver.
    struct ForcedOption {
        const char* name;  // psqlODBC's spelling, for the log line
        const char* key;
        const char* short_key;
        const char* value;
        const char* reason;
    };

    constexpr std::array FORCED_OPTIONS = {
        // No named plans, so no DEALLOCATE when a prepared statement is freed or prepared again. psqlODBC wraps that
        // DEALLOCATE in SAVEPOINT (before 18.00.0003 at every rollback level, since then at every level but 0) and
        // ignores its error. Aurora DSQL rejects SAVEPOINT, which aborts the transaction, and the COMMIT after it
        // reports success without committing anything.
        ForcedOption{.name = "UseServerSidePrepare", .key = "USESERVERSIDEPREPARE", .short_key = "C8", .value = "0",
                     .reason = "freeing a prepared statement inside a transaction can silently roll the transaction "
                               "back on Aurora DSQL"},
    };

    // Writes the option under its long keyword and removes the short one. The base driver's connection string is
    // sorted by key and psqlODBC reads it from left to right, so the long keyword also wins over the CX flags. Logs
    // when it replaces a different value.
    void Force(std::map<std::string, std::string>& conn_attr, const ForcedOption& option)
    {
        for (const char* key : {option.key, option.short_key}) {
            const auto it = conn_attr.find(key);
            if (it == conn_attr.end()) {
                continue;
            }
            if (it->second != option.value) {
                LOG(WARNING) << "[DSQL] Overriding " << option.name << "=" << it->second << " with " << option.value
                             << ": " << option.reason;
            }
            conn_attr.erase(it);
        }
        conn_attr.emplace(option.key, option.value);
    }
}  // namespace

bool DsqlCompatibility::IsDsql(const DBC* dbc)
{
    return dbc != nullptr && DsqlConnectionValidator::IsDsqlTarget(dbc->conn_attr);
}

void DsqlCompatibility::ApplyBaseDriverOptions(std::map<std::string, std::string>& conn_attr)
{
    for (const ForcedOption& option : FORCED_OPTIONS) {
        Force(conn_attr, option);
    }
}

void DsqlCompatibility::PrepareConnect(DBC* dbc)
{
    if (!IsDsql(dbc)) {
        return;
    }
    ApplyBaseDriverOptions(dbc->conn_attr);
}
