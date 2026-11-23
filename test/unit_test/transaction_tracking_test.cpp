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

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sqlext.h>

#include "../../driver/driver.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/default_plugin.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"
#include "../../driver/util/rds_strings.h"

namespace {
    // What SQLEndTran returns for a handle; SQL_SUCCESS for one not in it.
    std::map<SQLHANDLE, SQLRETURN> g_end_tran_results;
    // The handles SQLEndTran got, in order.
    std::vector<SQLHANDLE> g_end_tran_handles;

    SQLRETURN FakeEndTran(SQLSMALLINT /*handle_type*/, SQLHANDLE handle, SQLSMALLINT /*completion_type*/) {
        g_end_tran_handles.push_back(handle);
        const auto it = g_end_tran_results.find(handle);
        return it != g_end_tran_results.end() ? it->second : SQL_SUCCESS;
    }

    SQLRETURN FakeSetConnectAttr(SQLHDBC /*connection_handle*/, SQLINTEGER /*attribute*/, SQLPOINTER /*value*/,
                                 SQLINTEGER /*string_length*/) {
        return SQL_SUCCESS;
    }

    SQLRETURN g_statement_result = SQL_SUCCESS;

    SQLRETURN FakeExecDirect(SQLHSTMT /*statement_handle*/, SQLTCHAR* /*text*/, SQLINTEGER /*text_length*/) {
        return g_statement_result;
    }

    SQLRETURN FakeParamData(SQLHSTMT /*statement_handle*/, SQLPOINTER* /*value*/) {
        return g_statement_result;
    }

    class FakeBaseDriver : public RdsLibLoader {
    public:
        RdsPlatform::FuncHandle GetFunction(const std::string& function_name) override {
            if (function_name == RdsFuncNames::EndTran) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeEndTran);
            }
            if (function_name == RdsFuncNames::SetConnectAttr) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSetConnectAttr);
            }
            if (function_name == RdsFuncNames::ExecDirect) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeExecDirect);
            }
            if (function_name == RdsFuncNames::ParamData) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeParamData);
            }
            return nullptr;
        }
    };
}  // namespace

// A connection in autocommit mode over a fake base driver, in the environment's list.
class TransactionTrackingTest : public testing::Test {
protected:
    std::shared_ptr<FakeBaseDriver> base_driver_ = std::make_shared<FakeBaseDriver>();
    ENV env_;
    DBC dbc_{};
    DBC second_{};
    std::shared_ptr<DefaultPlugin> default_plugin_;
    STMT stmt_{};
    int base_env_ = 0;
    int base_dbc_ = 0;
    int second_base_dbc_ = 0;
    int base_stmt_ = 0;

    void SetUp() override {
        g_end_tran_results.clear();
        g_end_tran_handles.clear();
        g_statement_result = SQL_SUCCESS;
        env_.driver_lib_loader = base_driver_;
        env_.wrapped_env = static_cast<SQLHENV>(&base_env_);
        Connect(dbc_, base_dbc_);
        stmt_.dbc = &dbc_;
        stmt_.wrapped_stmt = static_cast<SQLHSTMT>(&base_stmt_);
    }

    // SQLExecDirect through the chain a connect to another database builds.
    SQLRETURN ExecDirect(const std::string& text) {
        if (!default_plugin_) {
            default_plugin_ = std::make_shared<DefaultPlugin>(&dbc_);
            dbc_.plugin_head = default_plugin_.get();
        }
#ifdef UNICODE
        const std::vector<uint16_t> utf16 = ConvertUTF8ToUTF16(text);
        std::vector<SQLTCHAR> statement(utf16.begin(), utf16.end());
#else
        std::vector<SQLTCHAR> statement(text.begin(), text.end());
        statement.push_back(0);
#endif
        return RDS_SQLExecDirect(&stmt_, statement.data(), SQL_NTS);
    }

    void Connect(DBC& dbc, int& base_dbc) {
        dbc.env = &env_;
        dbc.wrapped_dbc = static_cast<SQLHDBC>(&base_dbc);
        dbc.conn_status = CONN_CONNECTED;
        dbc.transaction_status = TRANSACTION_CLOSED;
        dbc.plugin_service = std::make_shared<PluginService>(
            base_driver_, &env_, std::map<std::string, std::string>{});
        env_.dbc_list.push_back(&dbc);
    }
};

// psqlODBC does nothing when autocommit is set on while it is already on, so a transaction opened with BEGIN stays open.
// Switching it on from off still ends the transaction.
TEST_F(TransactionTrackingTest, AutocommitSetOnAgain_KeepsABeginTransactionOpen) {
    dbc_.transaction_status = TRANSACTION_OPEN;  // as a BEGIN leaves it
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT,
                                                 reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_EQ(TRANSACTION_OPEN, dbc_.transaction_status);

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT,
                                                 reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT,
                                                 reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_EQ(TRANSACTION_CLOSED, dbc_.transaction_status);
}

// A connection's failed commit is the result, although the later connection and the base environment succeed. Every
// connection's commit is still sent.
TEST_F(TransactionTrackingTest, EnvCommit_AConnectionFails_ReturnsItsError) {
    Connect(second_, second_base_dbc_);
    g_end_tran_results[static_cast<SQLHANDLE>(&base_dbc_)] = SQL_ERROR;
    EXPECT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    const std::vector<SQLHANDLE> expected = {&base_dbc_, &second_base_dbc_, &base_env_};
    EXPECT_EQ(expected, g_end_tran_handles);
}

// An allocated connection that was never connected has no transaction; the connected ones are still committed.
TEST_F(TransactionTrackingTest, EnvCommit_SkipsAConnectionThatIsNotConnected) {
    second_.env = &env_;
    env_.dbc_list.push_front(&second_);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    const std::vector<SQLHANDLE> expected = {&base_dbc_, &base_env_};
    EXPECT_EQ(expected, g_end_tran_handles);
}

// With no connection ever connected there is no base environment and nothing to end.
TEST_F(TransactionTrackingTest, EnvCommit_NoConnectionConnected_ReturnsSuccess) {
    ENV env;
    second_.env = &env;
    env.dbc_list.push_back(&second_);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_ENV, &env, SQL_COMMIT));
}

// A statement call that fails or warns in a transaction is noted: with autocommit off, or after a BEGIN with it on.
// Not one in autocommit mode, and not one that succeeds.
TEST_F(TransactionTrackingTest, Execute_FailedStatementInATransaction_IsNoted) {
    struct Case {
        bool auto_commit;
        TRANSACTION_STATUS status;
        SQLRETURN result;
        bool noted;
    };
    for (const Case& c : {Case{false, TRANSACTION_OPEN, SQL_ERROR, true},
                          Case{false, TRANSACTION_OPEN, SQL_SUCCESS_WITH_INFO, true},
                          Case{true, TRANSACTION_OPEN, SQL_ERROR, true},
                          Case{true, TRANSACTION_CLOSED, SQL_ERROR, false},
                          Case{false, TRANSACTION_OPEN, SQL_SUCCESS, false}}) {
        SCOPED_TRACE(testing::Message() << "autocommit " << c.auto_commit << ", status " << static_cast<int>(c.status)
                                        << ", result " << c.result);
        dbc_.auto_commit = c.auto_commit;
        dbc_.transaction_status = c.status;
        dbc_.statement_failed_in_transaction = false;
        g_statement_result = c.result;
        EXPECT_EQ(c.result, ExecDirect("INSERT INTO t VALUES (1)"));
        EXPECT_EQ(c.noted, dbc_.statement_failed_in_transaction);
    }
}

// SQLParamData runs the statement of a data-at-execution call, so its failure is noted too.
TEST_F(TransactionTrackingTest, ParamData_FailedStatementInATransaction_IsNoted) {
    dbc_.auto_commit = false;
    dbc_.transaction_status = TRANSACTION_OPEN;
    g_statement_result = SQL_ERROR;
    SQLPOINTER token = nullptr;
    EXPECT_EQ(SQL_ERROR, SQLParamData(&stmt_, &token));
    EXPECT_TRUE(dbc_.statement_failed_in_transaction);
}

// What ends the transaction ends the note: SQLEndTran on the connection, whatever its result, or on the environment; a
// switch of autocommit; a ROLLBACK sent as text. Setting autocommit to the mode it is in ends nothing.
TEST_F(TransactionTrackingTest, TransactionEnd_ClearsTheNote) {
    dbc_.auto_commit = false;
    dbc_.statement_failed_in_transaction = true;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_DBC, &dbc_, SQL_COMMIT));
    EXPECT_FALSE(dbc_.statement_failed_in_transaction);

    dbc_.statement_failed_in_transaction = true;
    g_end_tran_results[static_cast<SQLHANDLE>(&base_dbc_)] = SQL_ERROR;
    EXPECT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_DBC, &dbc_, SQL_ROLLBACK));
    EXPECT_FALSE(dbc_.statement_failed_in_transaction);
    g_end_tran_results.clear();

    dbc_.statement_failed_in_transaction = true;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    EXPECT_FALSE(dbc_.statement_failed_in_transaction);

    dbc_.statement_failed_in_transaction = true;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT,
                                                 reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0));
    EXPECT_TRUE(dbc_.statement_failed_in_transaction);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT,
                                                 reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_FALSE(dbc_.statement_failed_in_transaction);

    dbc_.auto_commit = false;
    dbc_.transaction_status = TRANSACTION_OPEN;
    dbc_.statement_failed_in_transaction = true;
    EXPECT_EQ(SQL_SUCCESS, ExecDirect("ROLLBACK"));
    EXPECT_FALSE(dbc_.statement_failed_in_transaction);
}
