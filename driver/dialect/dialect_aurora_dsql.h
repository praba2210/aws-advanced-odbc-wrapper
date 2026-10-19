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

#ifndef DIALECT_AURORA_DSQL_H
#define DIALECT_AURORA_DSQL_H

#include <algorithm>
#include <string>
#include <vector>

#include "dialect.h"

// Aurora DSQL has no instance topology. The topology queries stay empty, so PluginService uses the plain
// HostListProvider and never starts a topology monitor.
class DialectAuroraDsql : public Dialect {
public:
    int GetDefaultPort() override { return DEFAULT_DSQL_PORT; };
    DatabaseDialectType GetDialectType() override { return DatabaseDialectType::AURORA_DSQL; };

    bool IsSqlStateAccessError(const char* sql_state) override {
        const std::string state(sql_state);
        return std::ranges::any_of(ACCESS_ERRORS, [&state](const std::string &prefix) {
            return state.starts_with(prefix);
        });
    };

    bool IsSqlStateAccessError(const char* sql_state, const std::string& error_message) override {
        if (IsSqlStateAccessError(sql_state)) {
            return true;
        }
        // psqlODBC reports every failed libpq connect as 08001 (connection exception) with the server's message,
        // so a rejected token is only recognizable from the message.
        return !error_message.empty()
            && (error_message.find("unable to accept connection, access denied") != std::string::npos
                || error_message.find("PAM authentication failed") != std::string::npos
                || error_message.find("password authentication failed") != std::string::npos);
    };

    bool IsSqlStateNetworkError(const char* sql_state) override {
        const std::string state(sql_state);
        return std::ranges::any_of(NETWORK_ERRORS, [&state](const std::string &prefix) {
            return state.starts_with(prefix);
        });
    };

private:
    const int DEFAULT_DSQL_PORT = 5432;

    // Same classes as Aurora PostgreSQL. A rejected DSQL token arrives as 08001 with "access denied" in the message.
    const std::vector<std::string> ACCESS_ERRORS = {
        "28P01",
        "28000",
    };

    const std::vector<std::string> NETWORK_ERRORS = {
        "53",       // insufficient resources
        "57P01",    // admin shutdown
        "57P02",    // crash shutdown
        "57P03",    // cannot connect now
        "58",       // system error (backend)
        "08",       // connection error
        "99",       // unexpected error
        "F0",       // configuration file error (backend)
        "XX",       // internal error (backend)
    };
};

#endif  // DIALECT_AURORA_DSQL_H
