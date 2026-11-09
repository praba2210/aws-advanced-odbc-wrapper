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

#include "odbc_helper.h"

#include "../odbcapi_rds_helper.h"

#include "rds_lib_loader.h"
#include "rds_strings.h"

#include <algorithm>
#include <mutex>

OdbcHelper::OdbcHelper(const std::shared_ptr<RdsLibLoader> &lib_loader, const ENV* env) {
    this->lib_loader_ = lib_loader;
    this->env_ = env;
}

void OdbcHelper::Disconnect(DBC* dbc) {
    if (dbc) {
        const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
        ReleaseBaseHandles(this->lib_loader_, dbc, false);
    }
}

void OdbcHelper::InvalidateImplicitDescriptors(STMT* stmt) {
    for (DESC* desc : {stmt->app_row_desc, stmt->app_param_desc, stmt->imp_row_desc, stmt->imp_param_desc}) {
        if (desc) {
            const std::lock_guard<std::recursive_mutex> desc_lock(desc->lock);
            // Owned by the underlying statement detach, don't free.
            desc->wrapped_desc = SQL_NULL_HDESC;
        }
    }
}

void OdbcHelper::SetWrappedStmt(STMT* stmt, SQLHSTMT wrapped_stmt) {
    stmt->dbc->base_handles.Store(stmt->wrapped_stmt, wrapped_stmt);
}

void OdbcHelper::FreeWrappedStmt(const std::shared_ptr<RdsLibLoader>& lib_loader, STMT* stmt) {
    if (const SQLHSTMT wrapped_stmt = stmt->dbc->base_handles.Detach(stmt->wrapped_stmt)) {
        stmt->dbc->base_handles.RunOrDefer([lib_loader, wrapped_stmt] {
            RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeHandle>(lib_loader, RdsFuncNames::FreeHandle,
                SQL_HANDLE_STMT, wrapped_stmt
            );
        });
    }
}

SQLHDBC OdbcHelper::ReleaseBaseHandles(const std::shared_ptr<RdsLibLoader>& lib_loader, DBC* dbc, bool free_base_dbc) {
    // Underlying statements; NULL handles crash some base drivers.
    std::vector<SQLHSTMT> wrapped_stmts;
    const std::list<STMT*> stmt_list = dbc->stmt_list;
    for (STMT* stmt : stmt_list) {
        const std::lock_guard<std::recursive_mutex> stmt_lock(stmt->lock);
        if (const SQLHSTMT wrapped_stmt = dbc->base_handles.Detach(stmt->wrapped_stmt)) {
            wrapped_stmts.push_back(wrapped_stmt);
        }
        InvalidateImplicitDescriptors(stmt);
    }
    // and underlying descriptors
    std::vector<SQLHDESC> wrapped_descs;
    const std::list<DESC*> desc_list = dbc->desc_list;
    for (DESC* desc : desc_list) {
        const std::lock_guard<std::recursive_mutex> desc_lock(desc->lock);
        if (desc->wrapped_desc) {
            wrapped_descs.push_back(desc->wrapped_desc);
            desc->wrapped_desc = SQL_NULL_HDESC;
        }
    }
    const SQLHDBC wrapped_dbc = dbc->wrapped_dbc;
    dbc->wrapped_dbc = SQL_NULL_HDBC;

    dbc->base_handles.RunOrDefer([lib_loader, wrapped_stmts, wrapped_descs, wrapped_dbc, free_base_dbc] {
        try {
            for (const SQLHSTMT wrapped_stmt : wrapped_stmts) {
                RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeHandle>(lib_loader, RdsFuncNames::FreeHandle,
                    SQL_HANDLE_STMT, wrapped_stmt
                );
            }
            for (const SQLHDESC wrapped_desc : wrapped_descs) {
                RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeHandle>(lib_loader, RdsFuncNames::FreeHandle,
                    SQL_HANDLE_DESC, wrapped_desc
                );
            }
            if (wrapped_dbc) {
                const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLDisconnect>(lib_loader,
                    RdsFuncNames::Disconnect, wrapped_dbc
                );
                if (free_base_dbc && SQL_SUCCEEDED(res.fn_result)) {
                    RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeHandle>(lib_loader, RdsFuncNames::FreeHandle,
                        SQL_HANDLE_DBC, wrapped_dbc
                    );
                }
            }
        } catch (const std::exception& ex) {
            LOG(ERROR) << "Exception while releasing the underlying handles of a connection: " << ex.what();
        }
    });
    return wrapped_dbc;
}

RdsLibResult OdbcHelper::AllocWrappedDesc(const std::shared_ptr<RdsLibLoader>& lib_loader, DESC* desc) {
    const std::lock_guard<std::recursive_mutex> desc_lock(desc->lock);
    if (desc->wrapped_desc) {
        return {
            .fn_load_success = true,
            .fn_result = SQL_SUCCESS,
            .fn_name = std::string(RdsFuncNames::AllocHandle),
        };
    }
    if (!desc->dbc->wrapped_dbc) {
        return {
            .fn_load_success = true,
            .fn_result = SQL_ERROR,
            .fn_name = std::string(RdsFuncNames::AllocHandle),
        };
    }
    SQLHDESC wrapped_desc = SQL_NULL_HDESC;
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLAllocHandle>(lib_loader, RdsFuncNames::AllocHandle,
        SQL_HANDLE_DESC, desc->dbc->wrapped_dbc, &wrapped_desc
    );
    if (SQL_SUCCEEDED(res.fn_result)) {
        desc->wrapped_desc = wrapped_desc;
    }
    return res;
}

RdsLibResult OdbcHelper::SetWrappedAppDesc(const std::shared_ptr<RdsLibLoader>& lib_loader, STMT* stmt,
    SQLINTEGER attribute, DESC* desc)
{
    SQLHDESC wrapped_desc = SQL_NULL_HDESC;
    if (desc) {
        const RdsLibResult alloc_res = AllocWrappedDesc(lib_loader, desc);
        if (!SQL_SUCCEEDED(alloc_res.fn_result)) {
            return alloc_res;
        }
        wrapped_desc = desc->wrapped_desc;
    }
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetStmtAttr>(lib_loader, RdsFuncNames::SetStmtAttr,
        stmt->wrapped_stmt, attribute, wrapped_desc, SQL_IS_POINTER
    );
}

void OdbcHelper::Disconnect(SQLHDBC* hdbc) {
    DBC* local_dbc = static_cast<DBC*>(*hdbc);
    Disconnect(local_dbc);
}

void OdbcHelper::DisconnectAndFree(SQLHDBC* hdbc) {
    Disconnect(hdbc);
    RDS_FreeConnect(*hdbc);
    *hdbc = SQL_NULL_HDBC;
}

bool OdbcHelper::IsClosed(SQLHDBC hdbc) {
    const DBC* local_dbc = static_cast<DBC*>(hdbc);
    if (hdbc == SQL_NULL_HDBC || local_dbc->wrapped_dbc == SQL_NULL_HDBC) {
        return true;
    }
    SQLUINTEGER connection_state = SQL_CD_FALSE;
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetConnectAttr>(this->lib_loader_, RdsFuncNames::GetConnectAttr,
        local_dbc->wrapped_dbc, SQL_ATTR_CONNECTION_DEAD, &connection_state, sizeof(SQLUINTEGER), nullptr
    );

    if (SQL_SUCCEEDED(res.fn_result)) {
        return connection_state == SQL_CD_TRUE;
    }

    return true;
}

SQLRETURN OdbcHelper::AllocEnv(SQLHENV* henv) {
    // Not counted toward CoreService, the monitors using it would otherwise keep the count above 0
    return RDS_AllocInternalEnv(henv);
}

// codechecker_suppress [readability-convert-member-functions-to-static]
SQLRETURN OdbcHelper::FreeEnv(SQLHENV* henv) {
    return RDS_FreeEnv(*henv);
}

// codechecker_suppress [readability-convert-member-functions-to-static]
SQLRETURN OdbcHelper::AllocDbc(SQLHENV &henv, SQLHDBC &hdbc) {
    return RDS_AllocDbc(henv, &hdbc);
}

RdsLibResult OdbcHelper::SetEnvAttr(
    const ENV *henv,
    const SQLINTEGER attribute,
    SQLPOINTER pointer,
    const int length)
{
    if (!henv || !henv->wrapped_env) {
        // A NULL wrapped_env would crash the base driver.
        return RdsLibResult { .fn_load_success = false, .fn_result = SQL_ERROR };
    }
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetEnvAttr>(this->lib_loader_, RdsFuncNames::SetEnvAttr,
        henv->wrapped_env, attribute, pointer, length
    );
}

RdsLibResult OdbcHelper::Fetch(SQLHSTMT* stmt)
{
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFetch>(this->lib_loader_, RdsFuncNames::Fetch,
        *stmt
    );
}

RdsLibResult OdbcHelper::BindCol(
    const SQLHSTMT* stmt,
    const int column,
    const int type,
    void* value,
    const size_t size,
    SQLLEN* len)
{
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLBindCol>(this->lib_loader_, RdsFuncNames::BindCol,
        *stmt, column, type, value, size, len
    );
}

RdsLibResult OdbcHelper::ExecDirect(const SQLHSTMT* stmt, const std::string &query) {
#if UNICODE
    if (this->GetUse4BytesBaseDriver()) {
        const std::wstring wide_conn = ConvertUTF8ToWString(query);
        SQLTCHAR* conn_in_sqltchar = const_cast<SQLTCHAR *>(reinterpret_cast<const SQLTCHAR *>(wide_conn.c_str()));
        return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLExecDirect>(this->lib_loader_, RdsFuncNames::ExecDirect,
            *stmt, conn_in_sqltchar, SQL_NTS
        );
    }
    const std::vector<uint16_t> query_vector = ConvertUTF8ToUTF16(query);
    SQLTCHAR* query_sqltchar = const_cast<SQLTCHAR *>(reinterpret_cast<const SQLTCHAR *>(query_vector.data()));
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLExecDirect>(this->lib_loader_, RdsFuncNames::ExecDirect,
        *stmt, query_sqltchar, SQL_NTS
    );
#else
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLExecDirect>(this->lib_loader_, RdsFuncNames::ExecDirect,
        *stmt, AS_SQLTCHAR(query), SQL_NTS
    );
#endif
}

RdsLibResult OdbcHelper::CloseCursor(SQLHSTMT stmt) {
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLCloseCursor>(this->lib_loader_, RdsFuncNames::CloseCursor,
        stmt
    );
}

RdsLibResult OdbcHelper::BaseAllocEnv(ENV* env) {
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLAllocHandle>(this->lib_loader_, RdsFuncNames::AllocHandle,
        SQL_HANDLE_ENV, nullptr, &env->wrapped_env
    );
}

RdsLibResult OdbcHelper::BaseAllocStmt(const SQLHDBC* wrapped_dbc, SQLHSTMT* stmt) {
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLAllocHandle>(this->lib_loader_, RdsFuncNames::AllocHandle,
        SQL_HANDLE_STMT, *wrapped_dbc, stmt
    );
}

RdsLibResult OdbcHelper::BaseFreeStmt(SQLHSTMT* stmt) {
    return RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeHandle>(this->lib_loader_, RdsFuncNames::FreeHandle,
        SQL_HANDLE_STMT, *stmt
    );
}

std::shared_ptr<RdsLibLoader> OdbcHelper::GetLibLoader() {
    return this->lib_loader_;
}

bool OdbcHelper::GetUse4BytesBaseDriver() const {
    return this->use_4_bytes_base_driver_;
}

bool OdbcHelper::GetUse4BytesUserApp() const {
    return this->env_ != nullptr && this->env_->use_4_bytes_user_app.load();
}

void OdbcHelper::SetUse4BytesBaseDriver(const bool use_4_bytes) {
    this->use_4_bytes_base_driver_ = use_4_bytes;
}

// codechecker_suppress [readability-convert-member-functions-to-static]
ConvertedSqltchar OdbcHelper::ConvertInput(SQLTCHAR* in, SQLINTEGER in_length) const {
    ConvertedSqltchar result;
#if UNICODE
    if (!in) {
        // Return as-is if this is a nullptr, let the caller decide how to handle nullptr.
        return result;
    }
    result.data = ConvertUserAppInputToBaseDriver(
        GetUse4BytesUserApp(),
        use_4_bytes_base_driver_,
        in,
        in_length);
    result.tchar_ptr = result.data.data();
#else
    result.tchar_ptr = in;
#endif
    return result;
}

std::string OdbcHelper::GetSqlStateAndLogMessage(DBC* dbc) {
    std::string unused;
    return GetSqlStateAndLogMessage(dbc, unused);
}

std::string OdbcHelper::GetSqlStateAndLogMessage(DBC* dbc, std::string& out_message) {
    SQLSMALLINT stmt_length;
    SQLINTEGER native_error;
    SQLTCHAR sql_state[MAX_SQL_STATE_LEN * 2] = { 0 };
    SQLTCHAR message[MAX_MSG_LEN * 2] = { 0 };
    // Use RDS_SQLGetDiagRec directly instead of RDS_SQLError to avoid consuming the
    // sql_error_called flag. RDS_SQLError sets this flag, which prevents subsequent
    // external SQLError calls from retrieving the diagnostic record.
    RDS_SQLGetDiagRec(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc), 1, sql_state, &native_error, message, MAX_MSG_LEN, &stmt_length, true);
#if UNICODE
    const std::string state_str = ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(sql_state));
    const std::string message_str = ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(message));
#else
    const std::string state_str = reinterpret_cast<const char*>(sql_state);
    const std::string message_str = reinterpret_cast<const char*>(message);
#endif
    LOG(WARNING) << "SQL State: " << state_str << ". Message: " << message_str;
    out_message = message_str;
    return state_str;
}

std::string OdbcHelper::GetStmtErrorMessage(SQLHSTMT stmt) {
    SQLTCHAR sql_state[MAX_SQL_STATE_LEN * 2] = { 0 };
    SQLTCHAR message[MAX_MSG_LEN * 2] = { 0 };
    SQLINTEGER native_error = 0;
    SQLSMALLINT text_length = 0;
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagRec>(lib_loader_, RdsFuncNames::GetDiagRec,
        SQL_HANDLE_STMT, stmt, 1, sql_state, &native_error, message, MAX_MSG_LEN, &text_length);
    if (SQL_SUCCEEDED(res.fn_result)) {
#if UNICODE
        return ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(message));
#else
        return {reinterpret_cast<const char*>(message)};
#endif
    }
    return "";
}

// Convert base driver output to either 2-byte or 4-byte output for the client application
void OdbcHelper::ConvertDriverOutputToTarget(
    const SQLTCHAR* src,
    SQLTCHAR* dst,
    const size_t src_byte_count,
    const size_t dst_byte_count) const
{
    ConvertDriverOutputToTarget(false, src, dst, src_byte_count, dst_byte_count);
}

// codechecker_suppress [readability-convert-member-functions-to-static]
void OdbcHelper::ConvertDriverOutputToTarget(
    const bool wrapper_call,
    const SQLTCHAR* src,
    SQLTCHAR* dst,
    const size_t src_byte_count,
    const size_t dst_byte_count) const
{
    if (dst == nullptr || src == nullptr || dst_byte_count == 0) {
        return;
    }
    // Only the string and its terminator are written: the app's buffer can be shorter than it claims.
#if UNICODE
    const bool user_4_byte = !wrapper_call && GetUse4BytesUserApp();
    const size_t driver_step = use_4_bytes_base_driver_ ? 2 : 1;  // SQLTCHARs per base driver character
    const size_t src_char_count = src_byte_count / sizeof(SQLTCHAR) / driver_step;
    const size_t dst_char_count = user_4_byte
        ? dst_byte_count / sizeof(SQLTCHAR) / 2
        : dst_byte_count / sizeof(SQLTCHAR);
    // The base driver's string, up to its terminator, in its characters
    size_t src_length = 0;
    while (src_length < src_char_count && (src[src_length * driver_step] != 0
        || (driver_step == 2 && src[(src_length * driver_step) + 1] != 0))) {
        src_length++;
    }

    if (user_4_byte == use_4_bytes_base_driver_) {
        // The same character size: copy the string and its terminator as they are.
        const size_t copy_chars = std::min(src_length < src_char_count ? src_length + 1 : src_length, dst_char_count);
        std::memcpy(dst, src, copy_chars * driver_step * sizeof(SQLTCHAR));
    } else if (user_4_byte) {
        // Expand the driver output from 2-byte to 4-byte for the client application.
        ConvertUTF16ToUTF32(src, dst, src_length, dst_char_count * 2);
    } else {
        // Narrow driver output from 4-byte to 2-byte for the client application.
        Convert4To2ByteString(use_4_bytes_base_driver_, const_cast<SQLTCHAR*>(src), dst, dst_char_count);
    }
#else
    size_t src_length = 0;
    while (src_length < src_byte_count && src[src_length] != 0) {
        src_length++;
    }
    const size_t copy_bytes = std::min(src_length < src_byte_count ? src_length + 1 : src_length, dst_byte_count);
    std::memcpy(dst, src, copy_bytes);
#endif
}

// Convert wrapper output to either 2-byte or 4-byte output for the client application
void OdbcHelper::ConvertWrapperOutputToTarget(
    SQLTCHAR* buf,
    const size_t char_count,
    const size_t buf_byte_count) const
{
    ConvertWrapperOutputToTarget(false, buf, char_count, buf_byte_count);
}

void OdbcHelper::ConvertWrapperOutputToTarget(
    const bool wrapper_call,
    SQLTCHAR* buf,
    const size_t char_count,
    const size_t buf_byte_count) const
{
    ConvertWrapperOutputToTarget(GetUse4BytesUserApp(), wrapper_call, buf, char_count, buf_byte_count);
}

// codechecker_suppress [readability-convert-member-functions-to-static]
void OdbcHelper::ConvertWrapperOutputToTarget(
    const bool user_4_byte,
    const bool wrapper_call,
    SQLTCHAR* buf,
    const size_t char_count,
    const size_t buf_byte_count)
{
#if UNICODE
    if (!wrapper_call && user_4_byte) {
        const size_t buf_char_count = buf_byte_count / sizeof(SQLTCHAR);
        ExpandUTF16ToUTF32InPlace(buf, char_count, buf_char_count);
    }
#endif
}

// codechecker_suppress [readability-convert-member-functions-to-static]
bool OdbcHelper::NeedsConversion() const {
#if UNICODE
    return GetUse4BytesUserApp() != use_4_bytes_base_driver_;
#else
    return false;
#endif
}

size_t OdbcHelper::GetTargetByteCount(const bool wrapper_call, const size_t char_count) const {
    const size_t multiplier = !wrapper_call && GetUse4BytesUserApp() ? 2 : 1;
    return char_count * multiplier * sizeof(SQLTCHAR);
}

#if UNICODE
DriverStringResult OdbcHelper::FetchDriverString(
    const std::function<RdsLibResult(SQLTCHAR*, size_t, size_t*)>& call,
    void* app_out,
    const size_t buffer_length,
    const bool count_bytes,
    const size_t max_length,
    const bool wrapper_call) const
{
    DriverStringResult result;
    const bool app_4_byte = !wrapper_call && GetUse4BytesUserApp();
    const size_t driver_step = use_4_bytes_base_driver_ ? 2 : 1;  // SQLTCHARs per base driver character
    const size_t app_unit = count_bytes ? (app_4_byte ? 2 : 1) * sizeof(SQLTCHAR) : 1;
    const size_t driver_unit = count_bytes ? driver_step * sizeof(SQLTCHAR) : 1;
    // The app's room in whole characters, the terminator's included
    const size_t app_chars = app_out ? buffer_length / app_unit : 0;
    const size_t max_room = std::max<size_t>(max_length / driver_unit, 1);

    // As many of the base driver's characters as the app's buffer holds, and room for a terminator at least
    size_t room = std::min(std::max<size_t>(app_chars, 1), max_room);
    std::vector<SQLTCHAR> buffer(room * driver_step, 0);
    size_t full = 0;
    result.res = call(buffer.data(), room * driver_unit, &full);
    full /= driver_unit;
    if (SQL_SUCCEEDED(result.res.fn_result) && full >= room && room < max_room) {
        // Cut: once more with room for all of it, so that the app's buffer is filled and the length counts it whole
        room = std::min(full + 1, max_room);
        buffer.assign(room * driver_step, 0);
        full = 0;
        result.res = call(buffer.data(), room * driver_unit, &full);
        full /= driver_unit;
    }
    if (!SQL_SUCCEEDED(result.res.fn_result)) {
        return result;
    }

    // What the base driver wrote, up to its terminator, as UTF-8
    const auto is_terminator = [&buffer, driver_step](size_t index) {
        return buffer[index * driver_step] == 0 && (driver_step == 1 || buffer[(index * driver_step) + 1] == 0);
    };
    const size_t limit = std::min(full, room - 1);
    size_t written = 0;
    while (written < limit && !is_terminator(written)) {
        written++;
    }
    const icu::UnicodeString text = use_4_bytes_base_driver_
        ? icu::UnicodeString::fromUTF32(reinterpret_cast<const UChar32*>(buffer.data()), static_cast<int32_t>(written))
        : icu::UnicodeString(reinterpret_cast<const char16_t*>(buffer.data()), static_cast<int32_t>(written));
    std::string utf8;
    text.toUTF8String(utf8);

    size_t length = CopyUTF8ToAppChars(utf8, app_chars > 0 ? app_out : nullptr, app_chars, app_4_byte);
    // Still cut (the string grew, or is longer than max_length): the rest counted in the base driver's characters
    if (full > limit) {
        length += full - limit;
    }
    result.length = length * app_unit;
    result.truncated = app_out != nullptr && length >= app_chars;
    return result;
}
#endif

bool OdbcHelper::IsStringConnectAttr(const SQLINTEGER attribute) {
    switch (attribute) {
        case SQL_ATTR_CURRENT_CATALOG:
        case SQL_ATTR_TRACEFILE:
        case SQL_ATTR_TRANSLATE_LIB:
            return true;
        default:
            return false;
    }
}

bool OdbcHelper::IsStringDescField(const SQLSMALLINT field_identifier) {
    switch (field_identifier) {
        case SQL_DESC_BASE_COLUMN_NAME:
        case SQL_DESC_BASE_TABLE_NAME:
        case SQL_DESC_CATALOG_NAME:
        case SQL_DESC_LABEL:
        case SQL_DESC_LITERAL_PREFIX:
        case SQL_DESC_LITERAL_SUFFIX:
        case SQL_DESC_LOCAL_TYPE_NAME:
        case SQL_DESC_NAME:
        case SQL_DESC_SCHEMA_NAME:
        case SQL_DESC_TABLE_NAME:
        case SQL_DESC_TYPE_NAME:
            return true;
        default:
            return false;
    }
}

bool OdbcHelper::IsStringDiagField(const SQLSMALLINT diag_identifier) {
    switch (diag_identifier) {
        case SQL_DIAG_CLASS_ORIGIN:
        case SQL_DIAG_CONNECTION_NAME:
        case SQL_DIAG_DYNAMIC_FUNCTION:
        case SQL_DIAG_MESSAGE_TEXT:
        case SQL_DIAG_SERVER_NAME:
        case SQL_DIAG_SQLSTATE:
        case SQL_DIAG_SUBCLASS_ORIGIN:
            return true;
        default:
            return false;
    }
}

// iODBC (info.c) and unixODBC (SQLGetInfo.c) convert exactly these between ANSI and Unicode.
bool OdbcHelper::IsStringInfoType(const SQLUSMALLINT info_type) {
    switch (info_type) {
        case SQL_ACCESSIBLE_PROCEDURES:
        case SQL_ACCESSIBLE_TABLES:
        case SQL_CATALOG_NAME:
        case SQL_CATALOG_NAME_SEPARATOR:
        case SQL_CATALOG_TERM:
        case SQL_COLLATION_SEQ:
        case SQL_COLUMN_ALIAS:
        case SQL_DATA_SOURCE_NAME:
        case SQL_DATA_SOURCE_READ_ONLY:
        case SQL_DATABASE_NAME:
        case SQL_DBMS_NAME:
        case SQL_DBMS_VER:
        case SQL_DESCRIBE_PARAMETER:
        case SQL_DRIVER_NAME:
        case SQL_DRIVER_ODBC_VER:
        case SQL_DRIVER_VER:
        case SQL_ODBC_VER:
        case SQL_EXPRESSIONS_IN_ORDERBY:
        case SQL_IDENTIFIER_QUOTE_CHAR:
        case SQL_INTEGRITY:
        case SQL_KEYWORDS:
        case SQL_LIKE_ESCAPE_CLAUSE:
        case SQL_MAX_ROW_SIZE_INCLUDES_LONG:
        case SQL_MULT_RESULT_SETS:
        case SQL_MULTIPLE_ACTIVE_TXN:
        case SQL_NEED_LONG_DATA_LEN:
        case SQL_ORDER_BY_COLUMNS_IN_SELECT:
        case SQL_PROCEDURE_TERM:
        case SQL_PROCEDURES:
        case SQL_ROW_UPDATES:
        case SQL_SCHEMA_TERM:
        case SQL_SEARCH_PATTERN_ESCAPE:
        case SQL_SERVER_NAME:
        case SQL_SPECIAL_CHARACTERS:
        case SQL_TABLE_TERM:
        case SQL_USER_NAME:
        case SQL_XOPEN_CLI_YEAR:
        case SQL_OUTER_JOINS:
            return true;
        default:
            return false;
    }
}

bool OdbcHelper::IsStringColAttribute(const SQLUSMALLINT field_identifier) {
    // SQL_COLUMN_TYPE_NAME, SQL_COLUMN_TABLE_NAME, SQL_COLUMN_OWNER_NAME, SQL_COLUMN_QUALIFIER_NAME and
    // SQL_COLUMN_LABEL have the values of their SQL_DESC_ names; SQL_COLUMN_NAME doesn't.
    return field_identifier == SQL_COLUMN_NAME || IsStringDescField(static_cast<SQLSMALLINT>(field_identifier));
}
