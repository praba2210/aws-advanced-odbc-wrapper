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

#include "../../driver/util/windows_headers.h"

#include <sqlext.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../../driver/driver.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/error.h"
#include "../../driver/host_list_providers/host_list_provider.h"
#include "../../driver/plugin/base_plugin.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/odbc_dsn_helper.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"
#include "../../driver/util/rds_strings.h"

class ErrorHandlingTest : public testing::Test {
protected:
    ENV* env_ = nullptr;
    DBC* dbc_ = nullptr;
    STMT* stmt_ = nullptr;
    DESC* desc_ = nullptr;

    void SetUp() override {
        env_ = new ENV();
        dbc_ = new DBC();
        dbc_->env = env_;
        stmt_ = new STMT();
        stmt_->dbc = dbc_;
        desc_ = new DESC();
        desc_->dbc = dbc_;
    }

    void TearDown() override {
        delete desc_;
        delete stmt_;
        delete dbc_;
        delete env_;
    }
};

// HasEnvAccess tests

TEST_F(ErrorHandlingTest, HasEnvAccess_NullEnv) {
    EXPECT_FALSE(HasEnvAccess<ENV>(nullptr));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_ValidEnv) {
    EXPECT_TRUE(HasEnvAccess<ENV>(env_));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_NullDbc) {
    EXPECT_FALSE(HasEnvAccess<DBC>(nullptr));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_DbcNullEnv) {
    DBC local_dbc{};
    local_dbc.env = nullptr;
    EXPECT_FALSE(HasEnvAccess<DBC>(&local_dbc));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_ValidDbc) {
    EXPECT_TRUE(HasEnvAccess<DBC>(dbc_));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_NullStmt) {
    EXPECT_FALSE(HasEnvAccess<STMT>(nullptr));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_StmtNullDbc) {
    STMT local_stmt{};
    local_stmt.dbc = nullptr;
    EXPECT_FALSE(HasEnvAccess<STMT>(&local_stmt));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_StmtDbcNullEnv) {
    DBC local_dbc{};
    local_dbc.env = nullptr;
    STMT local_stmt{};
    local_stmt.dbc = &local_dbc;
    EXPECT_FALSE(HasEnvAccess<STMT>(&local_stmt));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_ValidStmt) {
    EXPECT_TRUE(HasEnvAccess<STMT>(stmt_));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_NullDesc) {
    EXPECT_FALSE(HasEnvAccess<DESC>(nullptr));
}

TEST_F(ErrorHandlingTest, HasEnvAccess_ValidDesc) {
    EXPECT_TRUE(HasEnvAccess<DESC>(desc_));
}

// HasWrappedHandle tests

TEST_F(ErrorHandlingTest, HasWrappedHandle_EnvNull) {
    EXPECT_FALSE(HasWrappedHandle(env_));
}

TEST_F(ErrorHandlingTest, HasWrappedHandle_DbcNull) {
    EXPECT_FALSE(HasWrappedHandle(dbc_));
}

TEST_F(ErrorHandlingTest, HasWrappedHandle_StmtNull) {
    EXPECT_FALSE(HasWrappedHandle(stmt_));
}

TEST_F(ErrorHandlingTest, HasWrappedHandle_DescNull) {
    EXPECT_FALSE(HasWrappedHandle(desc_));
}

// ClearError tests

TEST_F(ErrorHandlingTest, ClearError_NullSafe) {
    ClearError(static_cast<ENV*>(nullptr));
    ClearError(static_cast<DBC*>(nullptr));
    ClearError(static_cast<STMT*>(nullptr));
    ClearError(static_cast<DESC*>(nullptr));
}

TEST_F(ErrorHandlingTest, ClearError_ResetsEnv) {
    env_->err = std::make_unique<ErrInfo>("test", ERR_GENERAL_ERROR);
    env_->sql_error_called = 1;
    ClearError(env_);
    EXPECT_FALSE(env_->err);
    EXPECT_EQ(0, env_->sql_error_called);
}

TEST_F(ErrorHandlingTest, ClearError_ResetsDbc) {
    dbc_->err = std::make_unique<ErrInfo>("test", ERR_GENERAL_ERROR);
    dbc_->sql_error_called = 1;
    ClearError(dbc_);
    EXPECT_FALSE(dbc_->err);
    EXPECT_EQ(0, dbc_->sql_error_called);
}

TEST_F(ErrorHandlingTest, ClearError_ResetsStmt) {
    stmt_->err = std::make_unique<ErrInfo>("test", ERR_GENERAL_ERROR);
    stmt_->sql_error_called = 1;
    ClearError(stmt_);
    EXPECT_FALSE(stmt_->err);
    EXPECT_EQ(0, stmt_->sql_error_called);
}

TEST_F(ErrorHandlingTest, ClearError_ResetsDesc) {
    desc_->err = std::make_unique<ErrInfo>("test", ERR_GENERAL_ERROR);
    desc_->sql_error_called = 1;
    ClearError(desc_);
    EXPECT_FALSE(desc_->err);
    EXPECT_EQ(0, desc_->sql_error_called);
}

// NextErrorRecord tests

TEST_F(ErrorHandlingTest, NextErrorRecord_NullReturnsZero) {
    EXPECT_EQ(0, NextErrorRecord<ENV>(nullptr));
    EXPECT_EQ(0, NextErrorRecord<DBC>(nullptr));
    EXPECT_EQ(0, NextErrorRecord<STMT>(nullptr));
    EXPECT_EQ(0, NextErrorRecord<DESC>(nullptr));
}

TEST_F(ErrorHandlingTest, NextErrorRecord_FirstCallReturnsOne) {
    env_->sql_error_called = 0;
    EXPECT_EQ(1, NextErrorRecord<ENV>(env_));
    EXPECT_EQ(1, env_->sql_error_called);
}

TEST_F(ErrorHandlingTest, NextErrorRecord_SecondCallReturnsZero) {
    env_->sql_error_called = 0;
    NextErrorRecord<ENV>(env_);
    EXPECT_EQ(0, NextErrorRecord<ENV>(env_));
}

// Invalid handle return tests

TEST_F(ErrorHandlingTest, RdsAllocDbc_NullEnvReturnsInvalidHandle) {
    SQLHDBC out = nullptr;
    EXPECT_EQ(SQL_INVALID_HANDLE, RDS_AllocDbc(nullptr, &out));
}

TEST_F(ErrorHandlingTest, RdsFreeEnv_NullReturnsInvalidHandle) {
    EXPECT_EQ(SQL_INVALID_HANDLE, RDS_FreeEnv(nullptr));
}

// ERR_INFO unique_ptr ownership tests

TEST_F(ErrorHandlingTest, UniquePtr_OwnershipTransfer) {
    auto err = std::make_unique<ErrInfo>("original", ERR_GENERAL_ERROR);
    stmt_->err = std::move(err);
    EXPECT_TRUE(stmt_->err);
    EXPECT_STREQ("original", stmt_->err->error_msg);
    EXPECT_STREQ("HY000", stmt_->err->sqlstate);
}

TEST_F(ErrorHandlingTest, UniquePtr_ReassignmentDeletesPrevious) {
    stmt_->err = std::make_unique<ErrInfo>("first", ERR_GENERAL_ERROR);
    stmt_->err = std::make_unique<ErrInfo>("second", ERR_MEMORY_ALLOCATION_ERROR);
    EXPECT_STREQ("second", stmt_->err->error_msg);
    EXPECT_STREQ("HY001", stmt_->err->sqlstate);
}

TEST_F(ErrorHandlingTest, UniquePtr_ResetCleansUp) {
    stmt_->err = std::make_unique<ErrInfo>("test", ERR_GENERAL_ERROR);
    stmt_->err.reset();
    EXPECT_FALSE(stmt_->err);
}

// SQLGetDiagRec returns correct messages

TEST_F(ErrorHandlingTest, DiagRec_ReturnsErrMessage) {
    dbc_->err = std::make_unique<ErrInfo>("Connection failed", ERR_COMMUNICATION_LINK_FAILURE);

    SQLTCHAR sql_state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT text_length = 0;

    SQLRETURN ret = RDS_SQLGetDiagRec(
        SQL_HANDLE_DBC, dbc_, 1,
        sql_state, &native_error, message, MAX_MSG_LEN, &text_length, false);

    EXPECT_EQ(SQL_SUCCESS, ret);
#ifndef UNICODE
    EXPECT_STREQ("08S01", reinterpret_cast<const char*>(sql_state));
    EXPECT_STREQ("Connection failed", reinterpret_cast<const char*>(message));
#endif
    EXPECT_EQ(static_cast<SQLINTEGER>(ERR_COMMUNICATION_LINK_FAILURE), native_error);
}

TEST_F(ErrorHandlingTest, DiagRec_RecordTwoReturnsNoData) {
    dbc_->err = std::make_unique<ErrInfo>("test", ERR_GENERAL_ERROR);

    SQLTCHAR sql_state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT text_length = 0;

    SQLRETURN ret = RDS_SQLGetDiagRec(
        SQL_HANDLE_DBC, dbc_, 2,
        sql_state, &native_error, message, MAX_MSG_LEN, &text_length, false);

    EXPECT_EQ(SQL_NO_DATA, ret);
}

TEST_F(ErrorHandlingTest, InitializeConnection_UnloadableDriverReturnsErrorDiagnostic) {
    dbc_->conn_attr[KEY_BASE_DRIVER] = "/nonexistent/path/no-such-driver.so";

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("IM003", dbc_->err->sqlstate);
    const std::string message(dbc_->err->error_msg);
    EXPECT_NE(std::string::npos, message.find("/nonexistent/path/no-such-driver.so"));
    EXPECT_FALSE(env_->driver_lib_loader);
}

TEST_F(ErrorHandlingTest, InitializeConnection_UnregisteredDriverNameReturnsErrorDiagnostic) {
    dbc_->conn_attr[KEY_BASE_DRIVER] = "DefinitelyNotARegisteredOdbcDriver123";

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_STREQ("IM003", dbc_->err->sqlstate);
    const std::string message(dbc_->err->error_msg);
    EXPECT_NE(std::string::npos, message.find("DefinitelyNotARegisteredOdbcDriver123"));
    EXPECT_FALSE(env_->driver_lib_loader);
}

TEST_F(ErrorHandlingTest, InitializeConnection_NoDriverKeepsExistingDiagnostic) {
    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ(static_cast<SQLINTEGER>(ERR_NO_UNDER_LYING_DRIVER), dbc_->err->native_err);
}

TEST_F(ErrorHandlingTest, DiagRec_InvalidHandleType) {
    SQLTCHAR sql_state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT text_length = 0;
    
    const SQLSMALLINT invalid_handle_type = 999;
    SQLRETURN ret = RDS_SQLGetDiagRec(invalid_handle_type, dbc_, 1, sql_state, &native_error, message, MAX_MSG_LEN, &text_length, false);

    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
}

namespace {
    // Leaves runs of text, each ending in two NULs, on the stack below the caller, where the next call's locals go,
    // so a buffer that nothing wrote holds text rather than zeros that happened to be there. Called through a
    // volatile pointer, so that it isn't inlined into the caller.
    void FillStackWithText() {
        volatile char16_t junk[2 * MAX_VAL_SIZE];
        for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); ++i) {
            junk[i] = i % 8 < 6 ? u'x' : u'\0';
        }
    }
    void (*volatile fill_stack)() = FillStackWithText;
}  // namespace

// A DSN the driver manager doesn't know adds nothing, and its entries are empty. unixODBC's
// SQLGetPrivateProfileStringW writes the buffer only for a non-empty result, so the Unicode driver used to read
// whatever the stack held.
TEST(DsnLoadTest, LoadAll_UnknownDsnAddsNothing) {
    std::map<std::string, std::string> conn_attr = {{KEY_SERVER, "kept.example.com"}};
    const std::string dsn = "no-such-dsn-dsn-load-test";
    fill_stack();
    OdbcDsnHelper::LoadAll(dsn, conn_attr);
    ASSERT_EQ(1U, conn_attr.size());
    EXPECT_EQ("kept.example.com", conn_attr.at(KEY_SERVER));
}

TEST(DsnLoadTest, Load_UnknownEntryIsEmpty) {
    const std::string dsn = "no-such-dsn-dsn-load-test";
    const std::string entry = KEY_SERVER;
    fill_stack();
    const std::string value = OdbcDsnHelper::Load(dsn, entry);
    EXPECT_EQ("", value);
}

namespace {
    // A string the wrapper wrote for the app, as UTF-8: a W build writes SQLWCHARs.
    std::string AppText(SQLTCHAR* buffer) {
#ifdef UNICODE
        return ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(buffer));
#else
        return reinterpret_cast<const char*>(buffer);
#endif
    }
}  // namespace

// Both driver managers pass SQLGetDiagField for a driver's record to the driver (unixODBC answers only
// SQL_DIAG_NUMBER itself). The wrapper's record comes first, as RDS_SQLGetDiagRec returns it: asking the base driver
// first, whenever the handle had an underlying one, reported no record.
TEST_F(ErrorHandlingTest, DiagField_DbcWrapperRecordBeforeBaseDriver) {
    int fake_handle = 0;
    dbc_->wrapped_dbc = &fake_handle;  // never called through
    dbc_->err = std::make_unique<ErrInfo>("Option value changed", WARN_OPTION_VALUE_CHANGED);
    const std::string expected(dbc_->err->sqlstate);

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_SQLSTATE, state,
        static_cast<SQLSMALLINT>(sizeof(state)), nullptr));
    EXPECT_EQ(expected, AppText(state));

    SQLTCHAR message[MAX_MSG_LEN] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_MESSAGE_TEXT, message,
        static_cast<SQLSMALLINT>(sizeof(message)), nullptr));
    EXPECT_EQ("Option value changed", AppText(message));

    SQLINTEGER count = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 0, SQL_DIAG_NUMBER, &count, 0, nullptr));
    EXPECT_EQ(1, count);

    dbc_->wrapped_dbc = SQL_NULL_HDBC;
}

// The same for a statement: wrapper records on statements the base driver knows (failover's 08S02, HYC00).
TEST_F(ErrorHandlingTest, DiagField_StmtWrapperRecordBeforeBaseDriver) {
    int fake_handle = 0;
    stmt_->wrapped_stmt = &fake_handle;  // never called through
    stmt_->err = std::make_unique<ErrInfo>("Optional feature not implemented", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
    const std::string expected(stmt_->err->sqlstate);

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SQLSTATE, state,
        static_cast<SQLSMALLINT>(sizeof(state)), nullptr));
    EXPECT_EQ(expected, AppText(state));

    SQLINTEGER native = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_NATIVE, &native, 0, nullptr));
    EXPECT_EQ(static_cast<SQLINTEGER>(ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED), native);

    stmt_->wrapped_stmt = SQL_NULL_HSTMT;
}

// String fields come back in the caller's character type, as RDS_SQLGetDiagRec writes them: SQLWCHARs in a W build,
// 4-byte ones for an app with a 4-byte SQLWCHAR. BufferLength and the returned length count bytes.
TEST_F(ErrorHandlingTest, DiagField_StringFieldsUseTheAppCharacterType) {
    stmt_->err = std::make_unique<ErrInfo>("SQLExecDirect - Connection not open", ERR_CONNECTION_NOT_OPEN);
    const std::string expected(stmt_->err->sqlstate);
    const auto expected_bytes = static_cast<SQLSMALLINT>(expected.size() * sizeof(SQLTCHAR));

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SQLSTATE, state,
        static_cast<SQLSMALLINT>(sizeof(state)), &length));
    EXPECT_EQ(expected, AppText(state));
    EXPECT_EQ(expected_bytes, length);

    // Room for 3 characters and the terminator: truncated, and the full length is returned.
    SQLTCHAR short_state[4] = {0};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SQLSTATE, short_state,
        static_cast<SQLSMALLINT>(sizeof(short_state)), &length));
    EXPECT_EQ(expected_bytes, length);

#ifdef UNICODE
    env_->use_4_bytes_user_app = true;
    uint32_t wide_state[MAX_SQL_STATE_LEN] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SQLSTATE, wide_state,
        static_cast<SQLSMALLINT>(sizeof(wide_state)), &length));
    EXPECT_EQ(expected, std::string(wide_state, wide_state + expected.size()));
    EXPECT_EQ(0U, wide_state[expected.size()]);
    EXPECT_EQ(static_cast<SQLSMALLINT>(expected.size() * sizeof(uint32_t)), length);
    env_->use_4_bytes_user_app = false;
#endif
}

// SQL_DIAG_COLUMN_NUMBER is an SQLINTEGER (psqlODBC writes it as one); an SQLLEN write overruns it.
TEST_F(ErrorHandlingTest, DiagField_ColumnNumberIsSqlInteger) {
    constexpr SQLINTEGER GUARD = 0x5A5A5A5A;
    stmt_->err = std::make_unique<ErrInfo>("test", ERR_GENERAL_ERROR);
    struct {
        SQLINTEGER column = 0;
        SQLINTEGER guard = GUARD;
    } out;

    EXPECT_EQ(SQL_SUCCESS,
        RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_COLUMN_NUMBER, &out.column, 0, nullptr));
    EXPECT_EQ(SQL_COLUMN_NUMBER_UNKNOWN, out.column);
    EXPECT_EQ(GUARD, out.guard);
}

// An application that reads records until SQL_NO_DATA stops after the wrapper's one record, also on a handle with an
// underlying one: the base driver isn't asked for a record 2 it never had.
TEST_F(ErrorHandlingTest, DiagField_DbcRecordTwoIsNoData) {
    int fake_handle = 0;
    dbc_->wrapped_dbc = &fake_handle;  // never called through
    dbc_->err = std::make_unique<ErrInfo>("Option value changed", WARN_OPTION_VALUE_CHANGED);

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 2, SQL_DIAG_SQLSTATE, state,
        static_cast<SQLSMALLINT>(sizeof(state)), nullptr));

    dbc_->wrapped_dbc = SQL_NULL_HDBC;
}

// A statement record the wrapper posted came from a call it answered itself, which executed nothing: the statement
// header fields are no rows and an unknown statement, with SQL_SUCCESS. Other handles don't have these fields.
TEST_F(ErrorHandlingTest, DiagField_StatementHeaderFieldsWithAWrapperRecord) {
    stmt_->err = std::make_unique<ErrInfo>("Optional feature not implemented", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);

    SQLLEN rows = 77;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 0, SQL_DIAG_ROW_COUNT, &rows, 0, nullptr));
    EXPECT_EQ(0, rows);
    SQLLEN cursor_rows = 77;
    EXPECT_EQ(SQL_SUCCESS,
        RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 0, SQL_DIAG_CURSOR_ROW_COUNT, &cursor_rows, 0, nullptr));
    EXPECT_EQ(0, cursor_rows);
    SQLINTEGER code = 77;
    EXPECT_EQ(SQL_SUCCESS,
        RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 0, SQL_DIAG_DYNAMIC_FUNCTION_CODE, &code, 0, nullptr));
    EXPECT_EQ(SQL_DIAG_UNKNOWN_STATEMENT, code);
    SQLTCHAR function[16] = {'x'};
    SQLSMALLINT length = 77;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 0, SQL_DIAG_DYNAMIC_FUNCTION, function,
        static_cast<SQLSMALLINT>(sizeof(function)), &length));
    EXPECT_EQ("", AppText(function));
    EXPECT_EQ(0, length);

    dbc_->err = std::make_unique<ErrInfo>("Option value changed", WARN_OPTION_VALUE_CHANGED);
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 0, SQL_DIAG_ROW_COUNT, &rows, 0, nullptr));
}

// SQL_DIAG_SUBCLASS_ORIGIN is "ODBC 3.0" for the SQLSTATEs ODBC defines, such as 01S02, and "ISO 9075" for the rest.
TEST_F(ErrorHandlingTest, DiagField_SubclassOriginFollowsTheSqlstate) {
    dbc_->err = std::make_unique<ErrInfo>("Option value changed", WARN_OPTION_VALUE_CHANGED);
    SQLTCHAR origin[16] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_SUBCLASS_ORIGIN, origin,
        static_cast<SQLSMALLINT>(sizeof(origin)), nullptr));
    EXPECT_EQ("ODBC 3.0", AppText(origin));

    stmt_->err = std::make_unique<ErrInfo>("test", ERR_GENERAL_ERROR);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SUBCLASS_ORIGIN, origin,
        static_cast<SQLSMALLINT>(sizeof(origin)), nullptr));
    EXPECT_EQ("ISO 9075", AppText(origin));
}

// The environment and descriptor handles read the wrapper's record first too. Through a driver manager the wrapper's
// ENV and DESC records come only from a function the base driver doesn't export; the order is the same everywhere.
TEST_F(ErrorHandlingTest, DiagField_EnvWrapperRecordBeforeBaseDriver) {
    int fake_handle = 0;
    env_->wrapped_env = &fake_handle;  // never called through
    env_->err = std::make_unique<ErrInfo>("SQLCancelHandle ENV - API Unsupported",
        ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
    const std::string expected(env_->err->sqlstate);

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_ENV, env_, 1, SQL_DIAG_SQLSTATE, state,
        static_cast<SQLSMALLINT>(sizeof(state)), nullptr));
    EXPECT_EQ(expected, AppText(state));
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagField(SQL_HANDLE_ENV, env_, 2, SQL_DIAG_SQLSTATE, state,
        static_cast<SQLSMALLINT>(sizeof(state)), nullptr));

    env_->wrapped_env = SQL_NULL_HENV;
}

TEST_F(ErrorHandlingTest, DiagField_DescWrapperRecordBeforeBaseDriver) {
    int fake_handle = 0;
    desc_->wrapped_desc = &fake_handle;  // never called through
    desc_->err = std::make_unique<ErrInfo>("SQLCancelHandle DESC - API Unsupported",
        ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
    const std::string expected(desc_->err->sqlstate);

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DESC, desc_, 1, SQL_DIAG_SQLSTATE, state,
        static_cast<SQLSMALLINT>(sizeof(state)), nullptr));
    EXPECT_EQ(expected, AppText(state));

    SQLINTEGER native = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DESC, desc_, 1, SQL_DIAG_NATIVE, &native, 0, nullptr));
    EXPECT_EQ(static_cast<SQLINTEGER>(ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED), native);

    desc_->wrapped_desc = SQL_NULL_HDESC;
}

// A string field with a negative BufferLength is an error and nothing is written; an integer field ignores it.
TEST_F(ErrorHandlingTest, DiagField_NegativeBufferLengthIsAnError) {
    stmt_->err = std::make_unique<ErrInfo>("SQLExecDirect - Connection not open", ERR_CONNECTION_NOT_OPEN);

    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT length = 77;
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_MESSAGE_TEXT, message, -1, &length));
    EXPECT_EQ(0, message[0]);
    EXPECT_EQ(77, length);

    // Also for a string field the record has no value for (no SERVER here).
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SERVER_NAME, message, -1, &length));

    SQLINTEGER native = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_NATIVE, &native, -1, nullptr));
    EXPECT_EQ(static_cast<SQLINTEGER>(ERR_CONNECTION_NOT_OPEN), native);
}

namespace {
    // The value the base driver puts in PWD, a token or a secret. No part of the base driver's string may reach the
    // app.
    constexpr const char* TOKEN = "SECRET-TOKEN-7";

    // Text as the caller's characters, with NULs after it: a 4-byte read of a 2-byte string stops inside the buffer.
    std::vector<SQLTCHAR> AppString(const std::string& text) {
#ifdef UNICODE
        const std::vector<uint16_t> utf16 = ConvertUTF8ToUTF16(text);
        std::vector<SQLTCHAR> out(utf16.begin(), utf16.end());
#else
        std::vector<SQLTCHAR> out(text.begin(), text.end());
#endif
        out.resize(out.size() + 4, 0);
        return out;
    }

#ifdef UNICODE
    // A connection string as an app with a 4-byte SQLWCHAR passes it (ASCII here), with NULs after it.
    std::vector<SQLTCHAR> AppString4(const std::string& text) {
        std::vector<SQLTCHAR> out;
        for (const char c : text) {
            out.push_back(static_cast<SQLTCHAR>(static_cast<unsigned char>(c)));
            out.push_back(0);
        }
        out.resize(out.size() + 4, 0);
        return out;
    }

    // A 4-byte string the wrapper wrote (ASCII here), up to its terminator.
    std::string AppText4(const uint32_t* buffer) {
        std::string text;
        for (; *buffer != 0; ++buffer) {
            text.push_back(static_cast<char>(*buffer));
        }
        return text;
    }
#endif

    // True when the text's characters appear anywhere in the buffer, as 1-, 2- or 4-byte little-endian characters.
    bool BufferHolds(const void* buffer, size_t byte_count, const std::string& text) {
        const auto* begin = static_cast<const unsigned char*>(buffer);
        const auto* end = begin + byte_count;
        for (const size_t width : {1U, 2U, 4U}) {
            std::vector<unsigned char> pattern;
            for (const char c : text) {
                pattern.push_back(static_cast<unsigned char>(c));
                pattern.insert(pattern.end(), width - 1, static_cast<unsigned char>(0));
            }
            if (std::search(begin, end, pattern.begin(), pattern.end()) != end) {
                return true;
            }
        }
        return false;
    }

    // Stands in for the plugin chain and the base driver. It records the attributes each connect gets, leaves keys
    // behind as token plugins and connection defaults do, and writes its own output string into the app's buffer as
    // psqlODBC does: the whole string and NULs after it when it fits, else NULs only (17.00.0005 and later; earlier
    // versions write a cut string, which a short base_out models).
    class ConnectRecorder : public BasePlugin {
    public:
        SQLRETURN Connect(SQLHDBC connection_handle, SQLHWND /*window_handle*/, SQLTCHAR* out_connection_string,
                          SQLSMALLINT buffer_length, SQLSMALLINT* string_length_ptr,
                          SQLUSMALLINT /*driver_completion*/) override {
            DBC* dbc = static_cast<DBC*>(connection_handle);
            seen.push_back(dbc->conn_attr);
            for (const auto& [key, value] : added) {
                dbc->conn_attr.insert_or_assign(key, value);
            }
            if (out_connection_string != nullptr && buffer_length > 0) {
                const std::vector<SQLTCHAR> text = AppString(base_out);
                const bool fits = base_out.size() < static_cast<size_t>(buffer_length);
                for (size_t i = 0; i < static_cast<size_t>(buffer_length); ++i) {
                    out_connection_string[i] = fits && i < base_out.size() ? text[i] : static_cast<SQLTCHAR>(0);
                }
            }
            if (string_length_ptr != nullptr) {
                *string_length_ptr = static_cast<SQLSMALLINT>(base_out.size());
            }
            if (warning != nullptr) {
                dbc->err = std::make_unique<ErrInfo>(warning, WARN_OPTION_VALUE_CHANGED);
                return SQL_SUCCESS_WITH_INFO;
            }
            return result;
        }

        std::vector<std::map<std::string, std::string>> seen;
        std::map<std::string, std::string> added;
        std::string base_out;
        const char* warning = nullptr;
        SQLRETURN result = SQL_SUCCESS;
    };

    // What RDS_SQLDriverConnect calls on the plugin service an earlier connect built, without topology.
    class ConnectOnlyPluginService : public PluginService {
    public:
        explicit ConnectOnlyPluginService(const ENV* env) : odbc_helper_(std::make_shared<OdbcHelper>(nullptr, env)) {}

        std::shared_ptr<OdbcHelper> GetOdbcHelper() override { return odbc_helper_; }
        std::shared_ptr<HostListProvider> GetHostListProvider() override { return host_list_provider_; }
        void InitHostListProvider() override {}
        void SetCurrentHostInfo(const HostInfo& /*info*/) override {}

    private:
        std::shared_ptr<OdbcHelper> odbc_helper_;
        std::shared_ptr<HostListProvider> host_list_provider_ = std::make_shared<HostListProvider>("connect-path-test");
    };
}  // namespace

// RDS_SQLDriverConnect and RDS_SQLConnect on a handle whose plugin chain an earlier connect built
// (RDS_InitializeConnection keeps it), with no base driver loaded.
class ConnectPathTest : public testing::Test {
protected:
    static constexpr const char* BASE_DRIVER_PATH = "/nonexistent/connect-path-test/libbase.so";

    ENV env_;
    DBC dbc_{};
    std::shared_ptr<ConnectRecorder> plugin_ = std::make_shared<ConnectRecorder>();
    int fake_env_ = 0;

    void SetUp() override {
        // RDS_InitializeConnection compares this path with BASE_DRIVER; no library is there, so nothing is loaded.
        env_.driver_lib_loader = std::make_shared<RdsLibLoader>(BASE_DRIVER_PATH);
        env_.wrapped_env = &fake_env_;  // never called through
        // The base driver's string as psqlODBC builds it, with PWD past the end of the wrapper's string.
        plugin_->base_out = std::string("DRIVER={PostgreSQL Unicode};DATABASE=postgres;SERVER=") + std::string(120, 'b')
            + ".example.com;PORT=5432;UID=admin;PWD=" + TOKEN + ";SSLmode=verify-full";
        Wire(dbc_);
    }

    void TearDown() override {
        dbc_.plugin_head = nullptr;
        env_.wrapped_env = SQL_NULL_HENV;
    }

    void Wire(DBC& dbc) {
        dbc.env = &env_;
        dbc.plugin_service = std::make_shared<ConnectOnlyPluginService>(&env_);
        dbc.plugin_head = plugin_.get();
    }

    static std::string ConnStr(const std::string& server, const std::string& more = "") {
        return std::string("DRIVER={AWS Advanced ODBC Wrapper};BASE_DRIVER=") + BASE_DRIVER_PATH + ";SERVER=" + server
            + more;
    }

    static SQLRETURN DriverConnect(DBC& dbc, const std::string& conn_str, SQLTCHAR* out, SQLSMALLINT buffer_length,
                                   SQLSMALLINT* length) {
        std::vector<SQLTCHAR> in = AppString(conn_str);
        return RDS_SQLDriverConnect(&dbc, nullptr, in.data(), SQL_NTS, out, buffer_length, length, SQL_DRIVER_NOPROMPT);
    }

    // The wrapper's whole output string for conn_str (as UTF-8) and its length in the app's characters, from a connect
    // on a handle of its own.
    std::string FullOutput(const std::string& conn_str, SQLSMALLINT* length) {
        DBC reference{};
        Wire(reference);
        std::vector<SQLTCHAR> out(MAX_MSG_LEN, 0);
        EXPECT_EQ(SQL_SUCCESS, DriverConnect(reference, conn_str, out.data(), static_cast<SQLSMALLINT>(out.size()),
            length));
        return AppText(out.data());
    }
};

// The wrapper writes its own output string over the base driver's. When it doesn't fit, SQL_SUCCESS_WITH_INFO comes
// with a 01004 record, and the buffer holds the string's first characters and a terminator: here, room for every
// character but the terminator.
TEST_F(ConnectPathTest, DriverConnect_TruncatedOutputStringHasARecord) {
    const std::string conn_str = ConnStr("truncation.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);
    ASSERT_GT(full_length, 1);

    std::vector<SQLTCHAR> out(static_cast<size_t>(full_length), 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, DriverConnect(dbc_, conn_str, out.data(), full_length, &length));
    EXPECT_EQ(full_length, length);
    ASSERT_EQ(0, out.back());  // the terminator, before reading the buffer as a string
    EXPECT_EQ(full.substr(0, full.size() - 1), AppText(out.data()));
    ASSERT_TRUE(dbc_.err);
    EXPECT_STREQ("01004", dbc_.err->sqlstate);
    EXPECT_EQ("SQLDriverConnect - The output connection string was truncated: the buffer needs "
        + std::to_string(full_length + 1) + " characters", std::string(dbc_.err->error_msg));

    // SQLGetDiagRec returns it as record 1.
    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native = 0;
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT message_length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state, &native, message, MAX_MSG_LEN,
        &message_length, false));
    EXPECT_EQ("01004", AppText(state));
    EXPECT_EQ(static_cast<SQLINTEGER>(WARN_STRING_DATA_RIGHT_TRUNCATED), native);
}

// A string that fits has no record. "é" is two UTF-8 bytes and one SQLWCHAR: in a W build, a buffer of the string's
// characters and the terminator is enough.
TEST_F(ConnectPathTest, DriverConnect_OutputStringThatFitsHasNoRecord) {
    const std::string conn_str = ConnStr("fit-\xC3\xA9.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);

    std::vector<SQLTCHAR> out(static_cast<size_t>(full_length) + 1, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS,
        DriverConnect(dbc_, conn_str, out.data(), static_cast<SQLSMALLINT>(out.size()), &length));
    EXPECT_EQ(full_length, length);
    EXPECT_FALSE(dbc_.err);
    EXPECT_EQ(full, AppText(out.data()));
}

// The wrapper keeps one record per handle: a warning the connect returned stays, and so does its return code. The
// buffer still gets the wrapper's first characters, and nothing of the base driver's string.
TEST_F(ConnectPathTest, DriverConnect_TruncationKeepsTheConnectWarning) {
    constexpr SQLSMALLINT BUFFER_CHARS = 40;
    const std::string conn_str = ConnStr("warning.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);
    ASSERT_GT(full_length, BUFFER_CHARS);
    plugin_->warning = "Option value changed";
    plugin_->base_out = std::string("UID=admin;PWD=") + TOKEN + ";";

    std::vector<SQLTCHAR> out(BUFFER_CHARS, 0);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, DriverConnect(dbc_, conn_str, out.data(), BUFFER_CHARS, nullptr));
    EXPECT_EQ(full.substr(0, BUFFER_CHARS - 1), AppText(out.data()));
    EXPECT_FALSE(BufferHolds(out.data(), out.size() * sizeof(SQLTCHAR), TOKEN));
    ASSERT_TRUE(dbc_.err);
    EXPECT_STREQ("01S02", dbc_.err->sqlstate);
}

// Without a length pointer the app still gets the wrapper's string, and nothing of the base driver's, whose PWD holds
// the token.
TEST_F(ConnectPathTest, DriverConnect_NoLengthPointerStillGetsTheWrapperString) {
    const std::string conn_str = ConnStr("no-length.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);

    std::vector<SQLTCHAR> out(MAX_MSG_LEN, 0);
    EXPECT_EQ(SQL_SUCCESS, DriverConnect(dbc_, conn_str, out.data(), static_cast<SQLSMALLINT>(out.size()), nullptr));
    EXPECT_EQ(full, AppText(out.data()));
    EXPECT_FALSE(BufferHolds(out.data(), out.size() * sizeof(SQLTCHAR), TOKEN));
    EXPECT_FALSE(BufferHolds(out.data(), out.size() * sizeof(SQLTCHAR), "PWD="));
}

// A wrapper string shorter than the base driver's leaves nothing of the base driver's string past its terminator.
TEST_F(ConnectPathTest, DriverConnect_ShorterStringLeavesNothingOfTheBaseDriverString) {
    const std::string conn_str = ConnStr("shorter.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);
    ASSERT_LT(static_cast<size_t>(full_length), plugin_->base_out.find(TOKEN));

    std::vector<SQLTCHAR> out(MAX_MSG_LEN, 0);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, DriverConnect(dbc_, conn_str, out.data(), static_cast<SQLSMALLINT>(out.size()), &length));
    EXPECT_EQ(full_length, length);
    EXPECT_EQ(full, AppText(out.data()));
    EXPECT_FALSE(BufferHolds(out.data(), out.size() * sizeof(SQLTCHAR), TOKEN));
    EXPECT_FALSE(BufferHolds(out.data(), out.size() * sizeof(SQLTCHAR), "PWD="));
}

// psqlODBC up to 17.00.0004 cuts its own string to a short buffer, keeping PWD, and returns SQL_SUCCESS_WITH_INFO with
// its 01004. The wrapper's string doesn't fit either: the buffer holds the wrapper's first characters, and the base
// driver's warning stays the connect's warning.
TEST_F(ConnectPathTest, DriverConnect_TruncatedStringReplacesTheBaseDriverString) {
    constexpr SQLSMALLINT BUFFER_CHARS = 40;
    const std::string conn_str = ConnStr("replaced.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);
    plugin_->base_out = std::string("UID=admin;PWD=") + TOKEN + ";";
    plugin_->result = SQL_SUCCESS_WITH_INFO;
    ASSERT_LT(plugin_->base_out.size(), static_cast<size_t>(BUFFER_CHARS));
    ASSERT_GT(full_length, BUFFER_CHARS);

    std::vector<SQLTCHAR> out(BUFFER_CHARS, 0);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, DriverConnect(dbc_, conn_str, out.data(), BUFFER_CHARS, &length));
    EXPECT_EQ(full_length, length);
    EXPECT_EQ(full.substr(0, BUFFER_CHARS - 1), AppText(out.data()));
    EXPECT_FALSE(BufferHolds(out.data(), out.size() * sizeof(SQLTCHAR), TOKEN));
    EXPECT_FALSE(dbc_.err);
}

#ifdef UNICODE
// A cut at the buffer's end doesn't keep the first half of a surrogate pair. U+1F600 is two SQLWCHARs.
TEST_F(ConnectPathTest, DriverConnect_TruncationKeepsSurrogatePairsWhole) {
    const std::string conn_str = ConnStr("pair-\xF0\x9F\x98\x80.example.com");
    std::vector<SQLTCHAR> full(MAX_MSG_LEN, 0);
    SQLSMALLINT full_length = 0;
    DBC reference{};
    Wire(reference);
    ASSERT_EQ(SQL_SUCCESS,
        DriverConnect(reference, conn_str, full.data(), static_cast<SQLSMALLINT>(full.size()), &full_length));
    const auto lead = static_cast<size_t>(
        std::find_if(full.begin(), full.end(), [](SQLTCHAR c) { return U16_IS_LEAD(c); }) - full.begin());
    ASSERT_LT(lead + 1, static_cast<size_t>(full_length));

    // Room for the characters before the pair, its first half and the terminator.
    std::vector<SQLTCHAR> out(lead + 2, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
        DriverConnect(dbc_, conn_str, out.data(), static_cast<SQLSMALLINT>(out.size()), &length));
    EXPECT_EQ(full_length, length);
    EXPECT_EQ(std::vector<SQLTCHAR>(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(lead)),
        std::vector<SQLTCHAR>(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(lead)));
    EXPECT_EQ(0, out[lead]);
}

// An app with a 4-byte SQLWCHAR gets 4-byte characters in its BufferLength characters: the whole string when it fits,
// else the first characters, a terminator and 01004, with nothing past them. Nothing of the base driver's 2-byte string
// stays in either case.
TEST_F(ConnectPathTest, DriverConnect_FourByteAppGetsFourByteCharacters) {
    constexpr uint32_t GUARD = 0x5A5A5A5A;
    constexpr SQLSMALLINT ROOMY = 400;
    constexpr SQLSMALLINT TIGHT = 21;
    const std::string conn_str = ConnStr("four-byte.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);
    // A base driver string that fits ROOMY, with the token past the end of the wrapper's 4-byte string.
    plugin_->base_out = std::string("DRIVER={PostgreSQL Unicode};SERVER=") + std::string(250, 'b') + ";UID=admin;PWD="
        + TOKEN;
    ASSERT_LT(full_length, ROOMY);
    ASSERT_LT(plugin_->base_out.size(), static_cast<size_t>(ROOMY));
    ASSERT_GT(plugin_->base_out.find(TOKEN) * sizeof(SQLTCHAR),
        (static_cast<size_t>(full_length) + 1) * sizeof(uint32_t));

    env_.use_4_bytes_user_app = true;
    std::vector<SQLTCHAR> in = AppString4(conn_str);
    std::vector<uint32_t> out(ROOMY + 64, GUARD);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLDriverConnect(&dbc_, nullptr, in.data(), SQL_NTS,
        reinterpret_cast<SQLTCHAR*>(out.data()), ROOMY, &length, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(full_length, length);
    EXPECT_EQ(full, AppText4(out.data()));
    EXPECT_FALSE(BufferHolds(out.data(), ROOMY * sizeof(uint32_t), TOKEN));
    EXPECT_EQ(out.size() - ROOMY, static_cast<size_t>(std::count(out.begin() + ROOMY, out.end(), GUARD)));

    // Room for 20 characters and the terminator, no length pointer.
    plugin_->base_out = std::string("PWD=") + TOKEN;
    DBC second{};
    Wire(second);
    std::vector<uint32_t> short_out(TIGHT + 64, GUARD);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLDriverConnect(&second, nullptr, in.data(), SQL_NTS,
        reinterpret_cast<SQLTCHAR*>(short_out.data()), TIGHT, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(full.substr(0, TIGHT - 1), AppText4(short_out.data()));
    EXPECT_FALSE(BufferHolds(short_out.data(), TIGHT * sizeof(uint32_t), TOKEN));
    EXPECT_EQ(short_out.size() - TIGHT,
        static_cast<size_t>(std::count(short_out.begin() + TIGHT, short_out.end(), GUARD)));
    ASSERT_TRUE(second.err);
    EXPECT_STREQ("01004", second.err->sqlstate);
}
#endif

// SQLGetInfo's BufferLength counts bytes. A W caller with room for 20 SQLWCHARs gets at most 19 and the terminator,
// and a longer string is reported as truncated.
TEST_F(ErrorHandlingTest, GetInfo_StringBufferLengthCountsBytes) {
    const std::string name(DRIVER_NAME);
    constexpr SQLTCHAR GUARD = 0x5A;
    struct {
        SQLTCHAR value[20];
        SQLTCHAR guard[48];
    } out{};
    for (SQLTCHAR& c : out.guard) {
        c = GUARD;
    }
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, out.value,
        static_cast<SQLSMALLINT>(sizeof(out.value)), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(name.size() * sizeof(SQLTCHAR)), length);
    for (const SQLTCHAR c : out.guard) {
        EXPECT_EQ(GUARD, c);
    }
}

TEST_F(ErrorHandlingTest, GetInfo_StringThatFitsIsComplete) {
    const std::string name(DRIVER_NAME);
    std::vector<SQLTCHAR> value(name.size() + 1, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, value.data(),
        static_cast<SQLSMALLINT>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(name, AppText(value.data()));
    EXPECT_EQ(static_cast<SQLSMALLINT>(name.size() * sizeof(SQLTCHAR)), length);
}

#ifdef UNICODE
// An app with a 4-byte SQLWCHAR gets 4-byte characters, before any connect too.
TEST_F(ErrorHandlingTest, GetInfo_FourByteAppGetsFourByteCharacters) {
    const std::string name(DRIVER_NAME);
    env_->use_4_bytes_user_app = true;
    std::vector<uint32_t> value(name.size() + 1, 0x5A5A5A5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, value.data(),
        static_cast<SQLSMALLINT>(value.size() * sizeof(uint32_t)), &length));
    std::string text;
    for (size_t i = 0; i < name.size(); ++i) {
        text.push_back(static_cast<char>(value[i]));
    }
    EXPECT_EQ(name, text);
    EXPECT_EQ(0U, value[name.size()]);
    EXPECT_EQ(static_cast<SQLSMALLINT>(name.size() * sizeof(uint32_t)), length);
    env_->use_4_bytes_user_app = false;
}
#endif
