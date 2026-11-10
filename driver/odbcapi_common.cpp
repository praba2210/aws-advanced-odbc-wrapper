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

#include <algorithm>
#include <limits>

#include "error.h"
#include "odbcapi_rds_helper.h"
#include "plugin/base_plugin.h"
#include "plugin/dsql/dsql_compatibility.h"
#include "util/plugin_service.h"
#include "util/rds_lib_loader.h"

// Unicode buffer helpers
#if UNICODE && !defined(_WIN32)
namespace {
// Writes a value the base driver returned, UTF-16 in local by now, into a 4-byte app's buffer as SQLGetData does: whole
// code points and a NUL, as many as app_buf_len bytes hold. *app_len_ptr gets the whole value's length in the app's
// bytes; base_bytes is the base driver's, in its own width, and for a value it cut itself the missing UTF-16 units
// count as code points, the most they can be. Returns whether the app's buffer cut the value.
bool CopyValueToFourByteApp(const SQLTCHAR* local, SQLLEN base_bytes, bool use_4_base, SQLPOINTER app_ptr,
                            SQLLEN app_buf_len, SQLLEN* app_len_ptr) {
    const size_t src_len = UShortStrlen(reinterpret_cast<const uint16_t*>(local));
    const icu::UnicodeString value(reinterpret_cast<const char16_t*>(local), static_cast<int32_t>(src_len));
    const auto held = static_cast<size_t>(value.countChar32());
    const auto base_length = static_cast<size_t>(base_bytes);
    size_t full = held;
    if (use_4_base) {
        full = std::max(held, base_length / (sizeof(SQLTCHAR) * 2));
    } else if (base_length / sizeof(SQLTCHAR) > src_len) {
        full = held + ((base_length / sizeof(SQLTCHAR)) - src_len);
    }
    const size_t app_chars = app_buf_len > 0 ? static_cast<size_t>(app_buf_len) / (sizeof(SQLTCHAR) * 2) : 0;
    if (app_ptr != nullptr && app_chars > 0) {
        ConvertUTF16ToUTF32(local, static_cast<SQLTCHAR*>(app_ptr), src_len, app_chars * 2);
    }
    if (app_len_ptr != nullptr) {
        *app_len_ptr = static_cast<SQLLEN>(full * sizeof(SQLTCHAR) * 2);
    }
    return full >= app_chars;
}

// The same for a 2-byte app: as many UTF-16 units as app_buf_len bytes hold with a NUL, a surrogate pair whole or not
// at all. For a value the base driver cut itself, its characters the wrapper never got count as one UTF-16 unit each,
// exact for text in the BMP. Returns whether the app's buffer cut the value.
bool CopyValueToTwoByteApp(const SQLTCHAR* local, SQLLEN base_bytes, bool use_4_base, SQLPOINTER app_ptr,
                           SQLLEN app_buf_len, SQLLEN* app_len_ptr) {
    const size_t src_len = UShortStrlen(reinterpret_cast<const uint16_t*>(local));
    const auto base_length = static_cast<size_t>(base_bytes);
    size_t full = src_len;
    if (use_4_base) {
        const icu::UnicodeString value(reinterpret_cast<const char16_t*>(local), static_cast<int32_t>(src_len));
        const auto held = static_cast<size_t>(value.countChar32());
        const size_t base_chars = base_length / (sizeof(SQLTCHAR) * 2);
        if (base_chars > held) {
            full = src_len + (base_chars - held);
        }
    } else {
        full = std::max(src_len, base_length / sizeof(SQLTCHAR));
    }
    const size_t app_units = app_buf_len > 0 ? static_cast<size_t>(app_buf_len) / sizeof(SQLTCHAR) : 0;
    if (app_ptr != nullptr && app_units > 0) {
        size_t copy_units = std::min(src_len, app_units - 1);
        if (copy_units > 0 && copy_units < src_len && U16_IS_LEAD(local[copy_units - 1])) {
            --copy_units;
        }
        SQLTCHAR* dst = static_cast<SQLTCHAR*>(app_ptr);
        std::memcpy(dst, local, copy_units * sizeof(SQLTCHAR));
        dst[copy_units] = 0;
    }
    if (app_len_ptr != nullptr) {
        *app_len_ptr = static_cast<SQLLEN>(full * sizeof(SQLTCHAR));
    }
    return full >= app_units;
}

// Drops a descriptor's copies for a record whose base record no longer points at them. A column's and a parameter's:
// a descriptor that is one statement's ARD and another's APD has one set of records (psqlODBC keeps both in a union).
void DropRecordCopies(DESC* desc, SQLUSMALLINT record) {
    std::vector<BoundColBuffer>& columns = desc->bound_col_buffers;
    columns.erase(std::remove_if(columns.begin(), columns.end(),
                                 [record](const BoundColBuffer& b) { return b.column_number == record; }),
                  columns.end());
    std::vector<BoundParamBuffer>& params = desc->bound_param_buffers;
    params.erase(std::remove_if(params.begin(), params.end(),
                                [record](const BoundParamBuffer& b) { return b.param_number == record; }),
                 params.end());
}

// Whether the base driver would use a binding of the statement's ARD (rows) or APD for more than one row or parameter
// set, row-wise or at a bind offset, as its statement attributes are now: they read the ARD's or APD's header, also an
// explicit descriptor's and one set through SQLSetDescField. An attribute the base driver doesn't give counts as unset.
bool UsesMultiRowBindings(const STMT* stmt, bool rows) {
    const ENV* env = stmt->dbc->env;
    const auto get = [stmt, env](SQLINTEGER attribute) {
        SQLULEN value = 0;
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetStmtAttr>(env->driver_lib_loader, RdsFuncNames::GetStmtAttr,
            stmt->wrapped_stmt, attribute, &value, static_cast<SQLINTEGER>(sizeof(value)), nullptr
        );
        return res.fn_load_success && SQL_SUCCEEDED(res.fn_result) ? value : 0;
    };
    if (rows) {
        return get(SQL_ATTR_ROW_ARRAY_SIZE) > 1 || get(SQL_ROWSET_SIZE) > 1
            || get(SQL_ATTR_ROW_BIND_TYPE) != SQL_BIND_BY_COLUMN || get(SQL_ATTR_ROW_BIND_OFFSET_PTR) != 0;
    }
    return get(SQL_ATTR_PARAMSET_SIZE) > 1 || get(SQL_ATTR_PARAM_BIND_TYPE) != SQL_PARAM_BIND_BY_COLUMN
        || get(SQL_ATTR_PARAM_BIND_OFFSET_PTR) != 0;
}

// A fetch or an execute whose bound value the wrapper cut for the app: SQL_SUCCESS_WITH_INFO with 01004, unless the
// base driver already returned a warning (its record stays) or an error.
SQLRETURN ReportBoundValueCut(STMT* stmt, SQLRETURN ret, bool cut, const char* value) {
    if (!cut || ret != SQL_SUCCESS) {
        return ret;
    }
    ClearError(stmt);
    stmt->err = std::make_unique<ErrInfo>(std::string(value) + " was truncated", WARN_STRING_DATA_RIGHT_TRUNCATED);
    return SQL_SUCCESS_WITH_INFO;
}

SQLRETURN ConvertBoundColBuffersAfterFetch(STMT* stmt, SQLRETURN ret, const char* function) {
    // The copies of the ARD the statement uses, which the base driver wrote; another descriptor's are its bindings.
    DESC* ard = CurrentRowDesc(stmt);
    if (ard == nullptr) {
        return ret;
    }
    const std::lock_guard<std::recursive_mutex> desc_lock_guard(ard->lock);
    std::vector<BoundColBuffer>& buffers = ard->bound_col_buffers;
    if (buffers.empty()) {
        return ret;
    }
    bool cut = false;

    const bool use_4_base = stmt->dbc->plugin_service->GetOdbcHelper()->GetUse4BytesBaseDriver();
    const bool use_4_app = stmt->dbc->plugin_service->GetOdbcHelper()->GetUse4BytesUserApp();

    for (BoundColBuffer& buffer : buffers) {
        if (*buffer.local_str_len == SQL_NULL_DATA || *buffer.local_str_len < 0) {
            if (buffer.app_str_len_ptr) {
                *buffer.app_str_len_ptr = *buffer.local_str_len;
            }
            continue;
        }

        const SQLTCHAR* local = buffer.local_buf.data();

        // If base driver is 4-byte, its UTF-32 to UTF-16, no further than the copy holds (the length of a value the
        // base driver cut is the whole value's), with room for all of the copy: a character outside the BMP takes two
        // units. In a buffer of its own: the copy keeps the base driver's value, which the base driver reads again
        // through a descriptor that is also another statement's APD.
        std::vector<SQLTCHAR> utf16;
        if (use_4_base) {
            utf16 = buffer.local_buf;
            const size_t base_chars = static_cast<size_t>(*buffer.local_str_len) / (sizeof(SQLTCHAR) * 2);
            ConvertUTF32ToUTF16InPlace(utf16.data(), 1 + base_chars, utf16.size());
            local = utf16.data();
        }

        // Copy to user's buffer in the expected encoding
        if (use_4_app) {
            cut = CopyValueToFourByteApp(local, *buffer.local_str_len, use_4_base, buffer.app_ptr, buffer.app_buf_len,
                                         buffer.app_str_len_ptr) || cut;
        } else {
            cut = CopyValueToTwoByteApp(local, *buffer.local_str_len, use_4_base, buffer.app_ptr, buffer.app_buf_len,
                                        buffer.app_str_len_ptr) || cut;
        }
    }
    return ReportBoundValueCut(stmt, ret, cut, (std::string(function) + " - A bound column's value").c_str());
}

} // namespace

void RDS_ConvertBoundParamBuffersBeforeExecute(STMT* stmt) {
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    // Invalidated (e.g. by a failed failover); nothing to rebind against.
    if (!stmt->wrapped_stmt) {
        return;
    }

    // The copies of the APD the statement uses: another descriptor's would go over the records of the one in use.
    DESC* apd = CurrentParamDesc(stmt);
    if (apd == nullptr) {
        return;
    }
    const std::lock_guard<std::recursive_mutex> desc_lock_guard(apd->lock);
    std::vector<BoundParamBuffer>& bindings = apd->bound_param_buffers;
    if (bindings.empty()) {
        return;
    }

    const bool use_4_base = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesBaseDriver();
    const bool use_4_app = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesUserApp();

    for (BoundParamBuffer& buffer : bindings) {
        if (buffer.input_output_type == SQL_PARAM_OUTPUT) {
            continue;
        }

        // A NULL indicator: the text ends in a NUL (ODBC), as for SQL_NTS.
        const SQLLEN app_ind = buffer.app_str_len_ptr ? *buffer.app_str_len_ptr : SQL_NTS;
        // NULL, the column's default or no value: the base driver gets the indicator, and the buffer isn't read.
        if (app_ind == SQL_NULL_DATA || app_ind == SQL_DEFAULT_PARAM || app_ind == SQL_COLUMN_IGNORE) {
            *buffer.local_str_len = app_ind;
            continue;
        }
        // Skip as data will be put by SQLPutData
        if (app_ind == SQL_DATA_AT_EXEC
            || app_ind <= SQL_LEN_DATA_AT_EXEC(0)) {
            continue;
        }

        SQLTCHAR* app_data = static_cast<SQLTCHAR*>(buffer.app_ptr);

        // Get explicit user length or get length of app_data
        size_t char_count;
        if (app_ind == SQL_NTS || app_ind < 0) {
            char_count = UShortStrlen(reinterpret_cast<const uint16_t*>(app_data), use_4_app);
        } else {
            char_count = static_cast<size_t>(app_ind);
            // app_ind is byte count, divide by app's SQLWCHAR size
            char_count = use_4_app
                ? char_count / 4
                : char_count / 2;
        }

        // Resize local buffer to runtime length of app_data
        const size_t app_data_size = 1 + char_count;
        const size_t local_buf_resize = use_4_base
            ? app_data_size * 2
            : app_data_size;
        if (local_buf_resize > buffer.local_buf.size()) {
            buffer.local_buf.resize(local_buf_resize, 0);
        }

        if (use_4_app) {
            // User app UTF32 -> the base driver's width: char_count code points (to the NUL for SQL_NTS) and no more,
            // whole also outside the BMP. The buffer only grows: the base driver writes an output parameter into it.
            const SQLINTEGER app_chars = app_ind == SQL_NTS || app_ind < 0
                ? SQL_NTS
                : static_cast<SQLINTEGER>(std::min<size_t>(char_count, std::numeric_limits<SQLINTEGER>::max()));
            const std::vector<SQLTCHAR> converted = ConvertUserAppInputToBaseDriver(true, use_4_base, app_data, app_chars);
            if (converted.size() > buffer.local_buf.size()) {
                buffer.local_buf.resize(converted.size(), 0);
            }
            std::copy(converted.begin(), converted.end(), buffer.local_buf.begin());
        } else {
            // Copy as-is
            const size_t src_len = char_count;
            const size_t max_copy = static_cast<size_t>(buffer.local_buf.size());
            const size_t copy_len = src_len < max_copy ? src_len : max_copy;
            std::memcpy(buffer.local_buf.data(), app_data, copy_len * sizeof(SQLTCHAR));
            buffer.local_buf[copy_len] = 0;

            if (use_4_base) {
                // Expand to UTF32 if needed
                ExpandUTF16ToUTF32InPlace(buffer.local_buf.data(), app_data_size, buffer.local_buf.size());
            }
        }

        *buffer.local_str_len = SQL_NTS;

        // Rebind the copy, which may have grown, with its size as SQLBindParameter binds it: the base driver writes an
        // SQL_PARAM_INPUT_OUTPUT value back into it, and the copy back cuts that for the app's buffer.
        const auto copy_bytes = static_cast<SQLLEN>(buffer.local_buf.size() * sizeof(SQLTCHAR));
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLBindParameter>(
            env->driver_lib_loader, RdsFuncNames::BindParameter, stmt->wrapped_stmt,
            buffer.param_number, buffer.input_output_type, buffer.value_type, buffer.param_type, buffer.column_size,
            buffer.decimal_digits, buffer.local_buf.data(), copy_bytes, buffer.local_str_len.get()
        );
    }
}

SQLRETURN RDS_ConvertBoundParamBuffersAfterExecute(STMT* stmt, SQLRETURN ret, const char* function) {
    // The copies of the APD the statement uses, which the base driver wrote.
    DESC* apd = CurrentParamDesc(stmt);
    if (apd == nullptr) {
        return ret;
    }
    const std::lock_guard<std::recursive_mutex> desc_lock_guard(apd->lock);
    std::vector<BoundParamBuffer>& bindings = apd->bound_param_buffers;
    if (bindings.empty()) {
        return ret;
    }
    bool cut = false;

    const bool use_4_base = stmt->dbc->plugin_service->GetOdbcHelper()->GetUse4BytesBaseDriver();
    const bool use_4_app = stmt->dbc->plugin_service->GetOdbcHelper()->GetUse4BytesUserApp();

    for (BoundParamBuffer& param : bindings) {
        // A data-at-execution parameter is bound with the app's own buffer, no copy: nothing to copy back.
        if (param.input_output_type == SQL_PARAM_INPUT || param.local_buf.empty()) {
            continue;
        }

        if (*param.local_str_len == SQL_NULL_DATA || *param.local_str_len < 0) {
            if (param.app_str_len_ptr) {
                *param.app_str_len_ptr = *param.local_str_len;
            }
            continue;
        }

        const SQLTCHAR* local = param.local_buf.data();

        // If base driver is 4-byte, its UTF-32 to UTF-16 as for a bound column, in a buffer of its own: the copy keeps
        // the base driver's value.
        std::vector<SQLTCHAR> utf16;
        if (use_4_base) {
            utf16 = param.local_buf;
            const size_t base_chars = static_cast<size_t>(*param.local_str_len) / (sizeof(SQLTCHAR) * 2);
            ConvertUTF32ToUTF16InPlace(utf16.data(), 1 + base_chars, utf16.size());
            local = utf16.data();
        }

        // Copy to user's buffer in the expected encoding; nothing written for a BufferLength of 0.
        if (use_4_app) {
            cut = CopyValueToFourByteApp(local, *param.local_str_len, use_4_base, param.app_ptr, param.app_buf_len,
                                         param.app_str_len_ptr) || cut;
        } else {
            cut = CopyValueToTwoByteApp(local, *param.local_str_len, use_4_base, param.app_ptr, param.app_buf_len,
                                        param.app_str_len_ptr) || cut;
        }
    }
    return ReportBoundValueCut(stmt, ret, cut, (std::string(function) + " - An output parameter's value").c_str());
}
#endif

// Common ODBC Functions
SQLRETURN SQL_API SQLAllocConnect(
    SQLHENV        EnvironmentHandle,
    SQLHDBC *      ConnectionHandle)
{
    LOG(INFO) << "Entering SQLAllocConnect";
    return RDS_AllocDbc(EnvironmentHandle, ConnectionHandle);
};

SQLRETURN SQL_API SQLAllocEnv(
    SQLHENV *      EnvironmentHandle)
{
    LOG(INFO) << "Entering SQLAllocEnv";
    return RDS_AllocEnv(EnvironmentHandle);
}

SQLRETURN SQL_API SQLAllocHandle(
    SQLSMALLINT    HandleType,
    SQLHANDLE      InputHandle,
    SQLHANDLE *    OutputHandlePtr)
{
    LOG(INFO) << "Entering SQLAllocHandle";
    SQLRETURN ret = SQL_ERROR;
    switch (HandleType) {
        case SQL_HANDLE_ENV:
            ret = RDS_AllocEnv(OutputHandlePtr);
            break;
        case SQL_HANDLE_DBC:
            ret = RDS_AllocDbc(InputHandle, OutputHandlePtr);
            break;
        case SQL_HANDLE_STMT:
            ret = RDS_AllocStmt(InputHandle, OutputHandlePtr);
            break;
        case SQL_HANDLE_DESC:
            ret = RDS_AllocDesc(InputHandle, OutputHandlePtr);
            break;
        default:
            ret = SQL_INVALID_HANDLE;
    }
    return ret;
}

SQLRETURN SQL_API SQLAllocStmt(
    SQLHDBC        ConnectionHandle,
    SQLHSTMT *     StatementHandle)
{
    LOG(INFO) << "Entering SQLAllocStmt";
    return RDS_AllocStmt(ConnectionHandle, StatementHandle);
}

SQLRETURN SQL_API SQLBindCol(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   ColumnNumber,
    SQLSMALLINT    TargetType,
    SQLPOINTER     TargetValuePtr,
    SQLLEN         BufferLength,
    SQLLEN *       StrLen_or_IndPtr)
{
    LOG(INFO) << "Entering SQLBindCol";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    #if UNICODE && !defined(_WIN32)
    const bool use_4_base = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesBaseDriver();
    // The copies are on the ARD that holds the binding (the explicit one the app set, or the implicit one) and live
    // as long as its base record points at them, whichever statements use it. With both SQLWCHARs the same width
    // (2 bytes, or 4 for an iODBC app over a psqlODBC built for iODBC) there are none: the base driver gets the app's
    // buffers as they are.
    if (DESC* ard = CurrentRowDesc(stmt);
        ard != nullptr && dbc->plugin_service->GetOdbcHelper()->NeedsConversion()) {
        const std::lock_guard<std::recursive_mutex> desc_lock_guard(ard->lock);
        // A SQL_C_WCHAR column with a buffer: the base driver gets a copy in its own width. The copy holds one row.
        const bool intercepted = TargetValuePtr != nullptr && TargetType == SQL_C_TCHAR && BufferLength > 0;
        if (intercepted && UsesMultiRowBindings(stmt, true)) {
            return RDS_OneRowCopiesError(stmt, "SQLBindCol");
        }
        BoundColBuffer new_buffer;
        SQLPOINTER bind_ptr = TargetValuePtr;
        SQLLEN bind_len = BufferLength;
        SQLLEN* bind_ind = StrLen_or_IndPtr;
        if (intercepted) {
            const size_t local_buf_size = use_4_base
                ? static_cast<size_t>(BufferLength) * 2
                : static_cast<size_t>(BufferLength);

            new_buffer.column_number = ColumnNumber;
            new_buffer.app_ptr = TargetValuePtr;
            new_buffer.app_buf_len = BufferLength;
            new_buffer.app_str_len_ptr = StrLen_or_IndPtr;
            new_buffer.local_buf.resize(local_buf_size, 0);

            // The base driver gets the copy with its size, as for a parameter: in a 4-byte base driver's characters a
            // 2-byte app's BufferLength holds half of what the app's buffer does. The copy back cuts it for the app.
            bind_ptr = new_buffer.local_buf.data();
            bind_len = static_cast<SQLLEN>(new_buffer.local_buf.size() * sizeof(SQLTCHAR));
            bind_ind = new_buffer.local_str_len.get();
        }
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLBindCol>(env->driver_lib_loader, RdsFuncNames::BindCol,
            stmt->wrapped_stmt, ColumnNumber, TargetType, bind_ptr, bind_len, bind_ind
        );
        const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
        // Whatever the new binding is (another type, a BufferLength of 0, an unbind), the base record doesn't point at
        // the column's old copy once the base driver took it, so the copy goes: kept, each fetch would write its stale
        // value into the app's old buffer. An intercepted one's new copy goes in; moving it keeps the buffers the base
        // driver has.
        if (SQL_SUCCEEDED(ret)) {
            DropRecordCopies(ard, ColumnNumber);
            if (intercepted) {
                ard->bound_col_buffers.push_back(std::move(new_buffer));
            }
        }
        return ret;
    }
    #endif

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLBindCol>(env->driver_lib_loader, RdsFuncNames::BindCol,
        stmt->wrapped_stmt, ColumnNumber, TargetType, TargetValuePtr, BufferLength, StrLen_or_IndPtr
    );
    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
#if UNICODE && !defined(_WIN32)
    // A copy made while the widths differed (before a connect that found other widths) no longer has the base record.
    if (DESC* ard = CurrentRowDesc(stmt); ard != nullptr && SQL_SUCCEEDED(ret)) {
        const std::lock_guard<std::recursive_mutex> desc_lock_guard(ard->lock);
        DropRecordCopies(ard, ColumnNumber);
    }
#endif
    return ret;
}

SQLRETURN SQL_API SQLBindParameter(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   ParameterNumber,
    SQLSMALLINT    InputOutputType,
    SQLSMALLINT    ValueType,
    SQLSMALLINT    ParameterType,
    SQLULEN        ColumnSize,
    SQLSMALLINT    DecimalDigits,
    SQLPOINTER     ParameterValuePtr,
    SQLLEN         BufferLength,
    SQLLEN *       StrLen_or_IndPtr)
{
    LOG(INFO) << "Entering SQLBindParameter";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    #if UNICODE && !defined(_WIN32)
    const bool use_4_base = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesBaseDriver();
    // The copies are on the APD that holds the binding, as for SQLBindCol's on the ARD (none with matched widths).
    if (DESC* apd = CurrentParamDesc(stmt);
        apd != nullptr && dbc->plugin_service->GetOdbcHelper()->NeedsConversion()) {
        const std::lock_guard<std::recursive_mutex> desc_lock_guard(apd->lock);

        // Data to be handled at execute runtime
        const bool is_data_at_exec = (StrLen_or_IndPtr != nullptr) &&
            (
                *StrLen_or_IndPtr == SQL_DATA_AT_EXEC
                || *StrLen_or_IndPtr <= SQL_LEN_DATA_AT_EXEC(0)
            );
        // A SQL_C_WCHAR parameter: the base driver gets a copy in its own width, or for data at execution the app's
        // buffer as its token (SQLPutData converts the data). A NULL buffer otherwise gets nothing of the wrapper's.
        const bool intercepted = ValueType == SQL_C_TCHAR && (ParameterValuePtr != nullptr || is_data_at_exec);
        // The copy holds one parameter set.
        if (intercepted && !is_data_at_exec && UsesMultiRowBindings(stmt, false)) {
            return RDS_OneRowCopiesError(stmt, "SQLBindParameter");
        }

        BoundParamBuffer new_buffer;
        SQLPOINTER bind_ptr = ParameterValuePtr;
        SQLLEN bind_len = BufferLength;
        SQLLEN* bind_itr = StrLen_or_IndPtr;
        if (intercepted) {
            new_buffer.param_number = ParameterNumber;
            new_buffer.input_output_type = InputOutputType;
            new_buffer.value_type = ValueType;
            new_buffer.param_type = ParameterType;
            new_buffer.column_size = ColumnSize;
            new_buffer.decimal_digits = DecimalDigits;
            new_buffer.app_ptr = ParameterValuePtr;
            new_buffer.app_buf_len = BufferLength;
            new_buffer.app_str_len_ptr = StrLen_or_IndPtr;

            // If data_at_exec, pass original pointer
            // so the driver can return token location properly
            if (!is_data_at_exec) {
                // The base driver gets the wrapper's copy in its own width, filled from the app's buffer and indicator
                // before each execute. Also for a BufferLength of 0, common for an input parameter: the copy grows to
                // the text then. The app's buffer isn't read now.
                const size_t app_buf_len = BufferLength > 0 ? static_cast<size_t>(BufferLength) : 0;
                const size_t local_buf_size = use_4_base ? app_buf_len * 2 : app_buf_len;
                // At least one of the base driver's characters and a NUL, which an output parameter's copy back
                // converts.
                const size_t base_char_size = use_4_base ? 2 : 1;
                new_buffer.local_buf.resize(std::max(1 + local_buf_size, 2 * base_char_size), 0);
                *new_buffer.local_str_len = SQL_NTS;
                bind_ptr = new_buffer.local_buf.data();
                bind_len = static_cast<SQLLEN>(new_buffer.local_buf.size() * sizeof(SQLTCHAR));
                bind_itr = new_buffer.local_str_len.get();
            }
        }

        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLBindParameter>(
            env->driver_lib_loader, RdsFuncNames::BindParameter, stmt->wrapped_stmt,
            ParameterNumber, InputOutputType, ValueType, ParameterType, ColumnSize,
            DecimalDigits, bind_ptr, bind_len, bind_itr
        );
        const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
        // Whatever the new binding is, the parameter's old copy goes once the base driver took it: kept, the rebind
        // before each execute would put it back over the new binding. An intercepted one goes in; moving it keeps the
        // buffers the base driver has.
        if (SQL_SUCCEEDED(ret)) {
            DropRecordCopies(apd, ParameterNumber);
            if (intercepted) {
                apd->bound_param_buffers.push_back(std::move(new_buffer));
            }
        }
        return ret;
    }
    #endif

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLBindParameter>(env->driver_lib_loader, RdsFuncNames::BindParameter,
        stmt->wrapped_stmt, ParameterNumber, InputOutputType, ValueType, ParameterType, ColumnSize, DecimalDigits, ParameterValuePtr, BufferLength, StrLen_or_IndPtr
    );
    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
#if UNICODE && !defined(_WIN32)
    // As for SQLBindCol.
    if (DESC* apd = CurrentParamDesc(stmt); apd != nullptr && SQL_SUCCEEDED(ret)) {
        const std::lock_guard<std::recursive_mutex> desc_lock_guard(apd->lock);
        DropRecordCopies(apd, ParameterNumber);
    }
#endif
    return ret;
}

SQLRETURN SQL_API SQLBulkOperations(
    SQLHSTMT       StatementHandle,
    SQLSMALLINT    Operation)
{
    LOG(INFO) << "Entering SQLBulkOperations";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
#if UNICODE && !defined(_WIN32)
    // As for SQLSetPos: the base driver would add, update, delete or fetch through the copies.
    if (HasOneRowCopies(CurrentRowDesc(stmt))) {
        stmt->err = std::make_unique<ErrInfo>(
            "SQLBulkOperations - Not supported with the SQL_C_WCHAR columns the wrapper converts between the application's"
            " and the driver's SQLWCHAR", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
        return SQL_ERROR;
    }
#endif
    // Its own data-at-execution columns, if any: no execute waits for SQLParamData now.
    stmt->execute_needs_data = false;
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLBulkOperations>(env->driver_lib_loader, RdsFuncNames::BulkOperations,
        stmt->wrapped_stmt, Operation
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLCancel(
    SQLHSTMT       StatementHandle)
{
    LOG(INFO) << "Entering SQLCancel";
    return RDS_SQLCancel(StatementHandle);
}

SQLRETURN SQL_API SQLCancelHandle(
    SQLSMALLINT    HandleType,
    SQLHANDLE      Handle)
{
    LOG(INFO) << "Entering SQLCancelHandle";
    DESC* desc;
    DBC* dbc;
    ENV* env;
    SQLRETURN ret = SQL_ERROR;
    switch (HandleType) {
        case SQL_HANDLE_STMT:
            return RDS_SQLCancel(Handle);
        case SQL_HANDLE_ENV:
        {
            if (!HasEnvAccess<ENV>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            env = static_cast<ENV*>(Handle);
            const std::lock_guard<std::recursive_mutex> lock_guard(env->lock);
            LOG(ERROR) << "Unsupported SQL API - SQLCancelHandle ENV";
            ClearError(env);
            env->err = std::make_unique<ErrInfo>("SQLCancelHandle ENV - API Unsupported", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
            RDS_NOT_IMPLEMENTED;
        }
        case SQL_HANDLE_DBC:
        {
            if (!HasEnvAccess<DBC>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            dbc = static_cast<DBC*>(Handle);
            const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
            LOG(ERROR) << "Unsupported SQL API - SQLCancelHandle DBC";
            ClearError(dbc);
            dbc->err = std::make_unique<ErrInfo>("SQLCancelHandle DBC - API Unsupported", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
            RDS_NOT_IMPLEMENTED;
        }
        case SQL_HANDLE_DESC:
        {
            if (!HasEnvAccess<DESC>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            desc = static_cast<DESC*>(Handle);
            const std::lock_guard<std::recursive_mutex> lock_guard(desc->lock);
            LOG(ERROR) << "Unsupported SQL API - SQLCancelHandle DESC";
            ClearError(desc);
            desc->err = std::make_unique<ErrInfo>("SQLCancelHandle DESC - API Unsupported", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
            RDS_NOT_IMPLEMENTED;
        }
        default:
            // TODO - Set Error
            ret = SQL_INVALID_HANDLE;
            break;
    }

    return ret;
}

SQLRETURN SQL_API SQLCloseCursor(
    SQLHSTMT       StatementHandle)
{
    LOG(INFO) << "Entering SQLCloseCursor";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;
    SQLRETURN ret = SQL_SUCCESS;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (stmt->wrapped_stmt) {
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLCloseCursor>(env->driver_lib_loader, RdsFuncNames::CloseCursor,
            stmt->wrapped_stmt
        );
        ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
    }

    return ret;
}

// TODO Maybe - Impl SQLCompleteAsync
// Both PostgreSQL AND MySQL do NOT implement this
SQLRETURN SQL_API SQLCompleteAsync(
    SQLSMALLINT   HandleType,
    SQLHANDLE     Handle,
    RETCODE *     AsyncRetCodePtr)
{
    LOG(INFO) << "Entering SQLCompleteAsync";
    switch (HandleType) {
        case SQL_HANDLE_DBC:
        {
            if (!HasEnvAccess<DBC>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            DBC* dbc = static_cast<DBC*>(Handle);
            const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
            LOG(ERROR) << "Unsupported SQL API - SQLCompleteAsync DBC";
            ClearError(dbc);
            dbc->err = std::make_unique<ErrInfo>("SQLCompleteAsync DBC - API Unsupported", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
            break;
        }

        case SQL_HANDLE_STMT:
        {
            if (!HasEnvAccess<STMT>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            STMT* stmt = static_cast<STMT*>(Handle);
            const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
            LOG(ERROR) << "Unsupported SQL API - SQLCompleteAsync STMT";
            ClearError(stmt);
            stmt->err = std::make_unique<ErrInfo>("SQLCompleteAsync STMT - API Unsupported", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
            break;
        }
        default:
            return SQL_INVALID_HANDLE;
    }
    RDS_NOT_IMPLEMENTED;
}

SQLRETURN SQL_API SQLCopyDesc(
    SQLHDESC       SourceDescHandle,
    SQLHDESC       TargetDescHandle)
{
    LOG(INFO) << "Entering SQLCopyDesc";
    return RDS_SQLCopyDesc(SourceDescHandle, TargetDescHandle);
}

SQLRETURN SQL_API SQLDescribeParam(
    SQLHSTMT      StatementHandle,
    SQLUSMALLINT  ParameterNumber,
    SQLSMALLINT * DataTypePtr,
    SQLULEN *     ParameterSizePtr,
    SQLSMALLINT * DecimalDigitsPtr,
    SQLSMALLINT * NullablePtr)
{
    LOG(INFO) << "Entering SQLDescribeParam";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLDescribeParam>(env->driver_lib_loader, RdsFuncNames::DescribeParam,
        stmt->wrapped_stmt, ParameterNumber, DataTypePtr, ParameterSizePtr, DecimalDigitsPtr, NullablePtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLDisconnect(
    SQLHDBC        ConnectionHandle)
{
    LOG(INFO) << "Entering SQLDisconnect";
    SQLRETURN ret = SQL_ERROR;
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    ClearError(dbc);

    // Cleanup tracked statements
    const std::list<STMT*> stmt_list = dbc->stmt_list;
    for (STMT* stmt : stmt_list) {
        RDS_FreeStmt(stmt, SQL_DROP);
    }
    dbc->stmt_list.clear();

    // Clean up plugin internal connections before disconnecting main handle
    if (dbc->plugin_head) {
        dbc->plugin_head->ReleaseResources();
    }

    // Break circular reference: PluginService -> HostListProvider -> Monitor -> PluginService
    // This ensures the topology monitor is stopped and cleaned up before the DBC is freed.
    if (dbc->plugin_service) {
        dbc->plugin_service->ReleaseHostListProvider();
    }

    if (!HasWrappedHandle(dbc)) {
        return SQL_INVALID_HANDLE;
    }
    // An SQLCancel from another thread is still in the base driver, and the base frees of the statements above wait
    // for it: so does the base disconnect, which frees or detaches them in the base driver. The base connection goes
    // with it, taken out of the handle, so that a new connect gets a base connection of its own that the late
    // disconnect can't close. The app gets SQL_SUCCESS: the base driver's answer comes after this returns.
    if (dbc->base_handles.Busy()) {
        OdbcHelper::ReleaseBaseHandles(env->driver_lib_loader, dbc, true);
        dbc->conn_status = CONN_NOT_CONNECTED;
        return SQL_SUCCESS;
    }
    // The app's descriptors lose their base descriptors first, as ReleaseBaseHandles does: psqlODBC before 18.00.0002
    // frees them in its SQLDisconnect, so RDS_FreeConnect, which frees the descriptors the app left, would free them
    // again. A DESC stays the app's until it frees it or the connection; SQLSetStmtAttr allocates a base one again.
    OdbcHelper::FreeWrappedDescs(env->driver_lib_loader, dbc);
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLDisconnect>(env->driver_lib_loader, RdsFuncNames::Disconnect,
        dbc->wrapped_dbc
    );
    ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
    if (SQL_SUCCEEDED(ret)) {
        dbc->conn_status = CONN_NOT_CONNECTED;
    }
    return ret;
}

SQLRETURN SQL_API SQLEndTran(
    SQLSMALLINT    HandleType,
    SQLHANDLE      Handle,
    SQLSMALLINT    CompletionType)
{
    LOG(INFO) << "Entering SQLEndTran";
    return RDS_SQLEndTran(HandleType, Handle, CompletionType);
}

SQLRETURN SQL_API SQLExecute(
    SQLHSTMT       StatementHandle)
{
    LOG(INFO) << "Entering SQLExecute";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    DBC* dbc = stmt->dbc;
    // Cancelable from here, also while the call waits for the connection another statement's call holds
    CancelState::Scope cancel_scope(stmt->cancel);

    // Lock DBC to prevent connection swap race condition
    const std::lock_guard<std::recursive_mutex> lock_guard_dbc(dbc->lock);
    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    // The statement's running call from here: a cancel now stops this call, not one waiting behind it. One that came
    // while it waited stops it before a plugin changes state for it.
    if (cancel_scope.Enter()) {
        return RDS_StatementCanceled(stmt);
    }

    SQLRETURN rc = SQL_ERROR;
    if (dbc->plugin_head) {
        #if UNICODE && !defined(_WIN32)
            RDS_ConvertBoundParamBuffersBeforeExecute(stmt);
        #endif
        rc = dbc->plugin_head->Execute(StatementHandle);
    } else {
        LOG(ERROR) << "Cannot execute without an open connection";
        stmt->err = std::make_unique<ErrInfo>("SQLExecute - Connection not open", ERR_CONNECTION_NOT_OPEN);
    }
    stmt->execute_needs_data = rc == SQL_NEED_DATA;

#if UNICODE && !defined(_WIN32)
    if (SQL_SUCCEEDED(rc)) {
        rc = RDS_ConvertBoundParamBuffersAfterExecute(stmt, rc, "SQLExecute");
    }
#endif

    return rc;
}

SQLRETURN SQL_API SQLExtendedFetch(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   FetchOrientation,
    SQLLEN         FetchOffset,
    SQLULEN *      RowCountPtr,
    SQLUSMALLINT * RowStatusArray)
{
    LOG(INFO) << "Entering SQLExtendedFetch";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLExtendedFetch>(env->driver_lib_loader, RdsFuncNames::ExtendedFetch,
        stmt->wrapped_stmt, FetchOrientation, FetchOffset, RowCountPtr, RowStatusArray
    );

    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
#if UNICODE && !defined(_WIN32)
    if (SQL_SUCCEEDED(ret)) {
        return ConvertBoundColBuffersAfterFetch(stmt, ret, "SQLExtendedFetch");
    }
#endif
    return ret;
}

SQLRETURN SQL_API SQLFetch(
    SQLHSTMT        StatementHandle)
{
    LOG(INFO) << "Entering SQLFetch";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFetch>(env->driver_lib_loader, RdsFuncNames::Fetch,
        stmt->wrapped_stmt
    );

    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
#if UNICODE && !defined(_WIN32)
    if (SQL_SUCCEEDED(ret)) {
        return ConvertBoundColBuffersAfterFetch(stmt, ret, "SQLFetch");
    }
#endif
    return ret;
}

SQLRETURN SQL_API SQLFetchScroll(
    SQLHSTMT       StatementHandle,
    SQLSMALLINT    FetchOrientation,
    SQLLEN         FetchOffset)
{
    LOG(INFO) << "Entering SQLFetchScroll";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFetchScroll>(env->driver_lib_loader, RdsFuncNames::FetchScroll,
        stmt->wrapped_stmt, FetchOrientation, FetchOffset
    );

    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
#if UNICODE && !defined(_WIN32)
    if (SQL_SUCCEEDED(ret)) {
        return ConvertBoundColBuffersAfterFetch(stmt, ret, "SQLFetchScroll");
    }
#endif
    return ret;
}

SQLRETURN SQL_API SQLFreeConnect(
    SQLHDBC        ConnectionHandle)
{
    LOG(INFO) << "Entering SQLFreeConnect";
    return RDS_FreeConnect(ConnectionHandle);
}

SQLRETURN SQL_API SQLFreeEnv(
    SQLHENV        EnvironmentHandle)
{
    LOG(INFO) << "Entering SQLFreeEnv";
    return RDS_FreeEnv(EnvironmentHandle);
}

SQLRETURN SQL_API SQLFreeHandle(
    SQLSMALLINT    HandleType,
    SQLHANDLE      Handle)
{
    LOG(INFO) << "Entering SQLFreeHandle";
    SQLRETURN ret = SQL_ERROR;
    switch (HandleType) {
        case SQL_HANDLE_DBC:
            ret = RDS_FreeConnect(Handle);
            break;
        case SQL_HANDLE_DESC:
            ret = RDS_FreeDesc(Handle);
            break;
        case SQL_HANDLE_ENV:
            ret = RDS_FreeEnv(Handle);
            break;
        case SQL_HANDLE_STMT:
            ret = RDS_FreeStmt(Handle, SQL_DROP);
            break;
        default:
            ret = SQL_ERROR;
    }

    return ret;
}

SQLRETURN SQL_API SQLFreeStmt(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   Option)
{
    LOG(INFO) << "Entering SQLFreeStmt";
    return RDS_FreeStmt(StatementHandle, Option);
}

SQLRETURN SQL_API SQLGetData(
    SQLHSTMT      StatementHandle,
    SQLUSMALLINT  Col_or_Param_Num,
    SQLSMALLINT   TargetType,
    SQLPOINTER    TargetValuePtr,
    SQLLEN        BufferLength,
    SQLLEN *      StrLen_or_IndPtr)
{
    LOG(INFO) << "Entering SQLGetData";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

#if UNICODE
    RdsLibResult res;
    const bool use_4_app = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesUserApp();
    const bool use_4_base = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesBaseDriver();

    if ((use_4_app || use_4_base) && TargetType == SQL_C_TCHAR) {
        std::vector<SQLTCHAR> buffer(static_cast<size_t>(BufferLength) * 2, 0);

        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetData>(env->driver_lib_loader, RdsFuncNames::GetData,
            stmt->wrapped_stmt, Col_or_Param_Num, TargetType, buffer.data(), BufferLength, StrLen_or_IndPtr
        );

        if (SQL_SUCCEEDED(res.fn_result) && StrLen_or_IndPtr && *StrLen_or_IndPtr != SQL_NULL_DATA) {
            SQLTCHAR* dst = reinterpret_cast<SQLTCHAR*>(TargetValuePtr);

            if (use_4_base) {
                Convert4To2ByteString(true, buffer.data(), nullptr, buffer.size());
            }

            const size_t src_len = UShortStrlen(reinterpret_cast<const uint16_t*>(buffer.data()));
            const size_t dst_len = static_cast<size_t>(BufferLength) / sizeof(SQLTCHAR);
            if (dst && dst_len > 0) {
                if (use_4_app) {
                    ConvertUTF16ToUTF32(buffer.data(), dst, src_len, dst_len);
                } else {
                    const size_t copy_len = src_len < dst_len ? src_len : dst_len - 1;
                    std::memcpy(dst, buffer.data(), copy_len * sizeof(SQLTCHAR));
                    dst[copy_len] = 0;
                }
            }
        }
    } else {
        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetData>(env->driver_lib_loader, RdsFuncNames::GetData,
            stmt->wrapped_stmt, Col_or_Param_Num, TargetType, TargetValuePtr, BufferLength, StrLen_or_IndPtr
        );
    }
#else
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetData>(env->driver_lib_loader, RdsFuncNames::GetData,
        stmt->wrapped_stmt, Col_or_Param_Num, TargetType, TargetValuePtr, BufferLength, StrLen_or_IndPtr
    );
#endif

    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLGetEnvAttr(
    SQLHENV        EnvironmentHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     BufferLength,
    SQLINTEGER *   StringLengthPtr)
{
    LOG(INFO) << "Entering SQLGetEnvAttr";
    return RDS_SQLGetEnvAttr(EnvironmentHandle, Attribute, ValuePtr, BufferLength, StringLengthPtr);
}

/*
    TODO - NOT YET IMPLEMENTED
    HARDCODED TO RETURN TRUE / ASSUMES ALL FUNCTIONS IMPLEMENTED
*/
// TODO - Impl SQLGetFunctions
SQLRETURN SQL_API SQLGetFunctions(
    SQLHDBC        ConnectionHandle,
    SQLUSMALLINT   FunctionId,
    SQLUSMALLINT * SupportedPtr)
{
    LOG(INFO) << "Entering SQLGetFunctions";
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    SQLRETURN ret = SQL_ERROR;

    if (dbc == nullptr) {
        return SQL_INVALID_HANDLE;
    }

    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    ClearError(dbc);

    // Query underlying driver if connection is established
    if (dbc->wrapped_dbc) {
        const ENV* env = dbc->env;
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetFunctions>(env->driver_lib_loader, RdsFuncNames::GetFunctions,
            dbc->wrapped_dbc, FunctionId, SupportedPtr
        );
        ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
    } else {
        // TODO - THIS IS HARDCODED
        // Will need to keep track of a map of the current implemented functions
        // as ODBC adds/removes functions in the future
        *SupportedPtr = SQL_TRUE;
        ret = SQL_SUCCESS;
    }

    return ret;
}

SQLRETURN SQL_API SQLGetStmtOption(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   Attribute,
    SQLPOINTER     ValuePtr)
{
    LOG(INFO) << "Entering SQLGetStmtOption";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetStmtOption>(env->driver_lib_loader, RdsFuncNames::GetStmtOption,
        stmt->wrapped_stmt, Attribute, ValuePtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLMoreResults(
    SQLHSTMT       StatementHandle)
{
    LOG(INFO) << "Entering SQLMoreResults";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLMoreResults>(env->driver_lib_loader, RdsFuncNames::MoreResults,
        stmt->wrapped_stmt
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLNumParams(
    SQLHSTMT       StatementHandle,
    SQLSMALLINT *  ParameterCountPtr)
{
    LOG(INFO) << "Entering SQLNumParams";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLNumParams>(env->driver_lib_loader, RdsFuncNames::NumParams,
        stmt->wrapped_stmt, ParameterCountPtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLNumResultCols(
    SQLHSTMT       StatementHandle,
    SQLSMALLINT *  ColumnCountPtr)
{
    LOG(INFO) << "Entering SQLNumResultCols";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLNumResultCols>(env->driver_lib_loader, RdsFuncNames::NumResultCols,
        stmt->wrapped_stmt, ColumnCountPtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLParamData(
    SQLHSTMT       StatementHandle,
    SQLPOINTER *   ValuePtrPtr)
{
    LOG(INFO) << "Entering SQLParamData";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLParamData>(env->driver_lib_loader, RdsFuncNames::ParamData,
        stmt->wrapped_stmt, ValuePtrPtr
    );

#if UNICODE && !defined(_WIN32)
    stmt->put_data_char_conversion = false;
    if (res.fn_result == SQL_NEED_DATA && ValuePtrPtr && *ValuePtrPtr) {
        const bool use_4_base = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesBaseDriver();
        const bool use_4_app = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesUserApp();

        if (use_4_base || use_4_app) {
            // Check if this parameter was bound as SQL_C_TCHAR, in the APD the statement uses
            if (DESC* apd = CurrentParamDesc(stmt); apd != nullptr) {
                const std::lock_guard<std::recursive_mutex> desc_lock_guard(apd->lock);
                for (const auto& p : apd->bound_param_buffers) {
                    if (p.app_ptr == *ValuePtrPtr && p.value_type == SQL_C_TCHAR) {
                        stmt->put_data_char_conversion = true;
                        break;
                    }
                }
            }
        }
    }
#endif

    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
    // Aurora DSQL: as after SQLExecute, a statement that failed in a transaction is checked for before the commit.
    // Racy by design: auto_commit and transaction_status are read without the connection's lock (taking it after the
    // statement's would invert SQLExecute's order). Only a call racing another thread's change of autocommit or of
    // the transaction on this connection reads a stale value; a note set that way costs one check at the commit.
    DsqlCompatibility::NoteStatementResult(stmt->dbc, ret);
    // The last SQLParamData of an SQLExecute or SQLExecDirect ran its statement (or failed to).
    if (stmt->execute_needs_data && ret != SQL_NEED_DATA && ret != SQL_STILL_EXECUTING) {
        stmt->execute_needs_data = false;
#if UNICODE && !defined(_WIN32)
        // Its output parameters come back now, as after those.
        if (SQL_SUCCEEDED(ret)) {
            return RDS_ConvertBoundParamBuffersAfterExecute(stmt, ret, "SQLParamData");
        }
#endif
    }
    return ret;
}

SQLRETURN SQL_API SQLParamOptions(
    SQLHSTMT       StatementHandle,
    SQLULEN        Crow,
    SQLULEN *      FetchOffsetPtr)
{
    LOG(INFO) << "Entering SQLParamOptions";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
#if UNICODE && !defined(_WIN32)
    // Crow is the parameter set size: one per copy of a SQL_C_WCHAR parameter.
    if (RDS_RefuseMultiRowWithCopies(stmt, SQL_ATTR_PARAMSET_SIZE, Crow, "SQLParamOptions") != SQL_SUCCESS) {
        return SQL_ERROR;
    }
#endif
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLParamOptions>(env->driver_lib_loader, RdsFuncNames::ParamOptions,
        stmt->wrapped_stmt, Crow, FetchOffsetPtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLPutData(
    SQLHSTMT       StatementHandle,
    SQLPOINTER     DataPtr,
    SQLLEN         StrLen_or_Ind)
{
    LOG(INFO) << "Entering SQLPutData";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

#if UNICODE && !defined(_WIN32)
    if (stmt->put_data_char_conversion && DataPtr != nullptr && StrLen_or_Ind != SQL_NULL_DATA && StrLen_or_Ind != 0) {
        const bool use_4_base = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesBaseDriver();
        const bool use_4_app = dbc->plugin_service->GetOdbcHelper()->GetUse4BytesUserApp();

        // StrLen_or_Ind is a byte count; the conversion takes characters of the app's SQLWCHAR and reads no more.
        // Another negative value than SQL_NTS sends no text, as before.
        SQLLEN char_len = 0;
        if (StrLen_or_Ind > 0) {
            char_len = StrLen_or_Ind / static_cast<SQLLEN>(use_4_app ? sizeof(SQLTCHAR) * 2 : sizeof(SQLTCHAR));
        } else if (StrLen_or_Ind == SQL_NTS) {
            char_len = SQL_NTS;
        }
        std::vector<SQLTCHAR> local_buf = ConvertUserAppInputToBaseDriver(use_4_app, use_4_base,
            static_cast<SQLTCHAR*>(DataPtr),
            static_cast<SQLINTEGER>(std::min<SQLLEN>(char_len, std::numeric_limits<SQLINTEGER>::max())));

        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLPutData>(
            env->driver_lib_loader, RdsFuncNames::PutData, stmt->wrapped_stmt, local_buf.data(), SQL_NTS);
        return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
    }
#endif

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLPutData>(env->driver_lib_loader, RdsFuncNames::PutData,
        stmt->wrapped_stmt, DataPtr, StrLen_or_Ind
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLRowCount(
    SQLHSTMT       StatementHandle,
    SQLLEN *       RowCountPtr)
{
    LOG(INFO) << "Entering SQLRowCount";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLRowCount>(env->driver_lib_loader, RdsFuncNames::RowCount,
        stmt->wrapped_stmt, RowCountPtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLSetDescRec(
    SQLHDESC      DescriptorHandle,
    SQLSMALLINT   RecNumber,
    SQLSMALLINT   Type,
    SQLSMALLINT   SubType,
    SQLLEN        Length,
    SQLSMALLINT   Precision,
    SQLSMALLINT   Scale,
    SQLPOINTER    DataPtr,
    SQLLEN *      StringLengthPtr,
    SQLLEN *      IndicatorPtr)
{
    LOG(INFO) << "Entering SQLSetDescRec";
    return RDS_SQLSetDescRec(DescriptorHandle, RecNumber, Type, SubType, Length, Precision, Scale, DataPtr, StringLengthPtr, IndicatorPtr);
}

SQLRETURN SQL_API SQLSetEnvAttr(
    SQLHENV        EnvironmentHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     StringLength)
{
    LOG(INFO) << "Entering SQLSetEnvAttr";
    return RDS_SQLSetEnvAttr(EnvironmentHandle, Attribute, ValuePtr, StringLength);
}

SQLRETURN SQL_API SQLSetParam(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   ParameterNumber,
    SQLSMALLINT    ValueType,
    SQLSMALLINT    ParameterType,
    SQLULEN        ColumnSize,
    SQLSMALLINT    DecimalDigits,
    SQLPOINTER     ParameterValuePtr,
    SQLLEN *       StrLen_or_IndPtr)
{
    LOG(INFO) << "Entering SQLSetParam";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetParam>(env->driver_lib_loader, RdsFuncNames::SetParam,
        stmt->wrapped_stmt, ParameterNumber, ValueType, ParameterType, ColumnSize, DecimalDigits, ParameterValuePtr, StrLen_or_IndPtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLSetPos(
    SQLHSTMT       StatementHandle,
    SQLSETPOSIROW  RowNumber,
    SQLUSMALLINT   Operation,
    SQLUSMALLINT   LockType)
{
    LOG(INFO) << "Entering SQLSetPos";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
#if UNICODE && !defined(_WIN32)
    // The base driver reads and writes the row through the ARD's records, which point at the wrapper's copies of the
    // SQL_C_WCHAR columns: it would update or add the last fetch in its own width, not the app's buffers, and a
    // refresh wouldn't reach them.
    if ((Operation == SQL_UPDATE || Operation == SQL_ADD || Operation == SQL_REFRESH)
        && HasOneRowCopies(CurrentRowDesc(stmt))) {
        stmt->err = std::make_unique<ErrInfo>(
            "SQLSetPos - SQL_UPDATE, SQL_ADD and SQL_REFRESH aren't supported with the SQL_C_WCHAR columns the wrapper"
            " converts between the application's and the driver's SQLWCHAR", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
        return SQL_ERROR;
    }
#endif
    // Its own data-at-execution columns, if any: no execute waits for SQLParamData now.
    stmt->execute_needs_data = false;
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetPos>(env->driver_lib_loader, RdsFuncNames::SetPos,
        stmt->wrapped_stmt, RowNumber, Operation, LockType
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLSetScrollOptions(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   Concurrency,
    SQLLEN         KeysetSize,
    SQLUSMALLINT   RowsetSize)
{
    LOG(INFO) << "Entering SQLSetScrollOptions";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
#if UNICODE && !defined(_WIN32)
    // RowsetSize is the rowset size: one row per copy of a SQL_C_WCHAR column.
    if (RDS_RefuseMultiRowWithCopies(stmt, SQL_ATTR_ROW_ARRAY_SIZE, RowsetSize, "SQLSetScrollOptions") != SQL_SUCCESS) {
        return SQL_ERROR;
    }
#endif
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetScrollOptions>(env->driver_lib_loader, RdsFuncNames::SetScrollOptions,
        stmt->wrapped_stmt, Concurrency, KeysetSize, RowsetSize
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLSetStmtOption(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   Option,
    SQLULEN        Param)
{
    LOG(INFO) << "Entering SQLSetStmtOption";
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
#if UNICODE && !defined(_WIN32)
    // SQL_ROWSET_SIZE and SQL_BIND_TYPE, as for SQLSetStmtAttr.
    if (RDS_RefuseMultiRowWithCopies(stmt, Option, Param, "SQLSetStmtOption") != SQL_SUCCESS) {
        return SQL_ERROR;
    }
#endif
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetStmtOption>(env->driver_lib_loader, RdsFuncNames::SetStmtOption,
        stmt->wrapped_stmt, Option, Param
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN SQL_API SQLTransact(
    SQLHENV        EnvironmentHandle,
    SQLHDBC        ConnectionHandle,
    SQLUSMALLINT   CompletionType)
{
    LOG(INFO) << "Entering SQLTransact";
    if (nullptr == EnvironmentHandle && nullptr == ConnectionHandle) {
        return SQL_INVALID_HANDLE;
    }

    return RDS_SQLEndTran(
        ConnectionHandle ? SQL_HANDLE_DBC : SQL_HANDLE_ENV,
        ConnectionHandle ? ConnectionHandle : EnvironmentHandle,
        static_cast<SQLSMALLINT>(CompletionType)
    );
}
