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
#include <cstring>
#include <functional>
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
    EXPECT_EQ("original", stmt_->err->error_msg);
    EXPECT_EQ("HY000", stmt_->err->sqlstate);
}

TEST_F(ErrorHandlingTest, UniquePtr_ReassignmentDeletesPrevious) {
    stmt_->err = std::make_unique<ErrInfo>("first", ERR_GENERAL_ERROR);
    stmt_->err = std::make_unique<ErrInfo>("second", ERR_MEMORY_ALLOCATION_ERROR);
    EXPECT_EQ("second", stmt_->err->error_msg);
    EXPECT_EQ("HY001", stmt_->err->sqlstate);
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

// SQLGetDiagField copies the handle's ErrInfo before reading it, so every field
// has to survive a copy. is_odbc3_subclass used to be dropped by a hand-written
// copy constructor, which made SQL_DIAG_SUBCLASS_ORIGIN always report ISO 9075.

TEST_F(ErrorHandlingTest, ErrInfo_CopyPreservesOdbc3Subclass) {
    const ErrInfo source("Connection failed", ERR_COMMUNICATION_LINK_FAILURE);
    ASSERT_TRUE(source.is_odbc3_subclass);

    const ErrInfo copy(source);

    EXPECT_EQ(source.error_msg, copy.error_msg);
    EXPECT_EQ(source.sqlstate, copy.sqlstate);
    EXPECT_EQ(source.native_err, copy.native_err);
    EXPECT_EQ(source.ret_code, copy.ret_code);
    EXPECT_EQ(source.is_odbc3_subclass, copy.is_odbc3_subclass);
}

TEST_F(ErrorHandlingTest, DiagField_SubclassOriginReportsOdbc3ForSubclassState) {
    // 08S01 is an ODBC 3 subclass.
    dbc_->err = std::make_unique<ErrInfo>("Connection failed", ERR_COMMUNICATION_LINK_FAILURE);

    SQLTCHAR origin[MAX_SQL_STATE_LEN * 4] = {0};
    SQLSMALLINT length = 0;
    const SQLRETURN ret = RDS_SQLGetDiagField(
        SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_SUBCLASS_ORIGIN,
        origin, sizeof(origin), &length);

    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_EQ("ODBC 3.0", AppText(origin));
}

TEST_F(ErrorHandlingTest, DiagField_SubclassOriginReportsIsoForNonSubclassState) {
    // HY000 is not an ODBC 3 subclass.
    dbc_->err = std::make_unique<ErrInfo>("General error", ERR_GENERAL_ERROR);

    SQLTCHAR origin[MAX_SQL_STATE_LEN * 4] = {0};
    SQLSMALLINT length = 0;
    const SQLRETURN ret = RDS_SQLGetDiagField(
        SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_SUBCLASS_ORIGIN,
        origin, sizeof(origin), &length);

    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_EQ("ISO 9075", AppText(origin));
}

TEST_F(ErrorHandlingTest, ErrInfo_NullCStringYieldsEmptyMessage) {
    const char* null_msg = nullptr;
    const ErrInfo err(null_msg, ERR_COMMUNICATION_LINK_FAILURE);

    EXPECT_TRUE(err.error_msg.empty());
    EXPECT_EQ("08S01", err.sqlstate);
    EXPECT_EQ(SQL_ERROR, err.ret_code);
    EXPECT_TRUE(err.is_odbc3_subclass);
}

TEST_F(ErrorHandlingTest, DiagRec_EmptyMessageStillReportsSqlState) {
    dbc_->err = std::make_unique<ErrInfo>("", ERR_COMMUNICATION_LINK_FAILURE);

    SQLTCHAR sql_state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT text_length = 0;

    const SQLRETURN ret = RDS_SQLGetDiagRec(
        SQL_HANDLE_DBC, dbc_, 1,
        sql_state, &native_error, message, MAX_MSG_LEN, &text_length, false);

    EXPECT_EQ(SQL_SUCCESS, ret);
#ifndef UNICODE
    EXPECT_STREQ("08S01", reinterpret_cast<const char*>(sql_state));
#endif
    EXPECT_EQ(static_cast<SQLINTEGER>(ERR_COMMUNICATION_LINK_FAILURE), native_error);
}

TEST_F(ErrorHandlingTest, DiagField_EmptyMessageTextWritesEmptyString) {
    dbc_->err = std::make_unique<ErrInfo>("", ERR_COMMUNICATION_LINK_FAILURE);

    char message[MAX_MSG_LEN];
    std::fill_n(message, sizeof(message), 'X');
    SQLSMALLINT length = -1;

    const SQLRETURN ret = RDS_SQLGetDiagField(
        SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_MESSAGE_TEXT,
        message, sizeof(message), &length);

    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("", message);
    EXPECT_EQ(0, length);
}

TEST_F(ErrorHandlingTest, DiagField_ConnectionNameWithoutDsnWritesEmptyString) {
    dbc_->err = std::make_unique<ErrInfo>("Connection failed", ERR_COMMUNICATION_LINK_FAILURE);
    ASSERT_FALSE(dbc_->conn_attr.contains(KEY_DSN));

    char conn_name[MAX_MSG_LEN];
    std::fill_n(conn_name, sizeof(conn_name), 'X');
    SQLSMALLINT length = -1;

    const SQLRETURN ret = RDS_SQLGetDiagField(
        SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_CONNECTION_NAME,
        conn_name, sizeof(conn_name), &length);

    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("", conn_name);
    EXPECT_EQ(0, length);
}

TEST_F(ErrorHandlingTest, DiagField_MessageTextIsReturnedInFull) {
    dbc_->err = std::make_unique<ErrInfo>("Connection failed", ERR_COMMUNICATION_LINK_FAILURE);

    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT length = 0;

    const SQLRETURN ret = RDS_SQLGetDiagField(
        SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_MESSAGE_TEXT,
        message, sizeof(message), &length);

    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_EQ("Connection failed", AppText(message));
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
    EXPECT_EQ("IM003", dbc_->err->sqlstate);
    const std::string& message = dbc_->err->error_msg;
    EXPECT_NE(std::string::npos, message.find("/nonexistent/path/no-such-driver.so"));
    EXPECT_FALSE(env_->driver_lib_loader);
}

TEST_F(ErrorHandlingTest, InitializeConnection_UnregisteredDriverNameReturnsErrorDiagnostic) {
    dbc_->conn_attr[KEY_BASE_DRIVER] = "DefinitelyNotARegisteredOdbcDriver123";

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(dbc_, ""));

    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ("IM003", dbc_->err->sqlstate);
    const std::string& message = dbc_->err->error_msg;
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
    OdbcHelper::SetWrappedStmt(stmt_, &fake_handle);  // never called through
    stmt_->err = std::make_unique<ErrInfo>("Optional feature not implemented", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
    const std::string expected(stmt_->err->sqlstate);

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SQLSTATE, state,
        static_cast<SQLSMALLINT>(sizeof(state)), nullptr));
    EXPECT_EQ(expected, AppText(state));

    SQLINTEGER native = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_NATIVE, &native, 0, nullptr));
    EXPECT_EQ(static_cast<SQLINTEGER>(ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED), native);

    OdbcHelper::SetWrappedStmt(stmt_, SQL_NULL_HSTMT);
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
    EXPECT_EQ(expected.substr(0, 3), AppText(short_state));

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

// A string field that doesn't fit is cut to a terminated prefix, also one that would fit but for its terminator:
// SQL_SUCCESS_WITH_INFO and the full length in both cases.
TEST_F(ErrorHandlingTest, DiagField_TruncatedFieldIsATerminatedPrefix) {
    stmt_->err = std::make_unique<ErrInfo>("SQLExecDirect - Connection not open", ERR_CONNECTION_NOT_OPEN);
    const std::string message(stmt_->err->error_msg);
    std::vector<SQLTCHAR> value(8, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_MESSAGE_TEXT, value.data(),
        static_cast<SQLSMALLINT>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(message.size() * sizeof(SQLTCHAR)), length);
    ASSERT_EQ(0, value.back());  // the terminator, before reading the buffer as a string
    EXPECT_EQ("SQLExec", AppText(value.data()));

    // The SQLSTATE's 5 characters in 5: room for 4 and the terminator.
    const std::string state(stmt_->err->sqlstate);
    std::vector<SQLTCHAR> exact(state.size(), 0x5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SQLSTATE, exact.data(),
        static_cast<SQLSMALLINT>(exact.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(state.size() * sizeof(SQLTCHAR)), length);
    ASSERT_EQ(0, exact.back());
    EXPECT_EQ(state.substr(0, state.size() - 1), AppText(exact.data()));
}

#ifdef UNICODE
// The length counts the app's characters, not UTF-8 bytes: "café" is 5 UTF-8 bytes and 4 SQLWCHARs, so it fits 5
// SQLWCHARs with its terminator.
TEST_F(ErrorHandlingTest, DiagField_NonAsciiMessageFitsWithoutWarning) {
    stmt_->err = std::make_unique<ErrInfo>("caf\xC3\xA9", ERR_GENERAL_ERROR);
    std::vector<SQLTCHAR> message(5, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_MESSAGE_TEXT, message.data(),
        static_cast<SQLSMALLINT>(message.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(8, length);
    EXPECT_EQ("caf\xC3\xA9", AppText(message.data()));
}

// The same for a DSN with a non-ASCII name: "Données" is 8 UTF-8 bytes and 7 SQLWCHARs.
TEST_F(ErrorHandlingTest, DiagField_NonAsciiConnectionNameFits) {
    dbc_->err = std::make_unique<ErrInfo>("Option value changed", WARN_OPTION_VALUE_CHANGED);
    const std::string dsn = "Donn\xC3\xA9" "es";
    dbc_->conn_attr[KEY_DSN] = dsn;
    std::vector<SQLTCHAR> name(8, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_CONNECTION_NAME, name.data(),
        static_cast<SQLSMALLINT>(name.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(14, length);
    EXPECT_EQ(dsn, AppText(name.data()));
}

// An app with a 4-byte SQLWCHAR counts code points, 4 bytes each: "café" is 16 bytes and fits 5 characters, and "a😀"
// is 8 bytes and fits 3, the emoji as one code point.
TEST_F(ErrorHandlingTest, DiagField_FourByteAppCountsCodePoints) {
    env_->use_4_bytes_user_app = true;
    stmt_->err = std::make_unique<ErrInfo>("caf\xC3\xA9", ERR_GENERAL_ERROR);
    std::vector<uint32_t> cafe(5, 0x5A5A5A5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_MESSAGE_TEXT, cafe.data(),
        static_cast<SQLSMALLINT>(cafe.size() * sizeof(uint32_t)), &length));
    EXPECT_EQ(16, length);
    EXPECT_EQ((std::vector<uint32_t>{'c', 'a', 'f', 0xE9, 0}), cafe);

    stmt_->err = std::make_unique<ErrInfo>("a\xF0\x9F\x98\x80", ERR_GENERAL_ERROR);
    std::vector<uint32_t> emoji(3, 0x5A5A5A5A);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_MESSAGE_TEXT, emoji.data(),
        static_cast<SQLSMALLINT>(emoji.size() * sizeof(uint32_t)), &length));
    EXPECT_EQ(8, length);
    EXPECT_EQ((std::vector<uint32_t>{'a', 0x1F600, 0}), emoji);
    env_->use_4_bytes_user_app = false;
}

// The room is counted in whole characters of the app: a BufferLength that isn't a multiple of the character size
// leaves its last bytes unused. The SQLSTATE's 5 characters with 11 bytes (5.5 SQLWCHARs), or 23 for a 4-byte app,
// don't fit with the terminator: a terminated prefix and SQL_SUCCESS_WITH_INFO, not a cut value with SQL_SUCCESS.
TEST_F(ErrorHandlingTest, DiagField_BufferLengthCountsWholeCharacters) {
    stmt_->err = std::make_unique<ErrInfo>("x", ERR_CONNECTION_NOT_OPEN);
    const std::string state(stmt_->err->sqlstate);
    ASSERT_EQ(5U, state.size());

    std::vector<SQLTCHAR> value(6, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SQLSTATE, value.data(),
        static_cast<SQLSMALLINT>(state.size() * sizeof(SQLTCHAR) + 1), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(state.size() * sizeof(SQLTCHAR)), length);
    EXPECT_EQ(state.substr(0, 4), AppText(value.data()));

    env_->use_4_bytes_user_app = true;
    std::vector<uint32_t> prefix(state.begin(), state.begin() + 4);
    prefix.push_back(0);
    std::vector<uint32_t> wide(6, 0x5A5A5A5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SQLSTATE, wide.data(),
        static_cast<SQLSMALLINT>(state.size() * sizeof(uint32_t) + 3), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(state.size() * sizeof(uint32_t)), length);
    EXPECT_EQ(prefix, std::vector<uint32_t>(wide.begin(), wide.begin() + 5));
    // One whole character more: it fits.
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_STMT, stmt_, 1, SQL_DIAG_SQLSTATE, wide.data(),
        static_cast<SQLSMALLINT>(wide.size() * sizeof(uint32_t)), &length));
    std::vector<uint32_t> whole(state.begin(), state.end());
    whole.push_back(0);
    EXPECT_EQ(whole, wide);
    env_->use_4_bytes_user_app = false;
}
#endif

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

    // Stands in for the plugin chain and the base driver. It records the attributes each connect gets and leaves keys
    // behind as token plugins and connection defaults do. The base driver's output string goes into DefaultPlugin's
    // buffer (default_plugin_test.cpp), so the app's buffer gets what DefaultPlugin leaves there: an empty string, in
    // the app's characters.
    class ConnectRecorder : public BasePlugin {
    public:
        SQLRETURN Connect(SQLHDBC connection_handle, SQLHWND /*window_handle*/, SQLTCHAR* out_connection_string,
                          SQLSMALLINT buffer_length, SQLSMALLINT* /*string_length_ptr*/,
                          SQLUSMALLINT /*driver_completion*/) override {
            DBC* dbc = static_cast<DBC*>(connection_handle);
            seen.push_back(dbc->conn_attr);
            for (const auto& [key, value] : added) {
                dbc->conn_attr.insert_or_assign(key, value);
            }
            if (out_connection_string != nullptr && buffer_length > 0) {
                std::memset(out_connection_string, 0, dbc->env->use_4_bytes_user_app.load() ? 4 : sizeof(SQLTCHAR));
            }
            if (warning != nullptr) {
                dbc->err = std::make_unique<ErrInfo>(warning, WARN_OPTION_VALUE_CHANGED);
                return SQL_SUCCESS_WITH_INFO;
            }
            return result;
        }

        std::vector<std::map<std::string, std::string>> seen;
        std::map<std::string, std::string> added;
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
        void SetCurrentHostInfo(const HostInfo& /*info*/) override {
            if (on_set_current_host_info) {
                on_set_current_host_info();
            }
        }

        // Runs where RDS_SQLDriverConnect records the connected host, after the plugin chain's connect returned.
        std::function<void()> on_set_current_host_info;

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

// The app gets the wrapper's output string. When it doesn't fit, SQL_SUCCESS_WITH_INFO comes
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
    EXPECT_EQ("01004", dbc_.err->sqlstate);
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
// buffer still gets the wrapper's first characters.
TEST_F(ConnectPathTest, DriverConnect_TruncationKeepsTheConnectWarning) {
    constexpr SQLSMALLINT BUFFER_CHARS = 40;
    const std::string conn_str = ConnStr("warning.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);
    ASSERT_GT(full_length, BUFFER_CHARS);
    plugin_->warning = "Option value changed";

    std::vector<SQLTCHAR> out(BUFFER_CHARS, 0);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, DriverConnect(dbc_, conn_str, out.data(), BUFFER_CHARS, nullptr));
    EXPECT_EQ(full.substr(0, BUFFER_CHARS - 1), AppText(out.data()));
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);
}

// Without a length pointer the app still gets the wrapper's string.
TEST_F(ConnectPathTest, DriverConnect_NoLengthPointerStillGetsTheWrapperString) {
    const std::string conn_str = ConnStr("no-length.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);

    std::vector<SQLTCHAR> out(MAX_MSG_LEN, 0);
    EXPECT_EQ(SQL_SUCCESS, DriverConnect(dbc_, conn_str, out.data(), static_cast<SQLSMALLINT>(out.size()), nullptr));
    EXPECT_EQ(full, AppText(out.data()));
}

// A connect that returned SQL_SUCCESS_WITH_INFO without a record of the wrapper's keeps that code when the wrapper's
// string doesn't fit either, and gets no 01004 over it: the buffer holds the wrapper's first characters.
TEST_F(ConnectPathTest, DriverConnect_TruncationKeepsTheConnectsWarningCode) {
    constexpr SQLSMALLINT BUFFER_CHARS = 40;
    const std::string conn_str = ConnStr("replaced.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);
    plugin_->result = SQL_SUCCESS_WITH_INFO;
    ASSERT_GT(full_length, BUFFER_CHARS);

    std::vector<SQLTCHAR> out(BUFFER_CHARS, 0);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, DriverConnect(dbc_, conn_str, out.data(), BUFFER_CHARS, &length));
    EXPECT_EQ(full_length, length);
    EXPECT_EQ(full.substr(0, BUFFER_CHARS - 1), AppText(out.data()));
    EXPECT_FALSE(dbc_.err);
}

// A negative BufferLength is an error with an HY090 record, before anything connects: nothing is written, and the
// handle isn't left connected under an error. unixODBC checks only StringLength1 here and passes it on.
TEST_F(ConnectPathTest, DriverConnect_NegativeBufferLengthIsAnError) {
    std::vector<SQLTCHAR> out(16, 0x5A);
    SQLSMALLINT length = 77;
    EXPECT_EQ(SQL_ERROR, DriverConnect(dbc_, ConnStr("negative.example.com"), out.data(), -1, &length));
    EXPECT_TRUE(plugin_->seen.empty());
    EXPECT_EQ(std::vector<SQLTCHAR>(16, 0x5A), out);
    EXPECT_EQ(77, length);
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ("HY090", dbc_.err->sqlstate);
}

// The wrapper writes its string and the terminator into the app's buffer, and nothing else: the bytes past them keep
// what the app had there.
TEST_F(ConnectPathTest, DriverConnect_WritesOnlyItsStringIntoTheBuffer) {
    constexpr SQLTCHAR GUARD = 0x5A;
    const std::string conn_str = ConnStr("only-its-string.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);

    std::vector<SQLTCHAR> out(MAX_MSG_LEN, GUARD);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, DriverConnect(dbc_, conn_str, out.data(), static_cast<SQLSMALLINT>(out.size()), &length));
    EXPECT_EQ(full_length, length);
    EXPECT_EQ(full, AppText(out.data()));
    const auto past = out.begin() + full_length + 1;
    EXPECT_EQ(static_cast<size_t>(out.end() - past), static_cast<size_t>(std::count(past, out.end(), GUARD)));
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
// else the first characters, a terminator and 01004, with nothing past them.
TEST_F(ConnectPathTest, DriverConnect_FourByteAppGetsFourByteCharacters) {
    constexpr uint32_t GUARD = 0x5A5A5A5A;
    constexpr SQLSMALLINT ROOMY = 400;
    constexpr SQLSMALLINT TIGHT = 21;
    const std::string conn_str = ConnStr("four-byte.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);
    ASSERT_LT(full_length, ROOMY);

    env_.use_4_bytes_user_app = true;
    std::vector<SQLTCHAR> in = AppString4(conn_str);
    std::vector<uint32_t> out(ROOMY + 64, GUARD);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLDriverConnect(&dbc_, nullptr, in.data(), SQL_NTS,
        reinterpret_cast<SQLTCHAR*>(out.data()), ROOMY, &length, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(full_length, length);
    EXPECT_EQ(full, AppText4(out.data()));
    EXPECT_EQ(out.size() - ROOMY, static_cast<size_t>(std::count(out.begin() + ROOMY, out.end(), GUARD)));

    // Room for 20 characters and the terminator, no length pointer.
    DBC second{};
    Wire(second);
    std::vector<uint32_t> short_out(TIGHT + 64, GUARD);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLDriverConnect(&second, nullptr, in.data(), SQL_NTS,
        reinterpret_cast<SQLTCHAR*>(short_out.data()), TIGHT, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(full.substr(0, TIGHT - 1), AppText4(short_out.data()));
    EXPECT_EQ(short_out.size() - TIGHT,
        static_cast<size_t>(std::count(short_out.begin() + TIGHT, short_out.end(), GUARD)));
    ASSERT_TRUE(second.err);
    EXPECT_EQ("01004", second.err->sqlstate);
}

// An app with a 4-byte SQLWCHAR gets the length in its characters, code points, where U+1F600 is one character and
// not a surrogate pair: the whole string fits a buffer with room for it and the terminator, without a 01004.
TEST_F(ConnectPathTest, DriverConnect_FourByteAppCountsCodePoints) {
    const auto code_points = [](const std::string& utf8) {
        const icu::UnicodeString text = icu::UnicodeString::fromUTF8(utf8);
        std::vector<uint32_t> out;
        for (int32_t i = 0; i < text.length(); i = text.moveIndex32(i, 1)) {
            out.push_back(static_cast<uint32_t>(text.char32At(i)));
        }
        return out;
    };
    const std::string conn_str = ConnStr("pair-\xF0\x9F\x98\x80.example.com");
    SQLSMALLINT utf16_length = 0;
    const std::vector<uint32_t> full = code_points(FullOutput(conn_str, &utf16_length));
    ASSERT_EQ(static_cast<size_t>(utf16_length), full.size() + 1);  // one surrogate pair

    env_.use_4_bytes_user_app = true;
    std::vector<uint32_t> in = code_points(conn_str);
    in.push_back(0);
    std::vector<uint32_t> out(full.size() + 1, 0x5A5A5A5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLDriverConnect(&dbc_, nullptr, reinterpret_cast<SQLTCHAR*>(in.data()), SQL_NTS,
        reinterpret_cast<SQLTCHAR*>(out.data()), static_cast<SQLSMALLINT>(out.size()), &length, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(static_cast<SQLSMALLINT>(full.size()), length);
    EXPECT_EQ(full, std::vector<uint32_t>(out.begin(), out.end() - 1));
    EXPECT_EQ(0U, out.back());
    EXPECT_FALSE(dbc_.err);
}

// When the app passes no output buffer, iODBC 3.52.16 passes its own, SQLWCHAR connStrOut_buf[2048] (8 KB for a 4-byte
// wchar_t), and its size in bytes, 8192, as the length in characters: four times the buffer. The wrapper writes only its
// string and the terminator, well inside the real buffer, and nothing past them. Zero-filling the 8192 characters it was
// told, 32 KB, overran iODBC's stack buffer by 24 KB and crashed the app. Here the whole claimed length is allocated, so
// that a write past the real buffer shows in the guard pattern instead of crashing the test.
TEST_F(ConnectPathTest, DriverConnect_IodbcClaimedLengthIsNotFilled) {
    constexpr SQLSMALLINT CLAIMED = 8192;  // characters, as iODBC says
    constexpr size_t REAL = 2048;          // iODBC's buffer, in 4-byte characters
    constexpr uint32_t GUARD = 0x5A5A5A5A;
    const std::string conn_str = ConnStr("iodbc.example.com");
    SQLSMALLINT full_length = 0;
    const std::string full = FullOutput(conn_str, &full_length);
    ASSERT_LT(static_cast<size_t>(full_length) + 1, REAL);

    env_.use_4_bytes_user_app = true;
    std::vector<SQLTCHAR> in = AppString4(conn_str);
    std::vector<uint32_t> out(CLAIMED, GUARD);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLDriverConnect(&dbc_, nullptr, in.data(), SQL_NTS,
        reinterpret_cast<SQLTCHAR*>(out.data()), CLAIMED, &length, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(full_length, length);
    EXPECT_EQ(full, AppText4(out.data()));
    const auto past = out.begin() + full_length + 1;
    EXPECT_EQ(static_cast<size_t>(REAL - (full_length + 1)), static_cast<size_t>(std::count(past, out.begin() + REAL, GUARD)));
    EXPECT_EQ(out.size() - REAL, static_cast<size_t>(std::count(out.begin() + REAL, out.end(), GUARD)));
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
    ASSERT_EQ(0, out.value[19]);
    EXPECT_EQ(name.substr(0, 19), AppText(out.value));
    for (const SQLTCHAR c : out.guard) {
        EXPECT_EQ(GUARD, c);
    }
}

// A value that doesn't fit is cut to a terminated prefix, also one that would fit but for its terminator.
TEST_F(ErrorHandlingTest, GetInfo_TruncatedValueIsATerminatedPrefix) {
    const std::string name(DRIVER_NAME);
    std::vector<SQLTCHAR> value(8, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, value.data(),
        static_cast<SQLSMALLINT>(value.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(name.size() * sizeof(SQLTCHAR)), length);
    ASSERT_EQ(0, value.back());
    EXPECT_EQ(name.substr(0, value.size() - 1), AppText(value.data()));

    std::vector<SQLTCHAR> exact(name.size(), 0x5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, exact.data(),
        static_cast<SQLSMALLINT>(exact.size() * sizeof(SQLTCHAR)), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(name.size() * sizeof(SQLTCHAR)), length);
    ASSERT_EQ(0, exact.back());
    EXPECT_EQ(name.substr(0, name.size() - 1), AppText(exact.data()));
}

// A negative BufferLength is an error with an HY090 record, for every info type: the wrapper's own strings and numbers,
// and on a connected handle the base driver's values (psqlODBC doesn't check it either). Nothing is written. A driver
// manager (unixODBC 2.3.12, iODBC) returns HY090 before it calls the driver; this is for callers without one.
TEST_F(ErrorHandlingTest, GetInfo_NegativeBufferLengthIsAnError) {
    constexpr unsigned char GUARD = 0x5A;
    struct {
        SQLTCHAR value[4];
        unsigned char guard[96];
    } out{};
    auto* const bytes = reinterpret_cast<unsigned char*>(&out);
    std::fill_n(bytes, sizeof(out), GUARD);
    SQLSMALLINT length = 77;
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, out.value, -1, &length));
    EXPECT_EQ(sizeof(out), static_cast<size_t>(std::count(bytes, bytes + sizeof(out), GUARD)));
    EXPECT_EQ(77, length);
    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native = 0;
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT message_length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, dbc_, 1, state, &native, message, MAX_MSG_LEN,
        &message_length, false));
    EXPECT_EQ("HY090", AppText(state));

    SQLUSMALLINT number = 77;
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetInfo(dbc_, SQL_MAX_CONCURRENT_ACTIVITIES, &number, -1, nullptr));
    EXPECT_EQ(77, number);

    // Without the check, the base driver would be asked here, and no function is loaded: a different record.
    int fake_dbc = 0;
    dbc_->wrapped_dbc = &fake_dbc;  // never called through
    dbc_->plugin_service = std::make_shared<ConnectOnlyPluginService>(env_);
    dbc_->err.reset();
    EXPECT_EQ(SQL_ERROR, RDS_SQLGetInfo(dbc_, SQL_DBMS_NAME, out.value, -2, &length));
    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ("HY090", dbc_->err->sqlstate);
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
}

namespace {
    constexpr const char* TRUNCATION_BASE_MESSAGE = "base driver: string data, right truncated";

    // Text in the app's characters (the base driver's too here), cut to room characters with a terminator.
    SQLSMALLINT TruncationWriteText(std::string_view text, SQLTCHAR* out, size_t room) {
        if (out != nullptr && room > 0) {
            const size_t copied = std::min(text.size(), room - 1);
            for (size_t i = 0; i < copied; ++i) {
                out[i] = static_cast<SQLTCHAR>(text[i]);
            }
            out[copied] = 0;
        }
        return static_cast<SQLSMALLINT>(text.size());
    }

    size_t TruncationRoom(SQLSMALLINT buffer_bytes) {
        return buffer_bytes > 0 ? static_cast<size_t>(buffer_bytes) / sizeof(SQLTCHAR) : 0;
    }

    // A base driver that cuts its SQLGetInfo string and posts its own 01004, as psqlODBC does.
    SQLRETURN SQL_API TruncationFakeGetInfo(SQLHDBC /*dbc*/, SQLUSMALLINT /*info_type*/, SQLPOINTER value,
                                            SQLSMALLINT buffer_length, SQLSMALLINT* length) {
        const auto bytes = static_cast<SQLSMALLINT>(
            TruncationWriteText("PostgreSQL", static_cast<SQLTCHAR*>(value), TruncationRoom(buffer_length))
            * sizeof(SQLTCHAR));
        if (length != nullptr) {
            *length = bytes;
        }
        return bytes >= buffer_length ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    }

    SQLRETURN SQL_API TruncationFakeGetDiagRec(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT record,
                                               SQLTCHAR* state, SQLINTEGER* native, SQLTCHAR* message,
                                               SQLSMALLINT buffer_length, SQLSMALLINT* length) {
        if (record != 1) {
            return SQL_NO_DATA;
        }
        TruncationWriteText("01004", state, MAX_SQL_STATE_LEN);
        if (native != nullptr) {
            *native = 7;
        }
        const SQLSMALLINT chars = TruncationWriteText(TRUNCATION_BASE_MESSAGE, message,
            buffer_length > 0 ? static_cast<size_t>(buffer_length) : 0);
        if (length != nullptr) {
            *length = chars;
        }
        return SQL_SUCCESS;
    }

    SQLRETURN SQL_API TruncationFakeGetDiagField(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT record,
                                                 SQLSMALLINT identifier, SQLPOINTER info, SQLSMALLINT buffer_length,
                                                 SQLSMALLINT* length) {
        if (record != 1) {
            return SQL_NO_DATA;
        }
        if (identifier != SQL_DIAG_MESSAGE_TEXT) {
            return SQL_ERROR;
        }
        const SQLSMALLINT chars = TruncationWriteText(TRUNCATION_BASE_MESSAGE, static_cast<SQLTCHAR*>(info),
            TruncationRoom(buffer_length));
        if (length != nullptr) {
            *length = static_cast<SQLSMALLINT>(chars * sizeof(SQLTCHAR));
        }
        return SQL_SUCCESS;
    }

    class TruncationFakeLoader : public RdsLibLoader {
    public:
        RdsPlatform::FuncHandle GetFunction(const std::string& function_name) override {
            if (function_name == RdsFuncNames::GetInfo) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&TruncationFakeGetInfo);
            }
            if (function_name == RdsFuncNames::GetDiagRec) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&TruncationFakeGetDiagRec);
            }
            if (function_name == RdsFuncNames::GetDiagField) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&TruncationFakeGetDiagField);
            }
            return nullptr;
        }
    };
}  // namespace

// A value SQLGetInfo cuts itself comes with a 01004 record on the connection, which SQLGetDiagRec and SQLGetDiagField
// return as record 1. The next SQLGetInfo replaces it: a value that fits leaves no record, and a value from the base
// driver leaves the base driver's own record readable, not hidden behind the wrapper's. SQLGetDiagField, which cuts
// fields too, posts nothing for itself.
TEST_F(ErrorHandlingTest, GetInfo_TruncatedValueHasARecord) {
    const std::string name(DRIVER_NAME);
    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native = 0;
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT message_length = 0;

    dbc_->err = std::make_unique<ErrInfo>("General error", ERR_GENERAL_ERROR);
    SQLTCHAR short_field[3] = {0};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_MESSAGE_TEXT, short_field,
        static_cast<SQLSMALLINT>(sizeof(short_field)), nullptr));
    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ("HY000", dbc_->err->sqlstate);

    std::vector<SQLTCHAR> value(8, 0x5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, value.data(),
        static_cast<SQLSMALLINT>(value.size() * sizeof(SQLTCHAR)), nullptr));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, dbc_, 1, state, &native, message, MAX_MSG_LEN,
        &message_length, false));
    EXPECT_EQ("01004", AppText(state));
    EXPECT_EQ(static_cast<SQLINTEGER>(WARN_STRING_DATA_RIGHT_TRUNCATED), native);
    EXPECT_EQ("SQLGetInfo - The value was truncated: the buffer needs "
        + std::to_string((name.size() + 1) * sizeof(SQLTCHAR)) + " bytes", AppText(message));
    SQLTCHAR field[MAX_SQL_STATE_LEN] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_SQLSTATE, field,
        static_cast<SQLSMALLINT>(sizeof(field)), nullptr));
    EXPECT_EQ("01004", AppText(field));
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, dbc_, 2, state, &native, message, MAX_MSG_LEN,
        &message_length, false));

    std::vector<SQLTCHAR> roomy(name.size() + 1, 0x5A);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, roomy.data(),
        static_cast<SQLSMALLINT>(roomy.size() * sizeof(SQLTCHAR)), nullptr));
    EXPECT_FALSE(dbc_->err);
    EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, dbc_, 1, state, &native, message, MAX_MSG_LEN,
        &message_length, false));

    // On a connected handle: the wrapper cuts its own value, then the base driver cuts one of its values.
    int fake_dbc = 0;
    env_->driver_lib_loader = std::make_shared<TruncationFakeLoader>();
    dbc_->plugin_service = std::make_shared<ConnectOnlyPluginService>(env_);
    dbc_->wrapped_dbc = &fake_dbc;  // only the fake loader's functions get it
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, value.data(),
        static_cast<SQLSMALLINT>(value.size() * sizeof(SQLTCHAR)), nullptr));
    ASSERT_TRUE(dbc_->err);
    EXPECT_EQ("01004", dbc_->err->sqlstate);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(dbc_, SQL_DBMS_NAME, value.data(),
        static_cast<SQLSMALLINT>(value.size() * sizeof(SQLTCHAR)), nullptr));
    EXPECT_FALSE(dbc_->err);
    native = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, dbc_, 1, state, &native, message, MAX_MSG_LEN,
        &message_length, false));
    EXPECT_EQ("01004", AppText(state));
    EXPECT_EQ(7, native);
    EXPECT_EQ(TRUNCATION_BASE_MESSAGE, AppText(message));
    SQLTCHAR base_message[MAX_MSG_LEN] = {0};
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, dbc_, 1, SQL_DIAG_MESSAGE_TEXT, base_message,
        static_cast<SQLSMALLINT>(sizeof(base_message)), nullptr));
    EXPECT_EQ(TRUNCATION_BASE_MESSAGE, AppText(base_message));
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
}

// A base driver's record: its message goes into the app's buffer up to the terminator, and the bytes past it keep what
// the app had there. With the app's and the base driver's characters the same size, the wrapper used to copy all of the
// claimed buffer from its own.
TEST_F(ErrorHandlingTest, DiagRec_BaseDriverMessageWritesOnlyTheString) {
    constexpr SQLTCHAR GUARD = 0x5A;
    const std::string base_message(TRUNCATION_BASE_MESSAGE);
    int fake_dbc = 0;
    env_->driver_lib_loader = std::make_shared<TruncationFakeLoader>();
    dbc_->plugin_service = std::make_shared<ConnectOnlyPluginService>(env_);
    dbc_->wrapped_dbc = &fake_dbc;  // only the fake loader's functions get it

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native = 0;
    std::vector<SQLTCHAR> message(MAX_MSG_LEN, GUARD);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, dbc_, 1, state, &native, message.data(),
        static_cast<SQLSMALLINT>(message.size()), &length, false));
    EXPECT_EQ("01004", AppText(state));
    EXPECT_EQ(base_message, AppText(message.data()));
    const auto past = message.begin() + static_cast<std::ptrdiff_t>(base_message.size() + 1);
    EXPECT_EQ(static_cast<size_t>(message.end() - past), static_cast<size_t>(std::count(past, message.end(), GUARD)));
    dbc_->wrapped_dbc = SQL_NULL_HDBC;
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

// The room is counted in whole characters of the app, as for SQLGetDiagField: the name's characters and one byte more
// (or three more for a 4-byte app) leave no room for the terminator, so the value is cut and the call says so.
TEST_F(ErrorHandlingTest, GetInfo_BufferLengthCountsWholeCharacters) {
    const std::string name(DRIVER_NAME);
    std::vector<SQLTCHAR> value(name.size() + 1, 0x5A);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, value.data(),
        static_cast<SQLSMALLINT>(name.size() * sizeof(SQLTCHAR) + 1), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(name.size() * sizeof(SQLTCHAR)), length);
    EXPECT_EQ(name.substr(0, name.size() - 1), AppText(value.data()));

    env_->use_4_bytes_user_app = true;
    std::vector<uint32_t> wide(name.size() + 1, 0x5A5A5A5A);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(dbc_, SQL_DRIVER_NAME, wide.data(),
        static_cast<SQLSMALLINT>(name.size() * sizeof(uint32_t) + 3), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(name.size() * sizeof(uint32_t)), length);
    EXPECT_EQ(name.substr(0, name.size() - 1), AppText4(wide.data()));
    env_->use_4_bytes_user_app = false;
}

namespace {
    // The base driver for the conversion path, where the app's and the base driver's SQLWCHAR sizes differ. It answers
    // as psqlODBC does: a string in its own characters, cut to BufferLength bytes, and the full length in bytes; a
    // number in its SQLUSMALLINT or SQLUINTEGER, and its size; for SQLColAttribute, a length of 0 for a numeric field.
    bool g_conversion_driver_4_byte = false;
    SQLSMALLINT g_conversion_buffer_length = 0;  // the BufferLength the base driver was last given
    // Every BufferLength it was given: the wrapper calls once more, with room for the whole string, after a cut.
    std::vector<SQLSMALLINT> g_conversion_buffer_lengths;
    constexpr SQLUINTEGER CONVERSION_BITMASK = 0x00012345;  // wider than 16 bits, so a cut to one SQLWCHAR shows
    constexpr const char* CONVERSION_TEXT = "PostgreSQL";
    constexpr const char* CONVERSION_YEAR = "1995";
    constexpr unsigned char CONVERSION_GUARD = 0x5A;

    SQLSMALLINT ConversionDriverString(std::string_view text, SQLPOINTER value, SQLSMALLINT buffer_length) {
        const size_t width = g_conversion_driver_4_byte ? 4 : 2;
        const size_t room = buffer_length > 0 ? static_cast<size_t>(buffer_length) / width : 0;
        if (value != nullptr && room > 0) {
            const size_t copied = std::min(text.size(), room - 1);
            auto* const bytes = static_cast<unsigned char*>(value);
            std::fill_n(bytes, (copied + 1) * width, 0);
            for (size_t i = 0; i < copied; ++i) {
                bytes[i * width] = static_cast<unsigned char>(text[i]);
            }
        }
        return static_cast<SQLSMALLINT>(text.size() * width);
    }

    // SQL_SUCCESS_WITH_INFO when a string was cut, as psqlODBC returns it.
    SQLRETURN ConversionDriverResult(SQLPOINTER value, SQLSMALLINT full, SQLSMALLINT buffer_length) {
        return value != nullptr && full >= buffer_length ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    }

    SQLRETURN SQL_API ConversionFakeGetInfo(SQLHDBC /*dbc*/, SQLUSMALLINT info_type, SQLPOINTER value,
                                            SQLSMALLINT buffer_length, SQLSMALLINT* length) {
        g_conversion_buffer_length = buffer_length;
        g_conversion_buffer_lengths.push_back(buffer_length);
        SQLSMALLINT full = 0;
        switch (info_type) {
            case SQL_SQL92_PREDICATES:
                if (value != nullptr) {
                    *static_cast<SQLUINTEGER*>(value) = CONVERSION_BITMASK;
                }
                full = sizeof(SQLUINTEGER);
                break;
            case SQL_CURSOR_COMMIT_BEHAVIOR:
                if (value != nullptr) {
                    *static_cast<SQLUSMALLINT*>(value) = SQL_CB_PRESERVE;
                }
                full = sizeof(SQLUSMALLINT);
                break;
            case SQL_XOPEN_CLI_YEAR:
                full = ConversionDriverString(CONVERSION_YEAR, value, buffer_length);
                if (length != nullptr) {
                    *length = full;
                }
                return ConversionDriverResult(value, full, buffer_length);
            default:
                full = ConversionDriverString(CONVERSION_TEXT, value, buffer_length);
                if (length != nullptr) {
                    *length = full;
                }
                return ConversionDriverResult(value, full, buffer_length);
        }
        if (length != nullptr) {
            *length = full;
        }
        return SQL_SUCCESS;
    }

    // The base driver's record on any handle: its message as SQL_DIAG_MESSAGE_TEXT.
    SQLRETURN SQL_API ConversionFakeGetDiagField(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/,
                                                 SQLSMALLINT /*record*/, SQLSMALLINT /*identifier*/, SQLPOINTER info,
                                                 SQLSMALLINT buffer_length, SQLSMALLINT* length) {
        g_conversion_buffer_length = buffer_length;
        g_conversion_buffer_lengths.push_back(buffer_length);
        const SQLSMALLINT full = ConversionDriverString(CONVERSION_TEXT, info, buffer_length);
        if (length != nullptr) {
            *length = full;
        }
        return ConversionDriverResult(info, full, buffer_length);
    }

    SQLRETURN SQL_API ConversionFakeColAttribute(SQLHSTMT /*stmt*/, SQLUSMALLINT /*column*/, SQLUSMALLINT field,
                                                 SQLPOINTER character, SQLSMALLINT buffer_length, SQLSMALLINT* length,
                                                 SQLLEN* numeric) {
        g_conversion_buffer_length = buffer_length;
        g_conversion_buffer_lengths.push_back(buffer_length);
        if (length != nullptr) {
            *length = 0;
        }
        if (field == SQL_DESC_NAME || field == SQL_COLUMN_NAME) {
            const SQLSMALLINT full = ConversionDriverString(CONVERSION_TEXT, character, buffer_length);
            if (length != nullptr) {
                *length = full;
            }
            return ConversionDriverResult(character, full, buffer_length);
        }
        if (numeric != nullptr) {
            *numeric = CONVERSION_BITMASK;
        }
        return SQL_SUCCESS;
    }

    class ConversionFakeLoader : public RdsLibLoader {
    public:
        RdsPlatform::FuncHandle GetFunction(const std::string& function_name) override {
            if (function_name == RdsFuncNames::GetInfo) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&ConversionFakeGetInfo);
            }
            if (function_name == RdsFuncNames::ColAttribute) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&ConversionFakeColAttribute);
            }
            if (function_name == RdsFuncNames::GetDiagField) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&ConversionFakeGetDiagField);
            }
            return nullptr;
        }
    };

    // N values with guard bytes after them, all set to CONVERSION_GUARD.
    template <typename T, size_t N>
    struct Guarded {
        T value[N];
        unsigned char guard[256];

        Guarded() { std::fill_n(reinterpret_cast<unsigned char*>(this), sizeof(*this), CONVERSION_GUARD); }

        [[nodiscard]] bool GuardIntact() const {
            return std::all_of(guard, guard + sizeof(guard), [](unsigned char c) { return c == CONVERSION_GUARD; });
        }
    };
}  // namespace

// The Unicode driver's conversion path: a 4-byte app with a 2-byte base driver (an iODBC app with a psqlODBC built
// against unixODBC), or a 2-byte app with a 4-byte base driver (a unixODBC app with a base driver built against iODBC).
class ConversionPathTest : public testing::Test {
protected:
    ENV env_;
    DBC dbc_{};
    STMT stmt_{};
    DESC desc_{};
    int fake_env_ = 0;
    int fake_dbc_ = 0;
    int fake_stmt_ = 0;
    int fake_desc_ = 0;

    void SetUp() override {
        env_.driver_lib_loader = std::make_shared<ConversionFakeLoader>();
        env_.wrapped_env = &fake_env_;
        env_.dbc_list.push_back(&dbc_);
        dbc_.env = &env_;
        dbc_.wrapped_dbc = &fake_dbc_;
        dbc_.plugin_service = std::make_shared<ConnectOnlyPluginService>(&env_);
        stmt_.dbc = &dbc_;
        OdbcHelper::SetWrappedStmt(&stmt_, &fake_stmt_);
        desc_.dbc = &dbc_;
        desc_.wrapped_desc = &fake_desc_;
    }

    void TearDown() override {
        desc_.wrapped_desc = SQL_NULL_HDESC;
        OdbcHelper::SetWrappedStmt(&stmt_, SQL_NULL_HSTMT);
        dbc_.wrapped_dbc = SQL_NULL_HDBC;
        env_.dbc_list.clear();
        env_.wrapped_env = SQL_NULL_HENV;
        g_conversion_driver_4_byte = false;
        g_conversion_buffer_length = 0;
        g_conversion_buffer_lengths.clear();
    }

    void UseFourByteApp() {
        env_.use_4_bytes_user_app = true;
        g_conversion_driver_4_byte = false;
        dbc_.plugin_service->GetOdbcHelper()->SetUse4BytesBaseDriver(false);
    }

    void UseFourByteDriver() {
        env_.use_4_bytes_user_app = false;
        g_conversion_driver_4_byte = true;
        dbc_.plugin_service->GetOdbcHelper()->SetUse4BytesBaseDriver(true);
    }
};

// A numeric info type isn't text: it goes into the app's buffer as the base driver writes it, with or without a length
// pointer and whatever BufferLength the app passes (ODBC ignores it for these types). It went through the string
// conversion (0x12345 came back as 0, or 0x2345 from a 64-byte buffer, and the length doubled), and without a length
// pointer it was copied as BufferLength SQLWCHARs, twice the buffer.
TEST_F(ConversionPathTest, GetInfo_NumericInfoTypeSkipsTheConversion) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app" : "4-byte base driver");
        if (four_byte_app) {
            UseFourByteApp();
        } else {
            UseFourByteDriver();
        }
        ASSERT_TRUE(dbc_.plugin_service->GetOdbcHelper()->NeedsConversion());
        for (const SQLSMALLINT buffer_length : {SQLSMALLINT{4}, SQLSMALLINT{64}}) {
            SCOPED_TRACE(buffer_length);
            Guarded<SQLUINTEGER, 1> with_length;
            SQLSMALLINT length = 0;
            EXPECT_EQ(SQL_SUCCESS,
                RDS_SQLGetInfo(&dbc_, SQL_SQL92_PREDICATES, with_length.value, buffer_length, &length));
            EXPECT_EQ(CONVERSION_BITMASK, with_length.value[0]);
            EXPECT_EQ(4, length);
            EXPECT_TRUE(with_length.GuardIntact());

            Guarded<SQLUINTEGER, 1> without_length;
            EXPECT_EQ(SQL_SUCCESS,
                RDS_SQLGetInfo(&dbc_, SQL_SQL92_PREDICATES, without_length.value, buffer_length, nullptr));
            EXPECT_EQ(CONVERSION_BITMASK, without_length.value[0]);
            EXPECT_TRUE(without_length.GuardIntact());
        }
    }
}

// iODBC's own call on every connect, from a 4-byte app (iODBC passes wchar_t through): SQL_CURSOR_COMMIT_BEHAVIOR into
// an SQLSMALLINT, BufferLength 2, no length pointer. Four bytes were copied into the two.
TEST_F(ConversionPathTest, GetInfo_IodbcConnectCallStaysInItsBuffer) {
    UseFourByteApp();
    Guarded<SQLUSMALLINT, 1> behavior;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_CURSOR_COMMIT_BEHAVIOR, behavior.value,
        static_cast<SQLSMALLINT>(sizeof(behavior.value)), nullptr));
    EXPECT_EQ(SQL_CB_PRESERVE, behavior.value[0]);
    EXPECT_TRUE(behavior.GuardIntact());
}

// unixODBC's own call on every connect, with a base driver whose SQLWCHAR is 4 bytes: SQL_XOPEN_CLI_YEAR into a 12-byte
// block, no length pointer. 24 unconverted bytes were copied into the 12. The base driver gets 24 bytes, its 6
// characters, so the whole year fits; with the app's 12 it had room for 2 characters and came back cut.
TEST_F(ConversionPathTest, GetInfo_UnixOdbcConnectCallStaysInItsBuffer) {
    const std::string year_text(CONVERSION_YEAR);
    UseFourByteDriver();
    Guarded<SQLTCHAR, 6> year;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_XOPEN_CLI_YEAR, year.value,
        static_cast<SQLSMALLINT>(sizeof(year.value)), nullptr));
    EXPECT_EQ(24, g_conversion_buffer_length);
    EXPECT_EQ(year_text, AppText(year.value));
    EXPECT_TRUE(year.GuardIntact());

    // The same with a length pointer: 4 SQLWCHARs.
    Guarded<SQLTCHAR, 6> again;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_XOPEN_CLI_YEAR, again.value,
        static_cast<SQLSMALLINT>(sizeof(again.value)), &length));
    EXPECT_EQ(year_text, AppText(again.value));
    EXPECT_EQ(static_cast<SQLSMALLINT>(year_text.size() * sizeof(SQLTCHAR)), length);
    EXPECT_TRUE(again.GuardIntact());
}

// A string comes back in the app's characters with or without a length pointer: the base driver always gets one, so
// the wrapper knows what it wrote. Without one, the base driver's bytes were copied unconverted, twice the buffer.
TEST_F(ConversionPathTest, GetInfo_StringIsConvertedWithoutALengthPointer) {
    const std::string text(CONVERSION_TEXT);
    UseFourByteApp();
    for (const bool with_length : {true, false}) {
        Guarded<uint32_t, 16> value;
        SQLSMALLINT length = 0;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, value.value,
            static_cast<SQLSMALLINT>(sizeof(value.value)), with_length ? &length : nullptr));
        EXPECT_EQ(text, AppText4(value.value));
        EXPECT_EQ(with_length ? static_cast<SQLSMALLINT>(text.size() * sizeof(uint32_t)) : 0, length);
        EXPECT_TRUE(value.GuardIntact());
    }

    UseFourByteDriver();
    for (const bool with_length : {true, false}) {
        Guarded<SQLTCHAR, 32> value;
        SQLSMALLINT length = 0;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, value.value,
            static_cast<SQLSMALLINT>(sizeof(value.value)), with_length ? &length : nullptr));
        EXPECT_EQ(text, AppText(value.value));
        EXPECT_EQ(with_length ? static_cast<SQLSMALLINT>(text.size() * sizeof(SQLTCHAR)) : 0, length);
        EXPECT_TRUE(value.GuardIntact());
    }
}

// SQLColAttribute's numeric field goes to NumericAttributePtr, and the character buffer the app passed as well isn't
// written. psqlODBC sets the length to 0 for it, which sent the call to the copy of BufferLength SQLWCHARs.
TEST_F(ConversionPathTest, ColAttribute_NumericFieldSkipsTheConversion) {
    for (const bool four_byte_app : {true, false}) {
        SCOPED_TRACE(four_byte_app ? "4-byte app" : "4-byte base driver");
        if (four_byte_app) {
            UseFourByteApp();
        } else {
            UseFourByteDriver();
        }
        Guarded<unsigned char, 16> character;
        SQLSMALLINT length = 77;
        SQLLEN numeric = 0;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLColAttribute(&stmt_, 1, SQL_DESC_LENGTH, character.value,
            static_cast<SQLSMALLINT>(sizeof(character.value)), &length, &numeric));
        EXPECT_EQ(static_cast<SQLLEN>(CONVERSION_BITMASK), numeric);
        EXPECT_EQ(0, length);
        EXPECT_EQ(CONVERSION_GUARD, character.value[0]);
        EXPECT_TRUE(character.GuardIntact());
    }
}

// A character field comes back in the app's characters with or without a length pointer, for the ODBC 3 identifier
// and for the ODBC 2 one that SQLColAttributesW passes to RDS_SQLColAttribute unchanged.
TEST_F(ConversionPathTest, ColAttribute_StringFieldIsConvertedWithoutALengthPointer) {
    const std::string text(CONVERSION_TEXT);
    for (const SQLUSMALLINT field : {SQLUSMALLINT{SQL_DESC_NAME}, SQLUSMALLINT{SQL_COLUMN_NAME}}) {
        SCOPED_TRACE(field);
        UseFourByteApp();
        for (const bool with_length : {true, false}) {
            Guarded<uint32_t, 16> value;
            SQLSMALLINT length = 0;
            EXPECT_EQ(SQL_SUCCESS, RDS_SQLColAttribute(&stmt_, 1, field, value.value,
                static_cast<SQLSMALLINT>(sizeof(value.value)), with_length ? &length : nullptr, nullptr));
            EXPECT_EQ(text, AppText4(value.value));
            EXPECT_EQ(with_length ? static_cast<SQLSMALLINT>(text.size() * sizeof(uint32_t)) : 0, length);
            EXPECT_TRUE(value.GuardIntact());
        }

        UseFourByteDriver();
        for (const bool with_length : {true, false}) {
            Guarded<SQLTCHAR, 32> value;
            SQLSMALLINT length = 0;
            EXPECT_EQ(SQL_SUCCESS, RDS_SQLColAttribute(&stmt_, 1, field, value.value,
                static_cast<SQLSMALLINT>(sizeof(value.value)), with_length ? &length : nullptr, nullptr));
            EXPECT_EQ(text, AppText(value.value));
            EXPECT_EQ(with_length ? static_cast<SQLSMALLINT>(text.size() * sizeof(SQLTCHAR)) : 0, length);
            EXPECT_TRUE(value.GuardIntact());
        }
    }
}

// The base driver gets the app's buffer in its own characters: as many as the app's buffer holds, in bytes of the base
// driver's. A 4-byte app's 24 bytes hold 6 characters, 12 bytes of a 2-byte base driver: it cuts "PostgreSQL" to
// "Postg" and says so. Given the app's 24 bytes, it wrote the whole value and returned SQL_SUCCESS, and the conversion
// cut it with no warning. A 2-byte app's 24 bytes hold 12 characters, 48 bytes of a 4-byte base driver: the whole value
// fits, where 24 bytes let the base driver write only "Postg".
TEST_F(ConversionPathTest, GetInfo_BaseDriverGetsTheBufferInItsOwnCharacters) {
    const std::string text(CONVERSION_TEXT);
    UseFourByteApp();
    Guarded<uint32_t, 6> wide;
    SQLSMALLINT length = 0;
    g_conversion_buffer_lengths.clear();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, wide.value,
        static_cast<SQLSMALLINT>(sizeof(wide.value)), &length));
    EXPECT_EQ(12, g_conversion_buffer_lengths.front());
    EXPECT_EQ(text.substr(0, 5), AppText4(wide.value));
    EXPECT_EQ(static_cast<SQLSMALLINT>(text.size() * sizeof(uint32_t)), length);
    EXPECT_TRUE(wide.GuardIntact());

    UseFourByteDriver();
    Guarded<SQLTCHAR, 12> narrow;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_DBMS_NAME, narrow.value,
        static_cast<SQLSMALLINT>(sizeof(narrow.value)), &length));
    EXPECT_EQ(48, g_conversion_buffer_length);
    EXPECT_EQ(text, AppText(narrow.value));
    EXPECT_EQ(static_cast<SQLSMALLINT>(text.size() * sizeof(SQLTCHAR)), length);
    EXPECT_TRUE(narrow.GuardIntact());
}

// The same for SQLGetDiagField on a base driver's record, on each handle type.
TEST_F(ConversionPathTest, GetDiagField_BaseDriverGetsTheBufferInItsOwnCharacters) {
    const std::string text(CONVERSION_TEXT);
    const std::vector<std::pair<SQLSMALLINT, SQLHANDLE>> handles = {
        {SQL_HANDLE_ENV, &env_}, {SQL_HANDLE_DBC, &dbc_}, {SQL_HANDLE_STMT, &stmt_}, {SQL_HANDLE_DESC, &desc_}};
    for (const auto& [handle_type, handle] : handles) {
        SCOPED_TRACE(handle_type);
        UseFourByteApp();
        Guarded<uint32_t, 6> wide;
        SQLSMALLINT length = 0;
        g_conversion_buffer_lengths.clear();
        EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLGetDiagField(handle_type, handle, 1, SQL_DIAG_MESSAGE_TEXT, wide.value,
            static_cast<SQLSMALLINT>(sizeof(wide.value)), &length));
        EXPECT_EQ(12, g_conversion_buffer_lengths.front());
        EXPECT_EQ(text.substr(0, 5), AppText4(wide.value));
        EXPECT_EQ(static_cast<SQLSMALLINT>(text.size() * sizeof(uint32_t)), length);
        EXPECT_TRUE(wide.GuardIntact());

        UseFourByteDriver();
        Guarded<SQLTCHAR, 12> narrow;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(handle_type, handle, 1, SQL_DIAG_MESSAGE_TEXT, narrow.value,
            static_cast<SQLSMALLINT>(sizeof(narrow.value)), &length));
        EXPECT_EQ(48, g_conversion_buffer_length);
        EXPECT_EQ(text, AppText(narrow.value));
        EXPECT_EQ(static_cast<SQLSMALLINT>(text.size() * sizeof(SQLTCHAR)), length);
        EXPECT_TRUE(narrow.GuardIntact());
    }
}

// The same for SQLColAttribute's character fields.
TEST_F(ConversionPathTest, ColAttribute_BaseDriverGetsTheBufferInItsOwnCharacters) {
    const std::string text(CONVERSION_TEXT);
    UseFourByteApp();
    Guarded<uint32_t, 6> wide;
    SQLSMALLINT length = 0;
    g_conversion_buffer_lengths.clear();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLColAttribute(&stmt_, 1, SQL_DESC_NAME, wide.value,
        static_cast<SQLSMALLINT>(sizeof(wide.value)), &length, nullptr));
    EXPECT_EQ(12, g_conversion_buffer_lengths.front());
    EXPECT_EQ(text.substr(0, 5), AppText4(wide.value));
    EXPECT_EQ(static_cast<SQLSMALLINT>(text.size() * sizeof(uint32_t)), length);
    EXPECT_TRUE(wide.GuardIntact());

    UseFourByteDriver();
    Guarded<SQLTCHAR, 12> narrow;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLColAttribute(&stmt_, 1, SQL_DESC_NAME, narrow.value,
        static_cast<SQLSMALLINT>(sizeof(narrow.value)), &length, nullptr));
    EXPECT_EQ(48, g_conversion_buffer_length);
    EXPECT_EQ(text, AppText(narrow.value));
    EXPECT_EQ(static_cast<SQLSMALLINT>(text.size() * sizeof(SQLTCHAR)), length);
    EXPECT_TRUE(narrow.GuardIntact());
}

// OdbcHelper::ConvertDriverOutputToTarget writes the base driver's string and its terminator into the app's buffer, not
// all of it: the rest keeps what the app had there, for each pair of character sizes.
TEST_F(ConversionPathTest, ConvertDriverOutput_WritesOnlyTheString) {
    const auto helper = dbc_.plugin_service->GetOdbcHelper();
    const uint32_t wide_guard = 0x5A5A5A5A;
    const auto narrow_guard = static_cast<SQLTCHAR>(0x5A5A);
    std::vector<SQLTCHAR> utf16(16, 0);  // "ab" from a 2-byte base driver, and the zeros after it
    utf16[0] = 'a';
    utf16[1] = 'b';
    std::vector<uint32_t> utf32(16, 0);  // the same from a 4-byte one
    utf32[0] = 'a';
    utf32[1] = 'b';

    UseFourByteApp();
    Guarded<uint32_t, 16> wide;
    helper->ConvertDriverOutputToTarget(utf16.data(), reinterpret_cast<SQLTCHAR*>(wide.value),
        utf16.size() * sizeof(SQLTCHAR), sizeof(wide.value));
    EXPECT_EQ((std::vector<uint32_t>{'a', 'b', 0}), std::vector<uint32_t>(wide.value, wide.value + 3));
    EXPECT_EQ(13, std::count(wide.value + 3, wide.value + 16, wide_guard));

    helper->SetUse4BytesBaseDriver(true);  // 4-byte on both sides
    Guarded<uint32_t, 16> same_wide;
    helper->ConvertDriverOutputToTarget(reinterpret_cast<SQLTCHAR*>(utf32.data()),
        reinterpret_cast<SQLTCHAR*>(same_wide.value), utf32.size() * sizeof(uint32_t), sizeof(same_wide.value));
    EXPECT_EQ((std::vector<uint32_t>{'a', 'b', 0}), std::vector<uint32_t>(same_wide.value, same_wide.value + 3));
    EXPECT_EQ(13, std::count(same_wide.value + 3, same_wide.value + 16, wide_guard));

    UseFourByteDriver();
    Guarded<SQLTCHAR, 16> narrow;
    helper->ConvertDriverOutputToTarget(reinterpret_cast<SQLTCHAR*>(utf32.data()), narrow.value,
        utf32.size() * sizeof(uint32_t), sizeof(narrow.value));
    EXPECT_EQ("ab", AppText(narrow.value));
    EXPECT_EQ(13, std::count(narrow.value + 3, narrow.value + 16, narrow_guard));

    helper->SetUse4BytesBaseDriver(false);  // 2-byte on both sides
    Guarded<SQLTCHAR, 16> same_narrow;
    helper->ConvertDriverOutputToTarget(utf16.data(), same_narrow.value, utf16.size() * sizeof(SQLTCHAR),
        sizeof(same_narrow.value));
    EXPECT_EQ("ab", AppText(same_narrow.value));
    EXPECT_EQ(13, std::count(same_narrow.value + 3, same_narrow.value + 16, narrow_guard));
}
#endif

// A handle reused after SQLDisconnect (the Windows driver manager keeps the driver's handle for the next connect)
// starts from the new connection string: nothing an earlier connect parsed or a plugin added carries over.
TEST_F(ConnectPathTest, DriverConnect_ReusedHandleStartsFromTheNewString) {
    plugin_->added = {{KEY_DB_PASSWORD, "token-from-the-first-connect"}, {"PQOPT", "{sslrootcert=system}"}};
    ASSERT_EQ(SQL_SUCCESS, DriverConnect(dbc_, ConnStr("first.example.com", ";UID=first_user"), nullptr, 0, nullptr));
    plugin_->added.clear();

    ASSERT_EQ(SQL_SUCCESS, DriverConnect(dbc_, ConnStr("second.example.com"), nullptr, 0, nullptr));
    ASSERT_EQ(2U, plugin_->seen.size());
    const std::map<std::string, std::string>& second = plugin_->seen[1];
    EXPECT_EQ("second.example.com", second.at(KEY_SERVER));
    EXPECT_FALSE(second.contains(KEY_DB_USERNAME));
    EXPECT_FALSE(second.contains(KEY_DB_PASSWORD));
    EXPECT_FALSE(second.contains("PQOPT"));
}

// The same after a failed connect, where no SQLDisconnect comes in between.
TEST_F(ConnectPathTest, DriverConnect_AfterAFailedConnectStartsFromTheNewString) {
    plugin_->result = SQL_ERROR;
    plugin_->added = {{"PQOPT", "{sslrootcert=system}"}};
    EXPECT_EQ(SQL_ERROR, DriverConnect(dbc_, ConnStr("first.example.com", ";UID=first_user"), nullptr, 0, nullptr));
    plugin_->result = SQL_SUCCESS;
    plugin_->added.clear();

    ASSERT_EQ(SQL_SUCCESS, DriverConnect(dbc_, ConnStr("second.example.com"), nullptr, 0, nullptr));
    ASSERT_EQ(2U, plugin_->seen.size());
    EXPECT_FALSE(plugin_->seen[1].contains(KEY_DB_USERNAME));
    EXPECT_FALSE(plugin_->seen[1].contains("PQOPT"));
}

// What the application set with SQLSetConnectAttr is kept: a driver manager that reuses the handle expects it.
TEST_F(ConnectPathTest, DriverConnect_ReusedHandleKeepsConnectAttributes) {
    const SQLPOINTER login_timeout = reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(7));
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_LOGIN_TIMEOUT, login_timeout, 0));
    ASSERT_EQ(SQL_SUCCESS, DriverConnect(dbc_, ConnStr("first.example.com"), nullptr, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, DriverConnect(dbc_, ConnStr("second.example.com"), nullptr, 0, nullptr));
    ASSERT_TRUE(dbc_.attr_map.contains(SQL_ATTR_LOGIN_TIMEOUT));
    EXPECT_EQ(login_timeout, dbc_.attr_map.at(SQL_ATTR_LOGIN_TIMEOUT).first);
}

// SQLConnect too. The DSN isn't registered, so nothing names a base driver and the connect fails before the plugin
// chain; the earlier connect's BASE_DRIVER and SERVER used to carry it through.
TEST_F(ConnectPathTest, Connect_ReusedHandleStartsFromItsArguments) {
    ASSERT_EQ(SQL_SUCCESS, DriverConnect(dbc_, ConnStr("first.example.com"), nullptr, 0, nullptr));

    std::vector<SQLTCHAR> dsn = AppString("no-such-dsn-connect-path-test");
    std::vector<SQLTCHAR> user = AppString("second_user");
    std::vector<SQLTCHAR> password = AppString("second_password");
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(&dbc_, dsn.data(), SQL_NTS, user.data(), SQL_NTS, password.data(), SQL_NTS));
    EXPECT_EQ(1U, plugin_->seen.size());
    EXPECT_FALSE(dbc_.conn_attr.contains(KEY_SERVER));
    EXPECT_FALSE(dbc_.conn_attr.contains(KEY_BASE_DRIVER));
    EXPECT_EQ("second_user", dbc_.conn_attr.at(KEY_DB_USERNAME));
    ASSERT_TRUE(dbc_.err);
    EXPECT_EQ(static_cast<int>(ERR_NO_UNDER_LYING_DRIVER), dbc_.err->native_err);
}
