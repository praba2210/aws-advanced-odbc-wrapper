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

#include "dsql_error_hint_plugin.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "../../odbcapi_rds_helper.h"
#include "../../util/dsql_connection_validator.h"
#include "../../util/logger_wrapper.h"
#include "../../util/odbc_helper.h"
#include "../../util/plugin_service.h"
#include "../../util/rds_strings.h"

namespace {
    // Aurora DSQL's 10 MiB message limit closes the connection with its own error: FATAL: invalid message length.
    const std::string MESSAGE_TOO_LARGE_STATE = "08P01";
    // The message buffer of the diagnostic read. psqlODBC splits long messages into records and takes the split size
    // from the first read with a buffer; 512 keeps it at psqlODBC's own default of 511 (DRVMNGRDIV, environ.c), so an
    // app sees the same records as without this read. 511 characters are plenty to match on.
    constexpr SQLSMALLINT DIAG_MESSAGE_CHARS = 512;
    // pg_advisory_lock, pg_advisory_xact_lock, pg_advisory_unlock_all, pg_try_advisory_lock and their variants.
    constexpr std::array<std::string_view, 2> ADVISORY_LOCK_FUNCTION_PREFIXES = {"pg_advisory_", "pg_try_advisory_"};
    // SQLSTATE classes whose errors aren't about the function called: no record, a lost connection, a rolled-back
    // transaction (OCC conflicts included).
    constexpr std::array<std::string_view, 3> NOT_A_FUNCTION_ERROR_CLASSES = {"00", "08", "40"};

    bool NamesAdvisoryLockFunction(std::string text)
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return std::ranges::any_of(ADVISORY_LOCK_FUNCTION_PREFIXES, [&text](std::string_view prefix) {
            return text.find(prefix) != std::string::npos;
        });
    }

    // The statement text the chain got from SQLExecDirect, as UTF-8, to its terminator: DefaultPlugin runs it that way
    // (SQL_NTS), whatever TextLength says. SQLExecute passes none.
    std::string StatementTextToUtf8(SQLTCHAR* text)
    {
        if (text == nullptr) {
            return "";
        }
#if UNICODE
        return ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(text));
#else
        return reinterpret_cast<const char*>(text);
#endif
    }
}  // namespace

DsqlErrorHintPlugin::DsqlErrorHintPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin)
    : BasePlugin(dbc, std::move(next_plugin))
{
    this->plugin_name_ = "DSQL_ERROR_HINT";
}

SQLRETURN DsqlErrorHintPlugin::Connect(
    SQLHDBC        ConnectionHandle,
    SQLHWND        WindowHandle,
    SQLTCHAR *     OutConnectionString,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr,
    SQLUSMALLINT   DriverCompletion)
{
    const SQLRETURN ret = ConnectNext(
        ConnectionHandle, WindowHandle, OutConnectionString, BufferLength, StringLengthPtr, DriverCompletion);
    // A new connection, or none: its age starts now, and it can get the hint once.
    connected_at_.reset();
    if (SQL_SUCCEEDED(ret)) {
        connected_at_ = Now();
    }
    age_hint_logged_ = false;
    return ret;
}

SQLRETURN DsqlErrorHintPlugin::Execute(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     StatementText,
    SQLINTEGER     TextLength)
{
    const SQLRETURN ret = ExecuteNext(StatementHandle, StatementText, TextLength);
    // Only a failed statement is looked at, so a successful one costs nothing here.
    if (ret != SQL_ERROR || StatementHandle == nullptr) {
        return ret;
    }

    STMT* stmt = static_cast<STMT*>(StatementHandle);
    std::string sql_state;
    std::string message;
    ReadStmtError(stmt, sql_state, message);
    if (const std::string_view hint = GetAdvisoryLockHint(sql_state, message,
                                                          StatementTextToUtf8(StatementText));
        !hint.empty()) {
        LOG(WARNING) << hint;
    }
    LogConnectionAgeHint(stmt->dbc, sql_state);
    return ret;
}

std::shared_ptr<BasePlugin> DsqlErrorHintPlugin::AddToChain(DBC* dbc, std::shared_ptr<BasePlugin> plugin_head)
{
    if (!DsqlConnectionValidator::IsDsqlTarget(dbc->conn_attr)) {
        return plugin_head;
    }
    return std::make_shared<DsqlErrorHintPlugin>(dbc, std::move(plugin_head));
}

std::string DsqlErrorHintPlugin::GetConnectionAgeHint(const std::string& sql_state,
                                                      std::chrono::steady_clock::duration age)
{
    if (age < CONNECTION_AGE_HINT_THRESHOLD || sql_state == MESSAGE_TOO_LARGE_STATE) {
        return "";
    }
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(age).count();
    return "[DSQL] The connection failed after " + std::to_string(minutes) + " minutes. This may be Aurora DSQL's "
           "60-minute connection limit. Disconnect and connect again, and recycle pooled connections before "
           + std::to_string(CONNECTION_AGE_HINT_THRESHOLD.count()) + " minutes.";
}

std::string_view DsqlErrorHintPlugin::GetAdvisoryLockHint(const std::string& sql_state, const std::string& message,
                                                          const std::string& statement)
{
    if (sql_state.empty() || std::ranges::any_of(NOT_A_FUNCTION_ERROR_CLASSES, [&sql_state](std::string_view error_class) {
            return sql_state.starts_with(error_class);
        })) {
        return "";
    }
    if (!NamesAdvisoryLockFunction(message) && !NamesAdvisoryLockFunction(statement)) {
        return "";
    }
    return "[DSQL] Aurora DSQL doesn't support advisory locks (pg_advisory_lock and related functions), and the driver "
           "doesn't emulate them. Remove the lock call, or coordinate through a table row instead.";
}

void DsqlErrorHintPlugin::LogConnectionAgeHint(DBC* dbc, const std::string& sql_state)
{
    if (!connected_at_.has_value() || age_hint_logged_) {
        return;
    }
    const std::string hint = GetConnectionAgeHint(sql_state, Now() - connected_at_.value());
    // The dead check comes last, after the record was read, and only for an old connection: psqlODBC's
    // SQLGetConnectAttr clears its connection error, which the statement's record no longer needs once built.
    if (hint.empty() || !IsConnectionDead(dbc)) {
        return;
    }
    LOG(WARNING) << hint;
    age_hint_logged_ = true;
}

void DsqlErrorHintPlugin::ReadStmtError(STMT* stmt, std::string& sql_state, std::string& message)
{
    SQLTCHAR state_buffer[MAX_SQL_STATE_LEN * 2] = { 0 };
    SQLTCHAR message_buffer[DIAG_MESSAGE_CHARS * 2] = { 0 };
    SQLINTEGER native_error = 0;
    SQLSMALLINT text_length = 0;
    // Record 1 through RDS_SQLGetDiagRec, not RDS_SQLError, which would consume it before the app reads it.
    RDS_SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state_buffer, &native_error, message_buffer, DIAG_MESSAGE_CHARS,
        &text_length, true);
#if UNICODE
    sql_state = ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(state_buffer));
    message = ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(message_buffer));
#else
    sql_state = reinterpret_cast<const char*>(state_buffer);
    message = reinterpret_cast<const char*>(message_buffer);
#endif
}

bool DsqlErrorHintPlugin::IsConnectionDead(DBC* dbc)
{
    const std::shared_ptr<OdbcHelper> odbc_helper =
        dbc->plugin_service != nullptr ? dbc->plugin_service->GetOdbcHelper() : nullptr;
    // Without a helper the state is unknown: no hint.
    return odbc_helper != nullptr && odbc_helper->IsClosed(dbc);
}

std::chrono::steady_clock::time_point DsqlErrorHintPlugin::Now()
{
    return std::chrono::steady_clock::now();
}
