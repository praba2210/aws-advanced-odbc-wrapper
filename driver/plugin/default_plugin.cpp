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

#include "default_plugin.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "../driver.h"
#include "../odbcapi.h"
#include "../odbcapi_rds_helper.h"
#include "../util/connection_string_helper.h"
#include "../util/logger_wrapper.h"
#include "../util/odbc_helper.h"
#include "../util/plugin_service.h"
#include "../util/rds_lib_loader.h"
#include "../util/secure_wipe.h"
#include "../util/sql_query_analyzer.h"
#include "dsql/dsql_compatibility.h"

namespace {
    // psqlODBC builds its output connection string in a MAX_CONNECT_STRING (4096) byte buffer, so a buffer of
    // this many characters always holds what it writes, and psqlODBC never reports it truncated.
    constexpr SQLSMALLINT BASE_DRIVER_OUT_CONN_STR_LEN = 4096;
}  // namespace

DefaultPlugin::DefaultPlugin(DBC *dbc) : DefaultPlugin(dbc, nullptr) {}

DefaultPlugin::DefaultPlugin(DBC *dbc, std::shared_ptr<BasePlugin> next_plugin) {
    this->plugin_name_ = "DefaultPlugin";
    this->odbc_helper_ = dbc->plugin_service->GetOdbcHelper();
}

SQLRETURN DefaultPlugin::Connect(
    SQLHDBC        ConnectionHandle,
    SQLHWND        WindowHandle,
    SQLTCHAR *     OutConnectionString,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr,
    SQLUSMALLINT   DriverCompletion)
{
    LOG(INFO) << "Entering Connect";
    SQLRETURN ret = SQL_ERROR;
    bool has_conn_attr_errors = false;
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    const ENV* env = dbc->env;

    // TODO - Should a new connect use a new underlying DBC?
    // Create Wrapped DBC if not already allocated
    RdsLibResult res;
    if (!dbc->wrapped_dbc) {
        if (!env->wrapped_env) {
            LOG(ERROR) << "Unable to allocate underlying DBC, underlying ENV nulled";
            ClearError(dbc);
            dbc->err = std::make_unique<ErrInfo>("Unable to allocate underlying DBC, underlying ENV nulled", ERR_UNDERLYING_HANDLE_NULL);
            return SQL_ERROR;
        }
        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLAllocHandle>(env->driver_lib_loader, RdsFuncNames::AllocHandle,
            SQL_HANDLE_DBC, env->wrapped_env, &dbc->wrapped_dbc
        );
        if (!SQL_SUCCEEDED(res.fn_result) || !dbc->wrapped_dbc) {
            LOG(ERROR) << "Unable to allocate underlying DBC";
            ClearError(dbc);
            dbc->err = std::make_unique<ErrInfo>("Unable to allocate underlying DBC", ERR_SQLALLOCHANDLE_ON_SQL_HANDLE_DBC_FAILED);
            return SQL_ERROR;
        }
    }

    // Apply pre-connect attributes to the wrapped DBC before SQLDriverConnect.
    // Attributes like SQL_ATTR_LOGIN_TIMEOUT must be set before connecting to take effect.
    for (auto const& [key, val] : dbc->attr_map) {
        if (key == SQL_ATTR_LOGIN_TIMEOUT || key == SQL_ATTR_CONNECTION_TIMEOUT) {
            RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetConnectAttr>(env->driver_lib_loader, RdsFuncNames::SetConnectAttr,
                dbc->wrapped_dbc, key, val.first, val.second
            );
        }
    }

    // Keep psqlODBC from doing what Aurora DSQL can't handle, and clear the connection's age, which counts from here.
    // Does nothing on other connections.
    const std::optional<DsqlConnectionLifetime::Instant> dsql_started = DsqlCompatibility::PrepareConnect(dbc);

    // DSN should be read from the original input
    // and a new connection string should be built without DSN & Driver
    // It holds the password or token in PWD, as do the converted copies below. This string, its UTF-16 copy and the
    // wide copy of the 4-byte retry are wiped when they go; the converters' own temporary copies (the by-value
    // argument, ICU's buffer, a vector they grow) are not.
    std::string conn_in = ConnectionStringHelper::BuildMinimumConnectionString(dbc->conn_attr);
    const ScopedWipe wipe_conn_in(conn_in);
    DLOG(INFO) << "Built minimum connection string for underlying driver: " << ConnectionStringHelper::MaskSensitiveInformation(conn_in);
    SQLTCHAR *conn_in_sqltchar;
#if UNICODE
    std::vector<uint16_t> conn_in_vec = ConvertUTF8ToUTF16(conn_in);
    const ScopedWipe wipe_conn_in_vec(conn_in_vec);
    const uint16_t* conn_in_ushort = conn_in_vec.data();

    conn_in_sqltchar = const_cast<SQLTCHAR *>(reinterpret_cast<const SQLTCHAR *>(conn_in_ushort));
#else
    conn_in_sqltchar = const_cast<SQLTCHAR *>(reinterpret_cast<const SQLTCHAR *>(conn_in.c_str()));
#endif
    // psqlODBC writes its own output connection string, PWD included, into the buffer it gets, and its length, and may
    // warn that it doesn't fit. The application gets the wrapper's string and its length from RDS_SQLDriverConnect, so
    // the base driver writes into a buffer and a length of the wrapper's (4 bytes a character, for a base driver with a
    // 4-byte SQLWCHAR); the application's buffer starts as an empty string. Without an application buffer, the base
    // driver gets none and no buffer length: psqlODBC's Unicode driver before 17.00.0005 would cut its string to that
    // length in a buffer of its own and report 01004.
    std::vector<uint32_t> base_out_conn_str;
    // It gets PWD, the password or token: wiped on every return, before it is freed.
    const ScopedWipe wipe_base_out_conn_str(base_out_conn_str);
    SQLSMALLINT base_out_len = 0;
    SQLTCHAR* out_conn_str = nullptr;
    SQLSMALLINT out_buffer_len = 0;
    if (OutConnectionString) {
        if (BufferLength > 0) {
#if UNICODE
            const size_t char_size = env->use_4_bytes_user_app.load() ? sizeof(uint32_t) : sizeof(SQLTCHAR);
#else
            const size_t char_size = sizeof(SQLTCHAR);
#endif
            std::memset(OutConnectionString, 0, char_size);
        }
        base_out_conn_str.resize(static_cast<size_t>(BASE_DRIVER_OUT_CONN_STR_LEN), 0);
        out_conn_str = reinterpret_cast<SQLTCHAR*>(base_out_conn_str.data());
        out_buffer_len = BASE_DRIVER_OUT_CONN_STR_LEN;
    }
    res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLDriverConnect>(env->driver_lib_loader, RdsFuncNames::DriverConnect,
        dbc->wrapped_dbc, WindowHandle, conn_in_sqltchar, SQL_NTS, out_conn_str, out_buffer_len, &base_out_len, DriverCompletion
    );

    if (res.fn_load_success) {
        ret = res.fn_result;
        if (!SQL_SUCCEEDED(ret)) {
#if UNICODE
            if (dbc->wrapped_dbc) {
                SQLINTEGER native_error;
                SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
                SQLTCHAR text[MAX_MSG_LEN * 2] = {0};
                SQLSMALLINT len;

                RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagRec>(env->driver_lib_loader, RdsFuncNames::GetDiagRec,
                    SQL_HANDLE_DBC, dbc->wrapped_dbc, 1, state, &native_error, text, MAX_MSG_LEN, &len
                );

                for (int i = MAX_SQL_STATE_LEN; i < MAX_SQL_STATE_LEN * 2; i++) {
                    if (state[i] != '\0') {
                        this->odbc_helper_->SetUse4BytesBaseDriver(true);
                    }
                }

                if (this->odbc_helper_->GetUse4BytesBaseDriver()) {
                    // Try connecting again with 4-byte characters
                    std::wstring wide_conn = ConvertUTF8ToWString(conn_in);
                    const ScopedWipe wipe_wide_conn(wide_conn);
                    conn_in_sqltchar = const_cast<SQLTCHAR *>(reinterpret_cast<const SQLTCHAR *>(wide_conn.c_str()));
                    res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLDriverConnect>(env->driver_lib_loader, RdsFuncNames::DriverConnect,
                        dbc->wrapped_dbc, WindowHandle, conn_in_sqltchar, SQL_NTS, out_conn_str, out_buffer_len, &base_out_len, DriverCompletion
                    );
                    ret = res.fn_result;
                }
            }
            if (!SQL_SUCCEEDED(ret)) {
                return ret;
            }
#else
            return ret;
#endif
        }
    }

    // Apply Tracked Connection Attributes
    for (auto const& [key, val] : dbc->attr_map) {
        if (key == SQL_ATTR_LOGIN_TIMEOUT || key == SQL_ATTR_CONNECTION_TIMEOUT) {
            continue;
        }
#if UNICODE
        // A string value is kept in the app's characters (RDS_SQLSetConnectAttr); the base driver gets its own.
        if (val.first && OdbcHelper::IsStringConnectAttr(key) && this->odbc_helper_->NeedsConversion()) {
            const auto converted = this->odbc_helper_->ConvertInput(static_cast<SQLTCHAR*>(val.first), SQL_NTS);
            res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetConnectAttr>(env->driver_lib_loader, RdsFuncNames::SetConnectAttr,
                dbc->wrapped_dbc, key, converted.tchar_ptr, SQL_NTS
            );
        } else
#endif
        {
            res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetConnectAttr>(env->driver_lib_loader, RdsFuncNames::SetConnectAttr,
                dbc->wrapped_dbc, key, val.first, val.second
            );
        }
        if (!SQL_SUCCEEDED(res.fn_result)) {
            LOG(WARNING) << "Error setting connection attribute: " << key;
            has_conn_attr_errors  = true;
        }
    }
    dbc->transaction_status = dbc->auto_commit ? TRANSACTION_CLOSED : TRANSACTION_OPEN;

    // TODO - Error Handling for ConnAttr, IsConnected
    // Successful Connection, but bad environment and/or connection attribute setting
    if (SQL_SUCCEEDED(ret)) {
        dbc->conn_status = CONN_CONNECTED;
        DsqlCompatibility::ConnectSucceeded(dbc, dsql_started);
        if (has_conn_attr_errors) {
            ret = SQL_SUCCESS_WITH_INFO;
        }

        if (!dbc->conn_attr.contains(KEY_MONITORING_CONN_UUID)) {
            dbc->plugin_service->UpdateDialect(dbc);
        }
    }
    return ret;
}

SQLRETURN DefaultPlugin::Execute(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     StatementText,
    SQLINTEGER     TextLength)
{
    LOG(INFO) << "Entering Execute";
    RdsLibResult res;
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;
    const std::string query = StatementText ? AS_UTF8_CSTR(StatementText) : "";

    // Allocate wrapped handle if NULL
    if (!stmt->wrapped_stmt) {
        if (dbc->wrapped_dbc) {
            // Stored through dbc->base_handles, as SQLCancel reads it without the statement's lock
            SQLHSTMT wrapped_stmt = SQL_NULL_HSTMT;
            res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLAllocHandle>(env->driver_lib_loader, RdsFuncNames::AllocHandle,
                SQL_HANDLE_STMT, dbc->wrapped_dbc, &wrapped_stmt
            );
            OdbcHelper::SetWrappedStmt(stmt, wrapped_stmt);
            if (!SQL_SUCCEEDED(res.fn_result) || !stmt->wrapped_stmt) {
                LOG(ERROR) << "Unable to allocate underlying STMT";
                ClearError(stmt);
                stmt->err = std::make_unique<ErrInfo>("Unable to allocate underlying STMT", ERR_UNDERLYING_HANDLE_NULL);
                return SQL_ERROR;
            }
        } else {
            LOG(ERROR) << "Unable to use STMT, underlying DBC nulled";
            stmt->err = std::make_unique<ErrInfo>("Unable to use STMT, underlying DBC nulled", ERR_UNDERLYING_HANDLE_NULL);
            return SQL_ERROR;
        }
        // Set statement settings
        for (auto const& [key, val] : stmt->attr_map) {
            res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetStmtAttr>(env->driver_lib_loader, RdsFuncNames::SetStmtAttr,
                stmt->wrapped_stmt, key, val.first, val.second
            );
        }
        // and the explicit ARD and APD the app set, through base descriptors on this base connection: releasing the
        // connection's base handles dropped theirs.
        for (const auto& [attribute, desc] : {
                 std::pair{SQL_ATTR_APP_ROW_DESC, stmt->explicit_row_desc},
                 std::pair{SQL_ATTR_APP_PARAM_DESC, stmt->explicit_param_desc},
             }) {
            if (desc) {
                res = OdbcHelper::SetWrappedAppDesc(env->driver_lib_loader, stmt, attribute, desc);
                if (!SQL_SUCCEEDED(res.fn_result)) {
                    LOG(WARNING) << "Unable to set the statement's explicit descriptor " << attribute;
                }
            }
        }
        // Cursor Name
        const std::string cursor_name = stmt->cursor_name;
#if UNICODE
        const std::vector<uint16_t> cursor_name_vector = ConvertUTF8ToUTF16(cursor_name);
        SQLTCHAR* cursor_name_sqltchar = const_cast<SQLTCHAR *>(reinterpret_cast<const SQLTCHAR *>(cursor_name_vector.data()));
        // Its length in UTF-16 units: the UTF-8 one counts bytes.
        const auto cursor_name_length = static_cast<SQLSMALLINT>(cursor_name_vector.size() - 1);
        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetCursorName>(env->driver_lib_loader, RdsFuncNames::SetCursorName,
            stmt->wrapped_stmt, cursor_name_sqltchar, cursor_name_length
        );
#else
        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetCursorName>(env->driver_lib_loader, RdsFuncNames::SetCursorName,
            stmt->wrapped_stmt, AS_SQLTCHAR(cursor_name), cursor_name.length()
        );
#endif
    }

    // SQLCancel came from another thread before the statement reached the base driver (while the call waited for
    // the connection, or in a plugin): don't run it. From here a cancel goes to the base driver.
    if (!stmt->cancel.EnterBaseDriver()) {
        return RDS_StatementCanceled(stmt);
    }

    if (query.empty()) {
        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLExecute>(env->driver_lib_loader, RdsFuncNames::Execute,
            stmt->wrapped_stmt
        );
    } else {
        const SQLHSTMT wrapped_stmt = stmt->wrapped_stmt;
        res = this->odbc_helper_->ExecDirect(&wrapped_stmt, query);
    }
    stmt->cancel.LeaveBaseDriver();

    // Supports checking for transaction changes only if it was a direct execute
    if (SQL_SUCCEEDED(res.fn_result) && !query.empty()) {
        if (SqlQueryAnalyzer::DoesOpenTransaction(query)) {
            dbc->transaction_status = TRANSACTION_OPEN;
        } else if (SqlQueryAnalyzer::DoesCloseTransaction(dbc, query)
            || SqlQueryAnalyzer::DoesSwitchAutoCommitFalseTrue(dbc, query)
        ) {
            dbc->transaction_status = TRANSACTION_CLOSED;
        }

        if (SqlQueryAnalyzer::IsStatementSettingAutoCommit(query)) {
            dbc->auto_commit = SqlQueryAnalyzer::GetAutoCommitValueFromSqlStatement(query);
            RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetConnectAttr>(env->driver_lib_loader, RdsFuncNames::SetConnectAttr,
                dbc->wrapped_dbc, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(dbc->auto_commit), 0
            );
            dbc->attr_map.insert_or_assign(SQL_ATTR_AUTOCOMMIT, std::make_pair(reinterpret_cast<SQLPOINTER>(dbc->auto_commit), 0));
        }
    }
    return res.fn_result;
}
