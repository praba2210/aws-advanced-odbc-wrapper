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

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../driver/driver.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/logger_wrapper.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"
#include "../../driver/util/rds_strings.h"

// After a failed statement Aurora DSQL has ended the transaction, and psqlODBC reports the COMMIT that saves nothing
// as a success. SQLEndTran(SQL_COMMIT) checks first, with SELECT 1: 25P02 means the transaction ended, and the
// wrapper rolls it back and returns 25S03.

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string ROLLED_BACK =
        "The transaction was rolled back, not committed: a statement in it failed, and Aurora DSQL ends a transaction "
        "at its first error (it has no savepoints). Roll back after an error, and run the whole transaction again.";

    // What the fake base driver answers.
    SQLRETURN g_alloc_result = SQL_SUCCESS;  // SQLAllocHandle for a statement
    SQLRETURN g_exec_result = SQL_SUCCESS;  // SQLExecDirect
    SQLRETURN g_param_data_result = SQL_SUCCESS;
    // The SQLSTATE of the base driver's record 1 on any handle; empty for none.
    std::string g_state;
    // A base driver built with a 4-byte SQLWCHAR: its text and SQLSTATEs are UCS-4.
    bool g_four_byte_base = false;
    // What SQLEndTran returns for a handle; SQL_SUCCESS for one not in it.
    std::map<SQLHANDLE, SQLRETURN> g_end_tran_results;

    // What it got.
    int g_probe_stand_in = 0;
    int g_alloc_calls = 0;
    std::vector<std::string> g_exec_texts;
    std::vector<SQLSMALLINT> g_diag_handle_types;
    std::vector<SQLHANDLE> g_freed;
    // Each SQLEndTran's handle and completion type, in order.
    std::vector<std::pair<SQLHANDLE, SQLSMALLINT>> g_end_trans;

    // Text in a 2-byte SQLWCHAR's characters (ANSI ones in the ANSI build), to its NUL: the app's in these tests.
    std::string AppText(const SQLTCHAR* text) {
#if UNICODE
        return ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(const_cast<SQLTCHAR*>(text)));
#else
        return reinterpret_cast<const char*>(text);
#endif
    }

    // Text in the base driver's characters, to its NUL: a 4-byte one's, or as AppText reads it.
    std::string BaseText(const SQLTCHAR* text) {
#if UNICODE
        if (g_four_byte_base) {
            return Convert4ByteSqlWChar(text, SQL_NTS);
        }
#endif
        return AppText(text);
    }

    // Writes the text, terminated, in the base driver's characters: a 4-byte one as two SQLTCHARs, low half first.
    void WriteBaseText(SQLTCHAR* out, const std::string& text) {
        const size_t step = g_four_byte_base ? 2 : 1;
        for (size_t i = 0; i <= text.size(); ++i) {
            const auto c = static_cast<SQLTCHAR>(i < text.size() ? static_cast<unsigned char>(text[i]) : 0);
            out[i * step] = c;
            if (step == 2) {
                out[(i * step) + 1] = 0;
            }
        }
    }

    SQLRETURN FakeAllocHandle(SQLSMALLINT handle_type, SQLHANDLE /*input_handle*/, SQLHANDLE* output_handle) {
        ++g_alloc_calls;
        if (handle_type != SQL_HANDLE_STMT || !SQL_SUCCEEDED(g_alloc_result)) {
            *output_handle = SQL_NULL_HANDLE;
            return SQL_ERROR;
        }
        *output_handle = static_cast<SQLHANDLE>(&g_probe_stand_in);
        return g_alloc_result;
    }

    SQLRETURN FakeExecDirect(SQLHSTMT /*statement*/, SQLTCHAR* text, SQLINTEGER /*length*/) {
        g_exec_texts.push_back(BaseText(text));
        return g_exec_result;
    }

    SQLRETURN FakeGetDiagRec(SQLSMALLINT handle_type, SQLHANDLE /*handle*/, SQLSMALLINT record, SQLTCHAR* state,
                             SQLINTEGER* native_error, SQLTCHAR* message, SQLSMALLINT buffer_length,
                             SQLSMALLINT* text_length) {
        g_diag_handle_types.push_back(handle_type);
        if (record != 1 || g_state.empty()) {
            return SQL_NO_DATA;
        }
        if (state != nullptr) {
            WriteBaseText(state, g_state);
        }
        if (message != nullptr && buffer_length > 0) {
            WriteBaseText(message, "");
        }
        if (native_error != nullptr) {
            *native_error = 0;
        }
        if (text_length != nullptr) {
            *text_length = 0;
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeFreeHandle(SQLSMALLINT /*handle_type*/, SQLHANDLE handle) {
        g_freed.push_back(handle);
        return SQL_SUCCESS;
    }

    SQLRETURN FakeEndTran(SQLSMALLINT /*handle_type*/, SQLHANDLE handle, SQLSMALLINT completion_type) {
        g_end_trans.emplace_back(handle, completion_type);
        const auto it = g_end_tran_results.find(handle);
        return it != g_end_tran_results.end() ? it->second : SQL_SUCCESS;
    }

    SQLRETURN FakeParamData(SQLHSTMT /*statement*/, SQLPOINTER* /*value*/) {
        return g_param_data_result;
    }

    SQLRETURN FakeSetConnectAttr(SQLHDBC /*connection*/, SQLINTEGER /*attribute*/, SQLPOINTER /*value*/,
                                 SQLINTEGER /*length*/) {
        return SQL_SUCCESS;
    }

    SQLRETURN FakeSetEnvAttr(SQLHENV /*environment*/, SQLINTEGER /*attribute*/, SQLPOINTER /*value*/,
                             SQLINTEGER /*length*/) {
        return SQL_SUCCESS;
    }

    // Answers the calls of the check before a commit, SQLEndTran, SQLParamData, SQLSetConnectAttr and SQLSetEnvAttr.
    class FakeBaseDriver : public RdsLibLoader {
    public:
        RdsPlatform::FuncHandle GetFunction(const std::string& function_name) override {
            const std::map<std::string_view, RdsPlatform::FuncHandle> functions = {
                {RdsFuncNames::AllocHandle, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeAllocHandle)},
                {RdsFuncNames::ExecDirect, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeExecDirect)},
                {RdsFuncNames::GetDiagRec, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetDiagRec)},
                {RdsFuncNames::FreeHandle, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeFreeHandle)},
                {RdsFuncNames::EndTran, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeEndTran)},
                {RdsFuncNames::ParamData, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeParamData)},
                {RdsFuncNames::SetConnectAttr, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSetConnectAttr)},
                {RdsFuncNames::SetEnvAttr, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSetEnvAttr)},
            };
            const auto it = functions.find(function_name);
            return it != functions.end() ? it->second : nullptr;
        }
    };

    // Collects the WARNING lines while it exists. It removes itself from ng-log even when an ASSERT returns early.
    class WarningSink : public nglog::LogSink {
    public:
        WarningSink() { nglog::AddLogSink(this); }
        ~WarningSink() override { nglog::RemoveLogSink(this); }
        WarningSink(const WarningSink&) = delete;
        WarningSink& operator=(const WarningSink&) = delete;

        void send(nglog::LogSeverity severity, const char* /*full_filename*/, const char* /*base_filename*/,
                  int /*line*/, const nglog::LogMessageTime& /*time*/, const char* message,
                  size_t message_len) override {
            if (severity != nglog::WARNING) {
                return;
            }
            const std::lock_guard<std::mutex> lock(mutex_);
            lines_.emplace_back(message, message_len);
        }

        // How many lines there are, or how many contain the text.
        std::ptrdiff_t Count(const std::string& text = "") {
            const std::lock_guard<std::mutex> lock(mutex_);
            return std::ranges::count_if(
                lines_, [&text](const std::string& line) { return line.find(text) != std::string::npos; });
        }

    private:
        std::mutex mutex_;
        std::vector<std::string> lines_;
    };
}  // namespace

// An Aurora DSQL connection with autocommit off, as a connect leaves it, over the fake base driver.
class DsqlTransactionAbortTest : public testing::Test {
protected:
    std::shared_ptr<FakeBaseDriver> base_driver_ = std::make_shared<FakeBaseDriver>();
    ENV env_;
    DBC dbc_{};
    DBC second_{};
    int base_env_stand_in_ = 0;
    int base_dbc_stand_in_ = 0;
    int second_base_dbc_stand_in_ = 0;

    void SetUp() override {
        g_alloc_result = SQL_SUCCESS;
        g_exec_result = SQL_SUCCESS;
        g_param_data_result = SQL_SUCCESS;
        g_state.clear();
        g_four_byte_base = false;
        g_end_tran_results.clear();
        g_alloc_calls = 0;
        g_exec_texts.clear();
        g_diag_handle_types.clear();
        g_freed.clear();
        g_end_trans.clear();
        env_.driver_lib_loader = base_driver_;
        env_.wrapped_env = static_cast<SQLHENV>(&base_env_stand_in_);
        Open(dbc_, base_dbc_stand_in_);
    }

    void TearDown() override {
        // The fake handles point at the test's members; never let them outlive the test.
        for (DBC* dbc : {&dbc_, &second_}) {
            dbc->wrapped_dbc = SQL_NULL_HDBC;
            dbc->plugin_service = nullptr;
        }
        env_.dbc_list.clear();
        env_.wrapped_env = SQL_NULL_HENV;
        env_.use_4_bytes_user_app = false;
    }

    // Connected to Aurora DSQL in manual-commit mode, in the environment's list as RDS_AllocDbc puts it.
    void Open(DBC& dbc, int& base_dbc) {
        dbc.env = &env_;
        dbc.is_dsql = true;
        dbc.wrapped_dbc = static_cast<SQLHDBC>(&base_dbc);
        dbc.conn_status = CONN_CONNECTED;
        dbc.auto_commit = false;
        dbc.transaction_status = TRANSACTION_OPEN;
        dbc.plugin_service = std::make_shared<PluginService>(
            base_driver_, &env_, std::map<std::string, std::string>{{KEY_SERVER, DSQL_HOST}});
        env_.dbc_list.push_back(&dbc);
    }

    // A statement failed, and Aurora DSQL ended the transaction: every statement after it fails with 25P02.
    void Abort(DBC& dbc) {
        dbc.dsql_statement_failed_in_transaction = true;
        g_exec_result = SQL_ERROR;
        g_state = "25P02";
    }

    SQLRETURN EndTran(SQLSMALLINT completion_type) {
        return RDS_SQLEndTran(SQL_HANDLE_DBC, &dbc_, completion_type);
    }

    // SQLGetDiagRec(SQL_HANDLE_ENV, 1)'s result.
    SQLRETURN EnvRecord() {
        SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
        SQLTCHAR message[MAX_MSG_LEN] = {0};
        SQLINTEGER native_error = 0;
        SQLSMALLINT length = 0;
        return RDS_SQLGetDiagRec(SQL_HANDLE_ENV, &env_, 1, state, &native_error, message, MAX_MSG_LEN, &length, false);
    }

    // The base SQLEndTran calls the connection got, in order.
    std::vector<std::pair<SQLHANDLE, SQLSMALLINT>> BaseEndTrans(std::vector<SQLSMALLINT> completions) {
        std::vector<std::pair<SQLHANDLE, SQLSMALLINT>> calls;
        for (const SQLSMALLINT completion : completions) {
            calls.emplace_back(static_cast<SQLHANDLE>(&base_dbc_stand_in_), completion);
        }
        return calls;
    }
};

TEST_F(DsqlTransactionAbortTest, Commit_AfterAFailedStatement_Aborted_RollsBackAndReturns25S03) {
    Abort(dbc_);
    WarningSink sink;
    EXPECT_EQ(SQL_ERROR, EndTran(SQL_COMMIT));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("25S03", dbc_.err->sqlstate);
    EXPECT_EQ(ROLLED_BACK, dbc_.err->error_msg);
    EXPECT_EQ(BaseEndTrans({SQL_ROLLBACK}), g_end_trans);
    EXPECT_EQ(std::vector<std::string>{"SELECT 1"}, g_exec_texts);
    EXPECT_EQ((std::vector<SQLSMALLINT>{SQL_HANDLE_STMT}), g_diag_handle_types);
    EXPECT_EQ(std::vector<SQLHANDLE>{&g_probe_stand_in}, g_freed);
    EXPECT_FALSE(dbc_.dsql_statement_failed_in_transaction);
    EXPECT_EQ(TRANSACTION_CLOSED, dbc_.transaction_status);
    EXPECT_EQ(1, sink.Count());
    EXPECT_EQ(1, sink.Count("[DSQL] " + ROLLED_BACK + " (the check before the commit got 25P02)"));
}

// The transaction is still open: the commit goes ahead, after the check.
TEST_F(DsqlTransactionAbortTest, Commit_AfterAFailedStatement_StillOpen_Commits) {
    dbc_.dsql_statement_failed_in_transaction = true;
    WarningSink sink;
    EXPECT_EQ(SQL_SUCCESS, EndTran(SQL_COMMIT));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_EQ(BaseEndTrans({SQL_COMMIT}), g_end_trans);
    EXPECT_EQ(std::vector<std::string>{"SELECT 1"}, g_exec_texts);
    EXPECT_EQ(std::vector<SQLHANDLE>{&g_probe_stand_in}, g_freed);
    EXPECT_FALSE(dbc_.dsql_statement_failed_in_transaction);
    EXPECT_EQ(TRANSACTION_CLOSED, dbc_.transaction_status);
    EXPECT_EQ(0, sink.Count());
}

// A check that fails some other way doesn't show the transaction ended: the commit goes ahead, as psqlODBC would make
// it, with a warning that names the state.
TEST_F(DsqlTransactionAbortTest, Commit_ProbeFailsWithoutAbortedState_CommitsAndLogs) {
    dbc_.dsql_statement_failed_in_transaction = true;
    g_exec_result = SQL_ERROR;
    g_state = "HY010";
    WarningSink sink;
    EXPECT_EQ(SQL_SUCCESS, EndTran(SQL_COMMIT));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_EQ(BaseEndTrans({SQL_COMMIT}), g_end_trans);
    EXPECT_EQ(std::vector<SQLHANDLE>{&g_probe_stand_in}, g_freed);
    EXPECT_FALSE(dbc_.dsql_statement_failed_in_transaction);
    EXPECT_EQ(1, sink.Count());
    EXPECT_EQ(1, sink.Count("HY010"));
}

TEST_F(DsqlTransactionAbortTest, Commit_NoFailedStatement_NoProbe) {
    g_exec_result = SQL_ERROR;
    g_state = "25P02";
    EXPECT_EQ(SQL_SUCCESS, EndTran(SQL_COMMIT));
    EXPECT_EQ(0, g_alloc_calls);
    EXPECT_TRUE(g_exec_texts.empty());
    EXPECT_EQ(BaseEndTrans({SQL_COMMIT}), g_end_trans);
}

// psqlODBC reports a failed allocation on the connection: the warning names that state, and the commit goes ahead.
TEST_F(DsqlTransactionAbortTest, Commit_ProbeCantBeAllocated_Commits) {
    dbc_.dsql_statement_failed_in_transaction = true;
    g_alloc_result = SQL_ERROR;
    g_state = "HY001";
    WarningSink sink;
    EXPECT_EQ(SQL_SUCCESS, EndTran(SQL_COMMIT));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_EQ(1, g_alloc_calls);
    EXPECT_TRUE(g_exec_texts.empty());
    EXPECT_TRUE(g_freed.empty());
    EXPECT_EQ((std::vector<SQLSMALLINT>{SQL_HANDLE_DBC}), g_diag_handle_types);
    EXPECT_EQ(BaseEndTrans({SQL_COMMIT}), g_end_trans);
    EXPECT_FALSE(dbc_.dsql_statement_failed_in_transaction);
    EXPECT_EQ(1, sink.Count());
    EXPECT_EQ(1, sink.Count("HY001"));
}

TEST_F(DsqlTransactionAbortTest, Rollback_ClearsTheFlagWithoutAProbe) {
    Abort(dbc_);
    EXPECT_EQ(SQL_SUCCESS, EndTran(SQL_ROLLBACK));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_EQ(0, g_alloc_calls);
    EXPECT_EQ(BaseEndTrans({SQL_ROLLBACK}), g_end_trans);
    EXPECT_FALSE(dbc_.dsql_statement_failed_in_transaction);
}

// Other databases keep psqlODBC's commit as it is, whatever the flag says.
TEST_F(DsqlTransactionAbortTest, Commit_OtherDatabase_NoProbe) {
    dbc_.is_dsql = false;
    Abort(dbc_);
    WarningSink sink;
    EXPECT_EQ(SQL_SUCCESS, EndTran(SQL_COMMIT));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_EQ(0, g_alloc_calls);
    EXPECT_TRUE(g_exec_texts.empty());
    EXPECT_EQ(BaseEndTrans({SQL_COMMIT}), g_end_trans);
    EXPECT_EQ(0, sink.Count());
}

// SQLEndTran on the environment (from the Windows driver manager, or an app without one) ends each connection's
// transaction: an aborted one is rolled back, the others are committed, and the environment's call returns the error,
// which the later calls don't replace.
TEST_F(DsqlTransactionAbortTest, EnvCommit_ChecksEachConnectionAndKeepsTheError) {
    Open(second_, second_base_dbc_stand_in_);
    Abort(dbc_);
    EXPECT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    const std::vector<std::pair<SQLHANDLE, SQLSMALLINT>> expected = {
        {static_cast<SQLHANDLE>(&base_dbc_stand_in_), SQL_ROLLBACK},
        {static_cast<SQLHANDLE>(&second_base_dbc_stand_in_), SQL_COMMIT},
        {static_cast<SQLHANDLE>(&base_env_stand_in_), SQL_COMMIT},
    };
    EXPECT_EQ(expected, g_end_trans);
    EXPECT_EQ(std::vector<std::string>{"SELECT 1"}, g_exec_texts);
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("25S03", dbc_.err->sqlstate);
    EXPECT_EQ(nullptr, second_.err);
    EXPECT_EQ(TRANSACTION_CLOSED, second_.transaction_status);
    EXPECT_FALSE(dbc_.dsql_statement_failed_in_transaction);
}

// The 25S03 the environment's call returns is the environment's record too, so SQLGetDiagRec on the environment reads
// it, not the base environment's record.
TEST_F(DsqlTransactionAbortTest, EnvCommit_TheKept25S03_IsTheEnvironmentsRecord) {
    Open(second_, second_base_dbc_stand_in_);
    Abort(dbc_);
    ASSERT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_ENV, &env_, 1, state, &native_error, message, MAX_MSG_LEN,
                                             &length, false));
    EXPECT_EQ("25S03", AppText(state));
    EXPECT_EQ(ROLLED_BACK, AppText(message));
    EXPECT_EQ(static_cast<SQLSMALLINT>(ROLLED_BACK.size()), length);
}

// The environment's 25S03 is that call's record only: the next call on the environment clears it. A successful one
// leaves no record (the base environment has none either).
TEST_F(DsqlTransactionAbortTest, EnvCommit_TheNextEnvironmentCall_ClearsThe25S03) {
    Abort(dbc_);
    ASSERT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    ASSERT_EQ(SQL_SUCCESS, EnvRecord());
    g_state.clear();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetEnvAttr(&env_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),
                                             0));
    EXPECT_EQ(SQL_NO_DATA, EnvRecord());
}

TEST_F(DsqlTransactionAbortTest, EnvCommit_AConnectionAllocatedNext_ClearsThe25S03) {
    Abort(dbc_);
    ASSERT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    ASSERT_EQ(SQL_SUCCESS, EnvRecord());
    g_state.clear();
    SQLHDBC connection = SQL_NULL_HDBC;
    ASSERT_EQ(SQL_SUCCESS, RDS_AllocDbc(&env_, &connection));
    EXPECT_EQ(SQL_NO_DATA, EnvRecord());
    EXPECT_EQ(SQL_SUCCESS, RDS_FreeConnect(connection));
}

// On any database: a connection's failed commit is the environment's result, although the base environment's own call
// after the loop succeeds. The other connections are still committed, and the environment gets no record of its own.
TEST_F(DsqlTransactionAbortTest, EnvCommit_AConnectionsCommitFails_IsKeptOverTheEnvironmentsSuccess) {
    dbc_.is_dsql = false;
    Open(second_, second_base_dbc_stand_in_);
    second_.is_dsql = false;
    g_end_tran_results[static_cast<SQLHANDLE>(&base_dbc_stand_in_)] = SQL_ERROR;
    EXPECT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    const std::vector<std::pair<SQLHANDLE, SQLSMALLINT>> expected = {
        {static_cast<SQLHANDLE>(&base_dbc_stand_in_), SQL_COMMIT},
        {static_cast<SQLHANDLE>(&second_base_dbc_stand_in_), SQL_COMMIT},
        {static_cast<SQLHANDLE>(&base_env_stand_in_), SQL_COMMIT},
    };
    EXPECT_EQ(expected, g_end_trans);
    EXPECT_EQ(TRANSACTION_OPEN, dbc_.transaction_status);
    EXPECT_EQ(TRANSACTION_CLOSED, second_.transaction_status);
    EXPECT_EQ(nullptr, env_.err);
}

// On any database: with every connection committed, the base environment's failure is the result.
TEST_F(DsqlTransactionAbortTest, EnvCommit_EveryConnectionCommits_ReturnsTheEnvironmentsFailure) {
    dbc_.is_dsql = false;
    Open(second_, second_base_dbc_stand_in_);
    second_.is_dsql = false;
    g_end_tran_results[static_cast<SQLHANDLE>(&base_env_stand_in_)] = SQL_ERROR;
    EXPECT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    EXPECT_EQ(3U, g_end_trans.size());
    EXPECT_EQ(TRANSACTION_CLOSED, dbc_.transaction_status);
    EXPECT_EQ(TRANSACTION_CLOSED, second_.transaction_status);
}

#if UNICODE && !defined(_WIN32)
// A base driver built with a 4-byte SQLWCHAR gets the check's text in 4-byte characters and writes its SQLSTATE in
// them.
TEST_F(DsqlTransactionAbortTest, Commit_FourByteBase_ReadsTheState) {
    g_four_byte_base = true;
    dbc_.plugin_service->GetOdbcHelper()->SetUse4BytesBaseDriver(true);
    Abort(dbc_);
    EXPECT_EQ(SQL_ERROR, EndTran(SQL_COMMIT));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("25S03", dbc_.err->sqlstate);
    EXPECT_EQ(std::vector<std::string>{"SELECT 1"}, g_exec_texts);
    EXPECT_EQ(BaseEndTrans({SQL_ROLLBACK}), g_end_trans);
}

// An app with a 4-byte SQLWCHAR (iODBC) reads the 25S03 record in 4-byte characters, the whole message.
TEST_F(DsqlTransactionAbortTest, Commit_FourByteApp_ReadsThe25S03Message) {
    env_.use_4_bytes_user_app = true;
    Abort(dbc_);
    ASSERT_EQ(SQL_ERROR, EndTran(SQL_COMMIT));

    std::vector<uint32_t> state(MAX_SQL_STATE_LEN, 0x5A5A5A5A);
    std::vector<uint32_t> message(MAX_MSG_LEN, 0x5A5A5A5A);
    SQLINTEGER native = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRecW(SQL_HANDLE_DBC, &dbc_, 1, reinterpret_cast<SQLWCHAR*>(state.data()), &native,
        reinterpret_cast<SQLWCHAR*>(message.data()), MAX_MSG_LEN, &length));
    EXPECT_EQ((std::vector<uint32_t>{'2', '5', 'S', '0', '3', 0}), state);
    EXPECT_EQ(static_cast<SQLSMALLINT>(ROLLED_BACK.size()), length);
    std::vector<uint32_t> expected(ROLLED_BACK.begin(), ROLLED_BACK.end());
    expected.push_back(0);
    message.resize(expected.size());
    EXPECT_EQ(expected, message);
}
#endif

// SQLParamData runs the statement of a data-at-execution SQLExecute or SQLExecDirect: its failure is noted too.
TEST_F(DsqlTransactionAbortTest, ParamData_ErrorWithAutocommitOff_SetsTheFlag) {
    const auto stmt = std::make_unique<STMT>();
    stmt->dbc = &dbc_;
    int base_stmt_stand_in = 0;
    OdbcHelper::SetWrappedStmt(stmt.get(), static_cast<SQLHSTMT>(&base_stmt_stand_in));
    g_param_data_result = SQL_ERROR;
    SQLPOINTER token = nullptr;
    EXPECT_EQ(SQL_ERROR, SQLParamData(stmt.get(), &token));
    EXPECT_TRUE(dbc_.dsql_statement_failed_in_transaction);
}

// Switching autocommit ends the transaction, so the note goes; setting the mode the connection has changes nothing,
// and the note stays. On an open connection the switch commits first through SQLEndTran, which ends the note (its
// check finds the transaction still open here). Between connects (the base connection kept, not connected) nothing is
// committed, and the setter ends the note itself, after the base call.
TEST_F(DsqlTransactionAbortTest, SetAutocommit_ASwitchClearsTheFlag) {
    for (const CONN_STATUS status : {CONN_CONNECTED, CONN_NOT_CONNECTED}) {
        SCOPED_TRACE(status == CONN_CONNECTED ? "connected" : "between connects");
        dbc_.conn_status = status;
        dbc_.auto_commit = false;
        dbc_.dsql_statement_failed_in_transaction = true;
        g_end_trans.clear();
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0));
        EXPECT_TRUE(dbc_.dsql_statement_failed_in_transaction);
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0));
        EXPECT_FALSE(dbc_.dsql_statement_failed_in_transaction);
        EXPECT_EQ(BaseEndTrans(status == CONN_CONNECTED ? std::vector<SQLSMALLINT>{SQL_COMMIT}
                                                        : std::vector<SQLSMALLINT>{}),
                  g_end_trans);
    }
}
