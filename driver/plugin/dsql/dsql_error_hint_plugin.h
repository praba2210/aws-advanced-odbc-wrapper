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

#ifndef DSQL_ERROR_HINT_PLUGIN_H_
#define DSQL_ERROR_HINT_PLUGIN_H_

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "../../driver.h"
#include "../base_plugin.h"

// Logs a hint when a statement fails on an Aurora DSQL limit the driver can't work around: the 60-minute connection
// limit. It looks at a statement only when it fails, and passes the return code and the base driver's diagnostics
// through untouched.
class DsqlErrorHintPlugin : public BasePlugin {
public:
    DsqlErrorHintPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin);

    SQLRETURN Connect(
        SQLHDBC        ConnectionHandle,
        SQLHWND        WindowHandle,
        SQLTCHAR *     OutConnectionString,
        SQLSMALLINT    BufferLength,
        SQLSMALLINT *  StringLengthPtr,
        SQLUSMALLINT   DriverCompletion) override;

    SQLRETURN Execute(
        SQLHSTMT       StatementHandle,
        SQLTCHAR *     StatementText,
        SQLINTEGER     TextLength) override;

    // plugin_head wrapped in this plugin when the connection targets Aurora DSQL, else plugin_head.
    static std::shared_ptr<BasePlugin> AddToChain(DBC* dbc, std::shared_ptr<BasePlugin> plugin_head);

    // The hint for a statement that failed on a closed connection of this age, or "" when none applies. Public for
    // tests.
    static std::string GetConnectionAgeHint(const std::string& sql_state, std::chrono::steady_clock::duration age);

    // Aurora DSQL closes connections at 60 minutes, with jitter. A closed connection from this age on gets the hint.
    static constexpr std::chrono::minutes CONNECTION_AGE_HINT_THRESHOLD{55};

protected:
    // The statement's first diagnostic record, read without consuming it. Virtual for tests.
    virtual void ReadStmtError(STMT* stmt, std::string& sql_state, std::string& message);
    // Whether the base driver reports the connection dead (SQL_ATTR_CONNECTION_DEAD). Virtual for tests.
    virtual bool IsConnectionDead(DBC* dbc);
    // Virtual for tests.
    virtual std::chrono::steady_clock::time_point Now();

private:
    void LogConnectionAgeHint(DBC* dbc, const std::string& sql_state);

    std::optional<std::chrono::steady_clock::time_point> connected_at_;
    bool age_hint_logged_ = false;
};

#endif // DSQL_ERROR_HINT_PLUGIN_H_
