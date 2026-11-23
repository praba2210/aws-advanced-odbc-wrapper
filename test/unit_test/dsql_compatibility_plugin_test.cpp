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
#include <ng-log/logging.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../../driver/driver.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/default_plugin.h"
#include "../../driver/plugin/dsql/dsql_compatibility_plugin.h"
#include "../../driver/util/connection_string_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string AURORA_HOST = "database-test-name.cluster-XYZ.us-east-2.rds.amazonaws.com";

    // What the fake base driver answers.
    SQLRETURN g_connect_result = SQL_SUCCESS;  // SQLDriverConnect
    SQLRETURN g_alloc_stmt_result = SQL_SUCCESS;  // SQLAllocHandle(SQL_HANDLE_STMT)
    SQLRETURN g_end_tran_result = SQL_SUCCESS;
    SQLRETURN g_exec_result = SQL_SUCCESS;  // SQLExecDirect
    // The SQLSTATE of the base driver's record 1 on any handle; empty for none.
    std::string g_state;

    // What it got: the last SQLDriverConnect's string, the SQLEndTran, SQLSetConnectAttr and SQLExecDirect calls, in
    // order, and how many statements it freed.
    std::string g_conn_str;
    std::vector<std::string> g_calls;
    int g_freed = 0;
    int g_base_dbc_stand_in = 0;
    int g_base_stmt_stand_in = 0;

    // ASCII text in SQLTCHARs, to its NUL.
    std::string AsciiText(const SQLTCHAR* text) {
        std::string ascii;
        for (const SQLTCHAR* c = text; *c != 0; ++c) {
            ascii.push_back(static_cast<char>(*c));
        }
        return ascii;
    }

    std::string SetConnectAttrCall(SQLINTEGER attribute, SQLULEN value) {
        return "SQLSetConnectAttr(" + std::to_string(attribute) + ", " + std::to_string(value) + ")";
    }

    std::string EndTranCall(SQLSMALLINT completion_type) {
        return std::string("SQLEndTran(") + (completion_type == SQL_COMMIT ? "SQL_COMMIT" : "SQL_ROLLBACK") + ")";
    }

    std::string ExecDirectCall(const std::string& text) {
        return "SQLExecDirect(" + text + ")";
    }

    SQLRETURN FakeAllocHandle(SQLSMALLINT handle_type, SQLHANDLE /*input_handle*/, SQLHANDLE* output_handle) {
        if (handle_type == SQL_HANDLE_STMT && !SQL_SUCCEEDED(g_alloc_stmt_result)) {
            *output_handle = SQL_NULL_HANDLE;
            return g_alloc_stmt_result;
        }
        *output_handle = handle_type == SQL_HANDLE_DBC ? static_cast<SQLHANDLE>(&g_base_dbc_stand_in)
                                                       : static_cast<SQLHANDLE>(&g_base_stmt_stand_in);
        return SQL_SUCCESS;
    }

    SQLRETURN FakeDriverConnect(SQLHDBC, SQLHWND, SQLTCHAR* in_connection_string, SQLSMALLINT, SQLTCHAR*, SQLSMALLINT,
                                SQLSMALLINT*, SQLUSMALLINT) {
        g_conn_str = AsciiText(in_connection_string);
        return g_connect_result;
    }

    SQLRETURN FakeSetConnectAttr(SQLHDBC /*connection*/, SQLINTEGER attribute, SQLPOINTER value,
                                 SQLINTEGER /*length*/) {
        g_calls.push_back(SetConnectAttrCall(attribute, static_cast<SQLULEN>(reinterpret_cast<uintptr_t>(value))));
        return SQL_SUCCESS;
    }

    SQLRETURN FakeEndTran(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT completion_type) {
        g_calls.push_back(EndTranCall(completion_type));
        return g_end_tran_result;
    }

    SQLRETURN FakeExecDirect(SQLHSTMT /*statement*/, SQLTCHAR* text, SQLINTEGER /*length*/) {
        g_calls.push_back(ExecDirectCall(AsciiText(text)));
        return g_exec_result;
    }

    SQLRETURN FakeGetDiagRec(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT record, SQLTCHAR* state,
                             SQLINTEGER* native_error, SQLTCHAR* message, SQLSMALLINT buffer_length,
                             SQLSMALLINT* text_length) {
        if (record != 1 || g_state.empty()) {
            return SQL_NO_DATA;
        }
        if (state != nullptr) {
            for (size_t i = 0; i <= g_state.size(); ++i) {
                state[i] = static_cast<SQLTCHAR>(i < g_state.size() ? g_state[i] : 0);
            }
        }
        if (message != nullptr && buffer_length > 0) {
            message[0] = 0;
        }
        if (native_error != nullptr) {
            *native_error = 0;
        }
        if (text_length != nullptr) {
            *text_length = 0;
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeFreeHandle(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/) {
        ++g_freed;
        return SQL_SUCCESS;
    }

    // Answers the calls above, records every lookup, and returns nullptr for anything else, so a forwarded call fails
    // without reaching a real driver.
    class FakeBaseDriver : public RdsLibLoader {
    public:
        RdsPlatform::FuncHandle GetFunction(const std::string& function_name) override {
            requested_functions.push_back(function_name);
            const std::map<std::string_view, RdsPlatform::FuncHandle> functions = {
                {RdsFuncNames::AllocHandle, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeAllocHandle)},
                {RdsFuncNames::DriverConnect, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeDriverConnect)},
                {RdsFuncNames::SetConnectAttr, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSetConnectAttr)},
                {RdsFuncNames::EndTran, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeEndTran)},
                {RdsFuncNames::ExecDirect, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeExecDirect)},
                {RdsFuncNames::GetDiagRec, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetDiagRec)},
                {RdsFuncNames::FreeHandle, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeFreeHandle)},
            };
            const auto it = functions.find(function_name);
            return it != functions.end() ? it->second : nullptr;
        }

        bool Requested(std::string_view function_name) const {
            return std::ranges::find(requested_functions, function_name) != requested_functions.end();
        }

        std::vector<std::string> requested_functions;
    };

    // An integer attribute value, passed as ODBC passes it.
    SQLPOINTER AsPointer(SQLULEN value) {
        return reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(value));
    }

    // psqlODBC's attribute for server-side prepare (psqlODBC pgapifunc.h).
    constexpr SQLINTEGER PGOPT_SERVER_SIDE_PREPARE = 65540;
}  // namespace

// A connection handle with the chain RDS_InitializeConnection builds for its attributes, over a fake base driver.
class DsqlCompatibilityPluginTest : public testing::Test {
protected:
    std::shared_ptr<FakeBaseDriver> base_driver_ = std::make_shared<FakeBaseDriver>();
    ENV env_;
    DBC dbc_{};
    STMT stmt_{};
    std::shared_ptr<BasePlugin> chain_;
    int base_env_stand_in_ = 0;

    void SetUp() override {
        g_connect_result = SQL_SUCCESS;
        g_alloc_stmt_result = SQL_SUCCESS;
        g_end_tran_result = SQL_SUCCESS;
        g_exec_result = SQL_SUCCESS;
        g_state.clear();
        g_conn_str.clear();
        g_calls.clear();
        g_freed = 0;
        env_.driver_lib_loader = base_driver_;
        dbc_.env = &env_;
        stmt_.dbc = &dbc_;
    }

    void TearDown() override {
        // The fake handles point at test objects; never let them outlive the test.
        stmt_.wrapped_stmt = SQL_NULL_HSTMT;
        dbc_.wrapped_dbc = SQL_NULL_HDBC;
        env_.wrapped_env = SQL_NULL_HENV;
        dbc_.plugin_head = nullptr;
        dbc_.plugin_service = nullptr;
        env_.dbc_list.clear();
    }

    // The attributes, the plugin service and the chain (DefaultPlugin, and the compatibility plugin when it applies).
    void UseConnAttr(std::map<std::string, std::string> conn_attr) {
        dbc_.conn_attr = std::move(conn_attr);
        dbc_.plugin_service = std::make_shared<PluginService>(base_driver_, &env_, dbc_.conn_attr, "");
        chain_ = DsqlCompatibilityPlugin::AddToChain(&dbc_, std::make_shared<DefaultPlugin>(&dbc_));
        dbc_.plugin_head = chain_.get();
    }

    void UseServer(const std::string& server) {
        UseConnAttr({{KEY_SERVER, server}});
    }

    // The chain's connect, as RDS_SQLDriverConnect runs it.
    SQLRETURN ConnectChain() {
        env_.wrapped_env = static_cast<SQLHENV>(&base_env_stand_in_);
        return dbc_.plugin_head->Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    }

    // The handles and the status a connect leaves, so the wrapper treats the connection and the statement as open.
    void Connect() {
        dbc_.wrapped_dbc = static_cast<SQLHDBC>(&g_base_dbc_stand_in);
        dbc_.conn_status = CONN_CONNECTED;
        stmt_.wrapped_stmt = static_cast<SQLHSTMT>(&g_base_stmt_stand_in);
    }
};

// psqlODBC's DSN setup writes UseServerSidePrepare=1 and BASE_DSN copies it into conn_attr, so a value there isn't a
// choice.
TEST_F(DsqlCompatibilityPluginTest, BaseDriverOptions_OverrideUserServerSidePrepare) {
    std::map<std::string, std::string> long_form = {{"USESERVERSIDEPREPARE", "1"}};
    DsqlCompatibilityPlugin::ApplyBaseDriverOptions(long_form);
    EXPECT_EQ("0", long_form.at("USESERVERSIDEPREPARE"));

    std::map<std::string, std::string> short_form = {{"C8", "1"}};
    DsqlCompatibilityPlugin::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("C8"));
    EXPECT_EQ("0", short_form.at("USESERVERSIDEPREPARE"));
}

// CX=1c2305bab: 0x1c flags, bitmask 0x2305bab, which has UseServerSidePrepare (1 << 25) and UseDeclareFetch (1 << 14)
// on. psqlODBC reads the connection string from left to right, so the long keyword must come after CX.
TEST_F(DsqlCompatibilityPluginTest, BaseDriverOptions_LongKeyFollowsCxFlags) {
    std::map<std::string, std::string> conn_attr = {{"CX", "1c2305bab"}};
    DsqlCompatibilityPlugin::ApplyBaseDriverOptions(conn_attr);
    const std::string conn_str = ConnectionStringHelper::BuildMinimumConnectionString(conn_attr);
    const size_t cx = conn_str.find("CX=1c2305bab");
    const size_t server_side_prepare = conn_str.find("USESERVERSIDEPREPARE=0");
    ASSERT_NE(std::string::npos, cx);
    ASSERT_NE(std::string::npos, server_side_prepare);
    EXPECT_LT(cx, server_side_prepare);
}

// Another database keeps the chain it had: the plugin isn't added, so nothing of it runs.
TEST_F(DsqlCompatibilityPluginTest, AddToChain_OtherDatabase_ReturnsTheNextPlugin) {
    UseServer(AURORA_HOST);
    const std::shared_ptr<BasePlugin> next = std::make_shared<DefaultPlugin>(&dbc_);
    EXPECT_EQ(next, DsqlCompatibilityPlugin::AddToChain(&dbc_, next));
}

TEST_F(DsqlCompatibilityPluginTest, Connect_Dsql_BaseDriverGetsTheForcedOptions) {
    UseServer(DSQL_HOST);
    EXPECT_EQ(SQL_SUCCESS, ConnectChain());
    for (const char* option : {"PROTOCOL=7.4-0", "IGNORETIMEOUT=1", "USEDECLAREFETCH=0", "USESERVERSIDEPREPARE=0",
                               "UPDATABLECURSORS=0"}) {
        EXPECT_NE(std::string::npos, g_conn_str.find(option)) << option << " in " << g_conn_str;
    }
}

namespace {
    // The 01S02 text for an isolation level set before connect.
    std::string IsolationChanged(const std::string& level) {
        return "SQL_ATTR_TXN_ISOLATION=" + level + ", set before connect, was changed to SQL_TXN_REPEATABLE_READ: "
               "Aurora DSQL runs every transaction at REPEATABLE READ.";
    }
}  // namespace

// DefaultPlugin::Connect sets the attributes set before connect again after psqlODBC connects: server-side prepare
// would undo USESERVERSIDEPREPARE=0, and an isolation level would make psqlODBC send SET SESSION CHARACTERISTICS. The
// connect reports the level it didn't set.
TEST_F(DsqlCompatibilityPluginTest, Connect_Dsql_DropsAttributesSetBeforeConnect) {
    UseServer(DSQL_HOST);
    dbc_.attr_map.insert_or_assign(PGOPT_SERVER_SIDE_PREPARE, std::make_pair(AsPointer(1), 0));
    dbc_.attr_map.insert_or_assign(SQL_ATTR_TXN_ISOLATION, std::make_pair(AsPointer(SQL_TXN_SERIALIZABLE), 0));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, ConnectChain());
    EXPECT_TRUE(g_calls.empty());
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);
    EXPECT_EQ(IsolationChanged("SQL_TXN_SERIALIZABLE"), dbc_.err->error_msg);
}

// Set on a live connection, the attribute turns server-side prepare back on for new statements, and the wrapper sets
// it again after every reconnect.
TEST_F(DsqlCompatibilityPluginTest, SetPgoptServerSidePrepare_Dsql_StaysOff) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(1), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);
    EXPECT_EQ("SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE changed to 0: freeing a prepared statement inside a transaction can "
              "silently roll the transaction back on Aurora DSQL", dbc_.err->error_msg);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
}

namespace {
    // The 01S02 text for an SSLmode replaced with verify-full, worded as the Aurora DSQL auth plugin's.
    std::string SslModeChanged(const std::string& mode) {
        return "SSLmode=" + mode + " was changed to verify-full: Aurora DSQL requires TLS. Set SSLmode to verify-full, "
               "verify-ca or require.";
    }
}  // namespace

// An auth type other than DSQL (here a token in PWD) with a mode psqlODBC runs as disable, which Aurora DSQL refuses:
// the base driver gets verify-full and postgres, Aurora DSQL's only database, and the connect reports the change.
TEST_F(DsqlCompatibilityPluginTest, Connect_OtherAuthDisable_GetsVerifyFullAndPostgresWith01S02) {
    UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}, {"SSLMODE", "disable"}});
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, ConnectChain());
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);
    EXPECT_EQ(SslModeChanged("disable"), dbc_.err->error_msg);
    EXPECT_NE(std::string::npos, g_conn_str.find("SSLMODE=verify-full")) << g_conn_str;
    EXPECT_NE(std::string::npos, g_conn_str.find("DATABASE=postgres")) << g_conn_str;
}

namespace {
    // Connects through the next plugin again when that fails, as Secrets Manager does after a cached secret fails.
    class RetryingPlugin : public BasePlugin {
    public:
        using BasePlugin::BasePlugin;

        SQLRETURN Connect(SQLHDBC dbc, SQLHWND window, SQLTCHAR* out, SQLSMALLINT out_length,
                          SQLSMALLINT* out_length_ptr, SQLUSMALLINT completion) override {
            if (const SQLRETURN ret = ConnectNext(dbc, window, out, out_length, out_length_ptr, completion);
                SQL_SUCCEEDED(ret)) {
                return ret;
            }
            g_connect_result = SQL_SUCCESS;
            return ConnectNext(dbc, window, out, out_length, out_length_ptr, completion);
        }
    };
}  // namespace

// A mode psqlODBC runs is kept as set, and the connect still gets postgres.
TEST_F(DsqlCompatibilityPluginTest, Connect_OtherAuthKeptMode_GetsPostgres) {
    UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}, {"SSLMODE", "require"}});
    EXPECT_EQ(SQL_SUCCESS, ConnectChain());
    EXPECT_NE(std::string::npos, g_conn_str.find("DATABASE=postgres")) << g_conn_str;
}

// A retry of the base connect within one connect starts from the application's settings again, so the app still gets
// the 01S02, for both changes in its one record.
TEST_F(DsqlCompatibilityPluginTest, Connect_RetryInTheSameConnect_KeepsThe01S02) {
    UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_SECRETS}, {"SSLMODE", "disable"}});
    dbc_.attr_map.insert_or_assign(SQL_ATTR_TXN_ISOLATION, std::make_pair(AsPointer(SQL_TXN_READ_COMMITTED), 0));
    chain_ = std::make_shared<RetryingPlugin>(&dbc_, chain_);
    dbc_.plugin_head = chain_.get();
    g_connect_result = SQL_ERROR;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, ConnectChain());
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ(SslModeChanged("disable") + " " + IsolationChanged("SQL_TXN_READ_COMMITTED"), dbc_.err->error_msg);
}

// A statement's timeout, and the connection's for later statements (ODBC 2's SQLSetConnectOption), get 01S02 and
// don't reach psqlODBC: Aurora DSQL rejects SET statement_timeout.
TEST_F(DsqlCompatibilityPluginTest, SetQueryTimeout_Dsql_ReturnsOptionValueChanged) {
    UseServer(DSQL_HOST);
    Connect();
    const std::string message = "SQL_ATTR_QUERY_TIMEOUT changed to 0: Aurora DSQL doesn't support query timeouts (it "
                                "limits each transaction to 5 minutes)";
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(30), 0));
    ASSERT_NE(nullptr, stmt_.err);
    EXPECT_EQ("01S02", stmt_.err->sqlstate);
    EXPECT_EQ(message, stmt_.err->error_msg);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetStmtAttr));
    EXPECT_FALSE(stmt_.attr_map.contains(SQL_ATTR_QUERY_TIMEOUT));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(30), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ(message, dbc_.err->error_msg);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));

    // No timeout is what psqlODBC has: it passes through.
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(0), 0));
    EXPECT_EQ(std::vector<std::string>{SetConnectAttrCall(SQL_ATTR_QUERY_TIMEOUT, 0)}, g_calls);
    RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(0), 0);
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::SetStmtAttr));
}

// Aurora DSQL runs every transaction at REPEATABLE READ. ODBC lets a driver substitute a higher level, never a lower
// one, so READ COMMITTED gets 01S02 and SERIALIZABLE HY024. psqlODBC would send SET SESSION CHARACTERISTICS.
TEST_F(DsqlCompatibilityPluginTest, SetTxnIsolation_Dsql_IsAnsweredLocally) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(SQL_TXN_REPEATABLE_READ), 0));
    EXPECT_EQ(nullptr, dbc_.err);

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(SQL_TXN_READ_COMMITTED), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);

    EXPECT_EQ(SQL_ERROR, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(SQL_TXN_SERIALIZABLE), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("HY024", dbc_.err->sqlstate);
    EXPECT_EQ("SQL_TXN_SERIALIZABLE isn't supported: Aurora DSQL runs every transaction at REPEATABLE READ "
              "(snapshot isolation). Use SQL_TXN_REPEATABLE_READ, and read the rows a decision depends on with "
              "SELECT ... FOR UPDATE.", dbc_.err->error_msg);

    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
    EXPECT_FALSE(dbc_.attr_map.contains(SQL_ATTR_TXN_ISOLATION));
}

// psqlODBC reports PostgreSQL's four levels.
TEST_F(DsqlCompatibilityPluginTest, GetInfo_Dsql_ReportsOnlyRepeatableRead) {
    UseServer(DSQL_HOST);
    Connect();
    SQLUINTEGER value = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_TXN_ISOLATION_OPTION, &value, static_cast<SQLSMALLINT>(sizeof(value)), &length));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_TXN_REPEATABLE_READ), value);
    EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(SQLUINTEGER)), length);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::GetInfo));
}

namespace {
    // The 25S03 message of an SQLEndTran(SQL_COMMIT) after a failed statement.
    const std::string ROLLED_BACK =
        "The transaction was rolled back, not committed: a statement in it failed, and Aurora DSQL ends a transaction "
        "at its first error (it has no savepoints). Roll back after an error, and run the whole transaction again.";

    // Collects the WARNING lines while it exists.
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

        // How many lines contain the text.
        std::ptrdiff_t Count(const std::string& text) {
            const std::lock_guard<std::mutex> lock(mutex_);
            return std::ranges::count_if(
                lines_, [&text](const std::string& line) { return line.find(text) != std::string::npos; });
        }

    private:
        std::mutex mutex_;
        std::vector<std::string> lines_;
    };
}  // namespace

// A transaction on Aurora DSQL in which a statement failed: Aurora DSQL has ended it, and every statement fails with
// 25P02.
class DsqlTransactionTest : public DsqlCompatibilityPluginTest {
protected:
    void SetUp() override {
        DsqlCompatibilityPluginTest::SetUp();
        UseServer(DSQL_HOST);
        Connect();
        dbc_.auto_commit = false;
        dbc_.transaction_status = TRANSACTION_OPEN;
    }

    void Abort() {
        dbc_.statement_failed_in_transaction = true;
        g_exec_result = SQL_ERROR;
        g_state = "25P02";
    }
};

// Switching autocommit inside a transaction commits it, and psqlODBC drops that COMMIT's result, while Aurora DSQL
// reports write conflicts at COMMIT: the wrapper commits through SQLEndTran first.
TEST_F(DsqlTransactionTest, SetAutocommitOn_Dsql_CommitsFirst) {
    // Setting the mode it is in commits nothing.
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_OFF), 0));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    const std::vector<std::string> expected = {
        SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF),
        EndTranCall(SQL_COMMIT),
        SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_ON),
    };
    EXPECT_EQ(expected, g_calls);
    EXPECT_TRUE(dbc_.auto_commit);
}

// A conflict, for example: the app gets the COMMIT's error, read from the base driver's record, and autocommit stays
// off.
TEST_F(DsqlTransactionTest, SetAutocommitOn_Dsql_CommitFails_ReturnsItsErrorAndStaysOff) {
    dbc_.attr_map.insert_or_assign(SQL_ATTR_AUTOCOMMIT, std::make_pair(AsPointer(SQL_AUTOCOMMIT_OFF), 0));
    const auto attr_map = dbc_.attr_map;
    g_end_tran_result = SQL_ERROR;
    g_state = "40001";
    EXPECT_EQ(SQL_ERROR, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_EQ(std::vector<std::string>{EndTranCall(SQL_COMMIT)}, g_calls);
    EXPECT_FALSE(dbc_.auto_commit);
    EXPECT_EQ(attr_map, dbc_.attr_map);
    SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state, &native_error, message, MAX_MSG_LEN,
                                             &length, false));
    EXPECT_EQ("40001", AsciiText(state));
}

// The commit before the switch is the chain's: it rolls the ended transaction back and returns 25S03, and autocommit
// stays off.
TEST_F(DsqlTransactionTest, SetAutocommitOn_Dsql_AfterAFailedStatement_Returns25S03) {
    Abort();
    EXPECT_EQ(SQL_ERROR, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("25S03", dbc_.err->sqlstate);
    const std::vector<std::string> expected = {ExecDirectCall("SELECT 1"), EndTranCall(SQL_ROLLBACK)};
    EXPECT_EQ(expected, g_calls);
    EXPECT_FALSE(dbc_.auto_commit);
    EXPECT_FALSE(dbc_.statement_failed_in_transaction);
}

TEST_F(DsqlTransactionTest, Commit_AfterAFailedStatement_Aborted_RollsBackAndReturns25S03) {
    Abort();
    WarningSink sink;
    EXPECT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_DBC, &dbc_, SQL_COMMIT));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("25S03", dbc_.err->sqlstate);
    EXPECT_EQ(ROLLED_BACK, dbc_.err->error_msg);
    const std::vector<std::string> expected = {ExecDirectCall("SELECT 1"), EndTranCall(SQL_ROLLBACK)};
    EXPECT_EQ(expected, g_calls);
    EXPECT_EQ(1, g_freed);
    EXPECT_FALSE(dbc_.statement_failed_in_transaction);
    EXPECT_EQ(TRANSACTION_CLOSED, dbc_.transaction_status);
    EXPECT_EQ(1, sink.Count("[DSQL] " + ROLLED_BACK + " (the check before the commit got 25P02)"));
}

// The transaction is still open: the commit goes ahead, after the check.
TEST_F(DsqlTransactionTest, Commit_AfterAFailedStatement_StillOpen_Commits) {
    dbc_.statement_failed_in_transaction = true;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_DBC, &dbc_, SQL_COMMIT));
    EXPECT_EQ(nullptr, dbc_.err);
    const std::vector<std::string> expected = {ExecDirectCall("SELECT 1"), EndTranCall(SQL_COMMIT)};
    EXPECT_EQ(expected, g_calls);
    EXPECT_FALSE(dbc_.statement_failed_in_transaction);
}

// Only 25P02 shows that the transaction ended: a check that fails some other way lets the commit go ahead, as psqlODBC
// would make it, with a warning that names the state.
TEST_F(DsqlTransactionTest, Commit_ProbeFailsWithoutAbortedState_CommitsAndLogs) {
    dbc_.statement_failed_in_transaction = true;
    g_exec_result = SQL_ERROR;
    g_state = "HY010";
    WarningSink sink;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_DBC, &dbc_, SQL_COMMIT));
    EXPECT_EQ(nullptr, dbc_.err);
    const std::vector<std::string> expected = {ExecDirectCall("SELECT 1"), EndTranCall(SQL_COMMIT)};
    EXPECT_EQ(expected, g_calls);
    EXPECT_EQ(1, sink.Count("got HY010, not 25P02"));
}

// A rollback ends the transaction either way, so it needs no check.
TEST_F(DsqlTransactionTest, Rollback_AfterAFailedStatement_NoProbe) {
    Abort();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_DBC, &dbc_, SQL_ROLLBACK));
    EXPECT_EQ(std::vector<std::string>{EndTranCall(SQL_ROLLBACK)}, g_calls);
}

// Without a statement the check can't run: the commit goes ahead, with a warning.
TEST_F(DsqlTransactionTest, Commit_ProbeCantBeAllocated_CommitsAndLogs) {
    Abort();
    g_alloc_stmt_result = SQL_ERROR;
    WarningSink sink;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_DBC, &dbc_, SQL_COMMIT));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_EQ(std::vector<std::string>{EndTranCall(SQL_COMMIT)}, g_calls);
    EXPECT_EQ(0, g_freed);
    EXPECT_EQ(1, sink.Count("couldn't allocate a statement: committing"));
}

TEST_F(DsqlTransactionTest, Commit_NoFailedStatement_NoProbe) {
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLEndTran(SQL_HANDLE_DBC, &dbc_, SQL_COMMIT));
    EXPECT_EQ(std::vector<std::string>{EndTranCall(SQL_COMMIT)}, g_calls);
}

// SQLEndTran on the environment (from the Windows driver manager, or an app without one) goes through each
// connection's chain, and the 25S03 it returns is the environment's record too.
TEST_F(DsqlTransactionTest, EnvCommit_TheKept25S03_IsTheEnvironmentsRecord) {
    env_.wrapped_env = static_cast<SQLHENV>(&base_env_stand_in_);
    env_.dbc_list.push_back(&dbc_);
    Abort();
    EXPECT_EQ(SQL_ERROR, RDS_SQLEndTran(SQL_HANDLE_ENV, &env_, SQL_COMMIT));
    SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_ENV, &env_, 1, state, &native_error, message, MAX_MSG_LEN,
                                             &length, false));
    EXPECT_EQ("25S03", AsciiText(state));
    EXPECT_EQ(ROLLED_BACK, AsciiText(message));
}
