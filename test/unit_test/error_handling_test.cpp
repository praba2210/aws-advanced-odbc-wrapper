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

#include <cstdint>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include "../../driver/driver.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/error.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/odbc_dsn_helper.h"
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
