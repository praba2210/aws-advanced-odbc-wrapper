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

#ifndef ODBC_HELPER_H
#define ODBC_HELPER_H

#include "../driver.h"
#include "../odbcapi.h"
#include "rds_lib_loader.h"

#include <functional>
#include <vector>

// Holds a converted SQLTCHAR buffer and a pointer suitable for passing to the
// base driver.  The vector keeps the data alive; `ptr` is nullptr when the
// original input was nullptr (preserving ODBC nullptr semantics).
struct ConvertedSqltchar {
    std::vector<SQLTCHAR> data;
    SQLTCHAR* tchar_ptr = nullptr;

    // The length to pass with tchar_ptr. A converted copy ends in a NUL right after the caller's text, so it goes
    // with SQL_NTS: the caller's length counts the app's characters or bytes (code points for an app with a 4-byte
    // SQLWCHAR), not the base driver's. The caller's length when nothing was converted (ANSI builds, a null input).
    template <typename Length>
    [[nodiscard]] Length BaseLength(Length in_length) const {
        return data.empty() ? in_length : static_cast<Length>(SQL_NTS);
    }
};

// A string OdbcHelper::FetchDriverString got from the base driver for the app.
struct DriverStringResult {
    RdsLibResult res{};      // the base driver's last call
    size_t length = 0;       // the whole string's length for the app, in the function's units
    bool truncated = false;  // the app's buffer holds a cut prefix
};

class OdbcHelper {
public:
    OdbcHelper(const std::shared_ptr<RdsLibLoader>& lib_loader, const ENV* env);

    virtual void Disconnect(DBC *dbc);
    virtual void Disconnect(SQLHDBC *hdbc);
    virtual void DisconnectAndFree(SQLHDBC *hdbc);
    static void InvalidateImplicitDescriptors(STMT *stmt);
    // Store, null or free a published statement's base handle through dbc->base_handles (see STMT::wrapped_stmt).
    static void SetWrappedStmt(STMT *stmt, SQLHSTMT wrapped_stmt);
    // Nulls the base handle, if there is one, and frees it now, or when an SQLCancel using it returns.
    static void FreeWrappedStmt(const std::shared_ptr<RdsLibLoader>& lib_loader, STMT *stmt);
    // Nulls the base handles of the connection's statements and explicit descriptors, and its base connection; then
    // frees the handles and disconnects the base connection (with free_base_dbc, also frees it if the disconnect
    // succeeded), now, or when the SQLCancel calls using them return. The caller holds dbc->lock. Returns the base
    // connection it took out.
    static SQLHDBC ReleaseBaseHandles(const std::shared_ptr<RdsLibLoader>& lib_loader, DBC *dbc, bool free_base_dbc);
    // Gives an explicit descriptor a base descriptor on its connection's base connection, if it has none (releasing a
    // connection's base handles drops them).
    static RdsLibResult AllocWrappedDesc(const std::shared_ptr<RdsLibLoader>& lib_loader, DESC *desc);
    // Makes desc the base statement's ARD or APD (attribute is SQL_ATTR_APP_ROW_DESC or SQL_ATTR_APP_PARAM_DESC): its
    // base descriptor, allocated first if it has none, or the base statement's implicit one for nullptr. The caller
    // holds stmt->lock, and the statement has a base handle.
    static RdsLibResult SetWrappedAppDesc(const std::shared_ptr<RdsLibLoader>& lib_loader, STMT *stmt,
        SQLINTEGER attribute, DESC *desc);

    virtual bool IsClosed(SQLHDBC hdbc);

    virtual SQLRETURN AllocEnv(SQLHENV *henv);
    virtual SQLRETURN FreeEnv(SQLHENV *henv);
    virtual SQLRETURN AllocDbc(SQLHENV &henv, SQLHDBC &hdbc);

    virtual RdsLibResult SetEnvAttr(const ENV *henv, SQLINTEGER attribute, SQLPOINTER pointer, int length);
    virtual RdsLibResult Fetch(SQLHSTMT *stmt);
    virtual RdsLibResult BindCol(const SQLHSTMT *stmt, int column, int type, void *value, size_t size, SQLLEN *len);
    virtual RdsLibResult ExecDirect(const SQLHSTMT *stmt, const std::string &query);

    virtual RdsLibResult CloseCursor(SQLHSTMT stmt);

    virtual RdsLibResult BaseAllocEnv(ENV *env);
    virtual RdsLibResult BaseAllocStmt(const SQLHDBC *wrapped_dbc, SQLHSTMT *stmt);
    virtual RdsLibResult BaseFreeStmt(SQLHSTMT *stmt);

    virtual std::shared_ptr<RdsLibLoader> GetLibLoader();

    [[nodiscard]] bool GetUse4BytesBaseDriver() const;
    [[nodiscard]] bool GetUse4BytesUserApp() const;
    void SetUse4BytesBaseDriver(bool use_4_bytes);

    // Converts a user-app SQLTCHAR* input to the encoding expected by the base
    // driver, respecting the current user_4_byte / driver_4_byte flags.
    // Returns a ConvertedSqltchar whose `tchar_ptr` is nullptr when `in` is nullptr.
    // In non-Unicode builds the input pointer is passed through unchanged.
    ConvertedSqltchar ConvertInput(SQLTCHAR* in, SQLINTEGER in_length) const;

    virtual std::string GetSqlStateAndLogMessage(DBC *dbc);
    virtual std::string GetSqlStateAndLogMessage(DBC *dbc, std::string& out_message);
    virtual std::string GetStmtErrorMessage(SQLHSTMT stmt);

    void ConvertDriverOutputToTarget(const SQLTCHAR* src, SQLTCHAR* dst, size_t src_byte_count, size_t dst_byte_count) const;
    void ConvertDriverOutputToTarget(bool wrapper_call, const SQLTCHAR* src, SQLTCHAR* dst, size_t src_byte_count, size_t dst_byte_count) const;

    void ConvertWrapperOutputToTarget(SQLTCHAR* buf, size_t char_count, size_t buf_byte_count) const;
    void ConvertWrapperOutputToTarget(bool wrapper_call, SQLTCHAR* buf, size_t char_count, size_t buf_byte_count) const;
    static void ConvertWrapperOutputToTarget(bool user_4_byte, bool wrapper_call, SQLTCHAR* buf, size_t char_count, size_t buf_byte_count);

    // Returns true if driver and user app character widths differ and conversion is needed.
    [[nodiscard]] bool NeedsConversion() const;

    [[nodiscard]] size_t GetTargetByteCount(bool wrapper_call, size_t char_count) const;

#if UNICODE
    // For a function that returns a string, when the app's characters and the base driver's differ in size. call(buffer,
    // length, &full_length) calls the base driver with a buffer of its own and that buffer's BufferLength, and sets the
    // string's full length; buffer_length, the lengths and the result count characters, or bytes of them with
    // count_bytes, as the function does, and max_length caps what the base driver is given. The string goes to app_out
    // in the app's characters (2-byte ones for a wrapper call), cut to a terminated prefix that fits buffer_length and
    // doesn't split a surrogate pair, and the length counts the whole string in them: code points for a 4-byte SQLWCHAR,
    // UTF-16 units for a 2-byte one. When the base driver cut the string (or app_out is null) it is called once more,
    // with room for all of it.
    DriverStringResult FetchDriverString(
        const std::function<RdsLibResult(SQLTCHAR* buffer, size_t length, size_t* full_length)>& call,
        void* app_out, size_t buffer_length, bool count_bytes, size_t max_length, bool wrapper_call = false) const;
#endif

    static bool IsStringConnectAttr(SQLINTEGER attribute);
    static bool IsStringDescField(SQLSMALLINT field_identifier);
    static bool IsStringDiagField(SQLSMALLINT diag_identifier);
    // The info types whose value is a character string: the list both driver managers convert for SQLGetInfo.
    static bool IsStringInfoType(SQLUSMALLINT info_type);
    // The SQLColAttribute fields whose value is a character string, ODBC 2 SQL_COLUMN_NAME included.
    static bool IsStringColAttribute(SQLUSMALLINT field_identifier);

private:
    std::shared_ptr<RdsLibLoader> lib_loader_;
    const ENV *env_ = nullptr;
    bool use_4_bytes_base_driver_ = false;
};

#endif //ODBC_HELPER_H
