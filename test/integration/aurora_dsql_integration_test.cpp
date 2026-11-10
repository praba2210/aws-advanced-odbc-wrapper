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

// Integration tests against a live Aurora DSQL cluster. Every case skips unless TEST_DSQL_ENDPOINT is set; the slow
// ones also need TEST_DSQL_SLOW=1. Each case that needs a table creates its own (drf_it_<pid>_<n>) and drops it.
// See aurora_dsql_test_helper.h for the environment variables.

#include <gtest/gtest.h>

#include "../../driver/util/windows_headers.h"

#include <sql.h>
#include <sqlext.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../common/base_connection_test.h"
#include "aurora_dsql_test_helper.h"

using dsql_it::DsqlTarget;
using dsql_it::Session;
using dsql_it::TempTable;

namespace {
    bool Contains(const std::string& text, const std::string& part) {
        return text.find(part) != std::string::npos;
    }

    SQLPOINTER IntPtr(uintptr_t value) {
        return reinterpret_cast<SQLPOINTER>(value);
    }

    // Prepares and executes an INSERT with two bound integers, then frees the statement with SQL_DROP.
    SQLRETURN InsertPreparedThenDrop(SQLHDBC dbc, const std::string& table, SQLINTEGER id, SQLINTEGER value) {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt))) {
            return SQL_ERROR;
        }
        std::vector<SQLTCHAR> sql = dsql_it::ToT("INSERT INTO " + table + " (id, v) VALUES (?, ?)");
        SQLRETURN ret = SQLPrepare(stmt, sql.data(), SQL_NTS);
        if (SQL_SUCCEEDED(ret)) {
            SQLLEN indicator = 0;
            SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &id, 0, &indicator);
            SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, 0, &indicator);
            ret = SQLExecute(stmt);
        }
        SQLFreeStmt(stmt, SQL_DROP);
        return ret;
    }

    struct IncrementResult {
        int committed = 0;
        int failed = 0;
        int other_than_40001 = 0;
        bool connect_failed = false;
    };

    // `threads` connections, each running `iterations` autocommit increments of the row with id 1.
    std::vector<IncrementResult> RunIncrements(SQLHENV env, const std::string& conn_str, const std::string& table,
                                               int threads, int iterations) {
        std::vector<IncrementResult> results(threads);
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; t++) {
            workers.emplace_back([&, t]() {
                Session session(env);
                if (!SQL_SUCCEEDED(session.Connect(conn_str))) {
                    results[t].connect_failed = true;
                    return;
                }
                for (int i = 0; i < iterations; i++) {
                    std::string state;
                    if (SQL_SUCCEEDED(session.Exec("UPDATE " + table + " SET v = v + 1 WHERE id = 1", &state))) {
                        results[t].committed++;
                    } else {
                        results[t].failed++;
                        if (state != "40001") {
                            results[t].other_than_40001++;
                        }
                    }
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        return results;
    }
}  // namespace

class AuroraDsqlIntegrationTest : public BaseConnectionTest {
protected:
    const DsqlTarget& target = DsqlTarget::Get();

    void SetUp() override {
        // Before BaseConnectionTest::SetUp: no sleep, nothing allocated.
        if (!target.Enabled()) {
            GTEST_SKIP() << "TEST_DSQL_ENDPOINT is not set";
        }
        BaseConnectionTest::SetUp();
    }

    void TearDown() override {
        if (!target.Enabled()) {
            return;
        }
        BaseConnectionTest::TearDown();
    }

    // The one-row table most cases use: (1, 0, 'x').
    static bool SeedRow(const TempTable& table) {
        return SQL_SUCCEEDED(dsql_it::ExecOnFreshConnection("INSERT INTO " + table.Name() + " VALUES (1, 0, 'x')"));
    }
};

// 1
TEST_F(AuroraDsqlIntegrationTest, ConnectEndpoint_DefaultsToAdmin) {
    Session session(env);
    std::string out;
    ASSERT_EQ(SQL_SUCCESS, session.Connect(target.ConnStr(), &out)) << session.Failure();

    EXPECT_EQ("admin", session.Fetch("SELECT current_user").value_or("<failed>"));
    EXPECT_EQ(0u, session.Fetch("SHOW application_name").value_or("<failed>").rfind("aurora-dsql-odbc/", 0));
    EXPECT_FALSE(Contains(out, "PWD=")) << "the output connection string holds a password";
    EXPECT_FALSE(Contains(out, "X-Amz-")) << "the output connection string holds a token";
}

// 2
TEST_F(AuroraDsqlIntegrationTest, ConnectClusterId_ExpandsWithTheRegion) {
    if (target.cluster_id.empty() || target.region.empty()) {
        GTEST_SKIP() << "TEST_DSQL_CLUSTER_ID and TEST_DSQL_REGION are not set";
    }
    {
        Session session(env);
        ASSERT_TRUE(SQL_SUCCEEDED(session.Connect(target.ConnStr({{"SERVER", target.cluster_id},
                                                                  {"REGION", target.region}}))))
            << session.Failure();
        EXPECT_EQ("admin", session.Fetch("SELECT current_user").value_or("<failed>"));
    }

    // CI exports a region, so remove every source of one the wrapper reads.
    const std::string empty_config = (std::filesystem::temp_directory_path() /
        ("drf_it_empty_aws_config_" + std::to_string(dsql_it::ProcessId()))).string();
    std::ofstream(empty_config).close();
    {
        const dsql_it::ScopedEnv no_region("AWS_REGION", std::nullopt);
        const dsql_it::ScopedEnv no_default_region("AWS_DEFAULT_REGION", std::nullopt);
        const dsql_it::ScopedEnv empty_config_file("AWS_CONFIG_FILE", empty_config);
        Session session(env);
        EXPECT_EQ(SQL_ERROR, session.Connect(target.ConnStr({{"SERVER", target.cluster_id}})));
        std::string message;
        EXPECT_EQ("08001", session.State(&message));
        EXPECT_TRUE(Contains(message, "Region is required when host is a cluster ID")) << message;
    }
    std::remove(empty_config.c_str());
}

// 3
TEST_F(AuroraDsqlIntegrationTest, SslModeDisable_Returns01S02ThroughTheDriverManager) {
    Session session(env);
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, session.Connect(target.ConnStr({{"SSLMODE", "disable"}})))
        << session.Failure();
    EXPECT_EQ("01S02", dsql_it::DiagState(session.Dbc(), SQL_HANDLE_DBC));
    EXPECT_EQ("01S02", dsql_it::DiagFieldState(session.Dbc(), SQL_HANDLE_DBC));
    EXPECT_EQ("1", session.Fetch("SELECT 1").value_or("<failed>"));
}

// 4: the guard for a prepared statement freed inside a transaction (the point of the distro psqlODBC 16 leg).
TEST_F(AuroraDsqlIntegrationTest, PreparedStatementFreedInATransaction_CommitKeepsBothRows) {
    const TempTable table;
    ASSERT_TRUE(table.Ok());

    Session session(env);
    ASSERT_EQ(SQL_SUCCESS, session.Connect(target.ConnStr())) << session.Failure();
    ASSERT_TRUE(SQL_SUCCEEDED(session.SetAutocommit(false)));
    ASSERT_TRUE(SQL_SUCCEEDED(InsertPreparedThenDrop(session.Dbc(), table.Name(), 1, 10)));
    ASSERT_TRUE(SQL_SUCCEEDED(InsertPreparedThenDrop(session.Dbc(), table.Name(), 2, 20)));
    EXPECT_EQ(SQL_SUCCESS, session.EndTran(SQL_COMMIT)) << session.Failure();

    EXPECT_EQ("2", dsql_it::ReadBack("SELECT count(*) FROM " + table.Name()).value_or("<failed>"));
}

// 5
TEST_F(AuroraDsqlIntegrationTest, QueryTimeoutAndIsolation_AnsweredByTheWrapper) {
    Session session(env);
    ASSERT_EQ(SQL_SUCCESS, session.Connect(target.ConnStr())) << session.Failure();

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, session.Dbc(), &stmt));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLSetStmtAttr(stmt, SQL_ATTR_QUERY_TIMEOUT, IntPtr(5), 0));
    EXPECT_EQ("01S02", dsql_it::DiagState(stmt, SQL_HANDLE_STMT));
    SQLULEN timeout = 99;
    EXPECT_TRUE(SQL_SUCCEEDED(SQLGetStmtAttr(stmt, SQL_ATTR_QUERY_TIMEOUT, &timeout, 0, nullptr)));
    EXPECT_EQ(0u, timeout);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(session.Dbc(), SQL_ATTR_TXN_ISOLATION, IntPtr(SQL_TXN_SERIALIZABLE), 0));
    EXPECT_EQ("HY024", session.State());
    EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(session.Dbc(), SQL_ATTR_TXN_ISOLATION, IntPtr(SQL_TXN_REPEATABLE_READ), 0));

    SQLUINTEGER options = 0;
    EXPECT_TRUE(SQL_SUCCEEDED(SQLGetInfo(session.Dbc(), SQL_TXN_ISOLATION_OPTION, &options, 0, nullptr)));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_TXN_REPEATABLE_READ), options);

    constexpr SQLINTEGER PGOPT_SERVER_SIDE_PREPARE = 65540;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLSetConnectAttr(session.Dbc(), PGOPT_SERVER_SIDE_PREPARE, IntPtr(1), 0));
    EXPECT_EQ("01S02", session.State());
}

// 6
TEST_F(AuroraDsqlIntegrationTest, OccRetry_ConcurrentIncrementsAllCommit) {
    constexpr int THREADS = 3;
    constexpr int ITERATIONS = 10;
    const TempTable table;
    ASSERT_TRUE(table.Ok());
    ASSERT_TRUE(SeedRow(table));

    // With retry: no error, and every increment counted.
    const auto with_retry = RunIncrements(env, target.ConnStr({{"ENABLE_OCC_RETRY", "1"}, {"OCC_MAX_RETRIES", "20"}}),
                                          table.Name(), THREADS, ITERATIONS);
    for (const auto& result : with_retry) {
        EXPECT_FALSE(result.connect_failed);
        EXPECT_EQ(0, result.failed);
        EXPECT_EQ(ITERATIONS, result.committed);
    }
    EXPECT_EQ(std::to_string(THREADS * ITERATIONS),
              dsql_it::ReadBack("SELECT v FROM " + table.Name() + " WHERE id = 1").value_or("<failed>"));

    // Without retry: the conflicts reach the application, all as 40001.
    const auto without_retry = RunIncrements(env, target.ConnStr(), table.Name(), THREADS, ITERATIONS);
    int failed = 0;
    for (const auto& result : without_retry) {
        EXPECT_FALSE(result.connect_failed);
        EXPECT_EQ(0, result.other_than_40001);
        failed += result.failed;
    }
    EXPECT_GE(failed, 1) << "no 40001 in " << THREADS * ITERATIONS << " concurrent updates";
}

// 7
TEST_F(AuroraDsqlIntegrationTest, OccRetry_NotRetriedInATransaction) {
    const TempTable table;
    ASSERT_TRUE(table.Ok());
    ASSERT_TRUE(SeedRow(table));

    const std::string conn_str = target.ConnStr({{"ENABLE_OCC_RETRY", "1"}});
    Session first(env);
    Session second(env);
    ASSERT_EQ(SQL_SUCCESS, first.Connect(conn_str)) << first.Failure();
    ASSERT_EQ(SQL_SUCCESS, second.Connect(conn_str)) << second.Failure();
    ASSERT_TRUE(SQL_SUCCEEDED(first.SetAutocommit(false)));
    ASSERT_TRUE(SQL_SUCCEEDED(second.SetAutocommit(false)));

    ASSERT_TRUE(SQL_SUCCEEDED(first.Exec("UPDATE " + table.Name() + " SET v = 100 WHERE id = 1")));
    ASSERT_TRUE(SQL_SUCCEEDED(second.Exec("UPDATE " + table.Name() + " SET v = 200 WHERE id = 1")));
    EXPECT_EQ(SQL_SUCCESS, first.EndTran(SQL_COMMIT)) << first.Failure();
    EXPECT_EQ(SQL_ERROR, second.EndTran(SQL_COMMIT));
    EXPECT_EQ("40001", second.State());
    second.EndTran(SQL_ROLLBACK);

    EXPECT_EQ("100", dsql_it::ReadBack("SELECT v FROM " + table.Name() + " WHERE id = 1").value_or("<failed>"));
}

// 8: the wait between retries ends at SQLCancel. A connection whose catalog another session made stale fails its
// first UPDATE with OC001, and the retry plugin then waits; OC001 doesn't always happen, so up to 6 trials.
TEST_F(AuroraDsqlIntegrationTest, OccRetry_CancelDuringTheWait_ReturnsHY008) {
    const TempTable table;
    ASSERT_TRUE(table.Ok());
    ASSERT_TRUE(SeedRow(table));

    const std::string conn_str = target.ConnStr({{"ENABLE_OCC_RETRY", "1"}, {"OCC_MAX_RETRIES", "1"},
                                                 {"OCC_BASE_DELAY_MS", "8000"}, {"OCC_MAX_DELAY_MS", "8000"},
                                                 {"OCC_JITTER_FACTOR", "0"}});
    int applied = 0;  // updates that went through without a conflict
    bool conflicted = false;
    for (int trial = 1; trial <= 6 && !conflicted; trial++) {
        Session stale(env);
        ASSERT_EQ(SQL_SUCCESS, stale.Connect(conn_str)) << stale.Failure();
        ASSERT_TRUE(stale.Fetch("SELECT count(*) FROM " + table.Name()).has_value());  // loads the catalog
        ASSERT_TRUE(SQL_SUCCEEDED(dsql_it::ExecOnFreshConnection(
            "ALTER TABLE " + table.Name() + " ADD COLUMN c_" + std::to_string(trial) + " INT")));

        SQLHSTMT stmt = SQL_NULL_HSTMT;
        ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, stale.Dbc(), &stmt));
        std::thread canceller([stmt]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            SQLCancel(stmt);
        });
        std::vector<SQLTCHAR> sql = dsql_it::ToT("UPDATE " + table.Name() + " SET v = v + 1 WHERE id = 1");
        const auto started = std::chrono::steady_clock::now();
        const SQLRETURN ret = SQLExecDirect(stmt, sql.data(), SQL_NTS);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        canceller.join();
        const std::string state = ret == SQL_SUCCESS ? "" : dsql_it::DiagState(stmt, SQL_HANDLE_STMT);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);

        if (seconds < 1.0) {  // no OC001: the update went through at once
            ASSERT_TRUE(SQL_SUCCEEDED(ret)) << "SQLSTATE " << state;
            applied++;
            continue;
        }
        conflicted = true;
        EXPECT_EQ(SQL_ERROR, ret);
        EXPECT_EQ("HY008", state);
        EXPECT_LT(seconds, 4.0) << "the canceled wait took " << seconds << " s";
    }
    if (!conflicted) {
        GTEST_SKIP() << "no OC001 conflict in 6 trials";
    }
    EXPECT_EQ(std::to_string(applied),
              dsql_it::ReadBack("SELECT v FROM " + table.Name() + " WHERE id = 1").value_or("<failed>"));
}

// 9
TEST_F(AuroraDsqlIntegrationTest, RejectedSettings_FailBeforeTheDriverLoads) {
    // A base driver path that doesn't exist shows the check runs before the base driver loads.
    const std::string no_driver = "/nonexistent/psqlodbc.so";
    {
        Session session(env);
        EXPECT_EQ(SQL_ERROR, session.Connect(target.ConnStr({{"ENABLE_CLUSTER_FAILOVER", "1"},
                                                              {"BASE_DRIVER", no_driver}})));
        std::string message;
        EXPECT_EQ("08001", session.State(&message));
        EXPECT_TRUE(Contains(message, "Aurora DSQL doesn't support these plugins")) << message;
        EXPECT_TRUE(Contains(message, "ENABLE_CLUSTER_FAILOVER")) << message;
    }
    {
        Session session(env);
        EXPECT_EQ(SQL_ERROR, session.Connect(target.ConnStr({{"RDS_AUTH_TYPE", "IAM"}, {"BASE_DRIVER", no_driver}})));
        std::string message;
        EXPECT_EQ("08001", session.State(&message));
        EXPECT_TRUE(Contains(message, "RDS_AUTH_TYPE=IAM doesn't work with Aurora DSQL")) << message;
    }
}

// 10 (M3): an empty user name in SQLConnect keeps the DSN's user, or the default admin when the DSN has none. Needs the
// two DSNs of test/resources/odbc.ini.in: run with ODBCSYSINI=<repo>/test/resources.
TEST_F(AuroraDsqlIntegrationTest, SqlConnect_EmptyUserName_KeepsTheDsnUser) {
    for (const char* dsn : {"inte-dsql-dsn", "inte-dsql-admin-dsn"}) {
        Session session(env);
        const SQLRETURN ret = session.ConnectDsn(dsn, "", "");
        if (!SQL_SUCCEEDED(ret) && session.State() == "IM002") {
            GTEST_SKIP() << "DSN " << dsn << " not found: set ODBCSYSINI to test/resources";
        }
        ASSERT_TRUE(SQL_SUCCEEDED(ret)) << dsn << " " << session.Failure();
        EXPECT_EQ("admin", session.Fetch("SELECT current_user").value_or("<failed>")) << dsn;
    }
}

// 11 (M5): a request for an updatable cursor reads read-only instead of failing at the first SELECT.
TEST_F(AuroraDsqlIntegrationTest, UpdatableCursorRequest_ReadsReadOnly) {
    const TempTable table;
    ASSERT_TRUE(table.Ok());
    ASSERT_TRUE(SeedRow(table));

    Session session(env);
    ASSERT_EQ(SQL_SUCCESS, session.Connect(target.ConnStr())) << session.Failure();
    ASSERT_TRUE(SQL_SUCCEEDED(session.SetAutocommit(false)));

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, session.Dbc(), &stmt));
    EXPECT_TRUE(SQL_SUCCEEDED(SQLSetStmtAttr(stmt, SQL_ATTR_CURSOR_TYPE, IntPtr(SQL_CURSOR_KEYSET_DRIVEN), 0)));
    EXPECT_TRUE(SQL_SUCCEEDED(SQLSetStmtAttr(stmt, SQL_ATTR_CONCURRENCY, IntPtr(SQL_CONCUR_ROWVER), 0)));  // 01S02 allowed
    SQLULEN concurrency = 0;
    EXPECT_TRUE(SQL_SUCCEEDED(SQLGetStmtAttr(stmt, SQL_ATTR_CONCURRENCY, &concurrency, 0, nullptr)));
    EXPECT_EQ(static_cast<SQLULEN>(SQL_CONCUR_READ_ONLY), concurrency);

    std::vector<SQLTCHAR> sql = dsql_it::ToT("SELECT id, v FROM " + table.Name());
    ASSERT_TRUE(SQL_SUCCEEDED(SQLExecDirect(stmt, sql.data(), SQL_NTS))) << dsql_it::Describe(stmt, SQL_HANDLE_STMT);
    ASSERT_TRUE(SQL_SUCCEEDED(SQLFetchScroll(stmt, SQL_FETCH_NEXT, 0))) << dsql_it::Describe(stmt, SQL_HANDLE_STMT);
    SQLINTEGER id = 0;
    SQLLEN indicator = 0;
    EXPECT_TRUE(SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_SLONG, &id, sizeof(id), &indicator)));
    EXPECT_EQ(1, id);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    // An INSERT after the read, in the same transaction, commits.
    ASSERT_TRUE(SQL_SUCCEEDED(session.Exec("INSERT INTO " + table.Name() + " VALUES (2, 0, 'y')")));
    EXPECT_EQ(SQL_SUCCESS, session.EndTran(SQL_COMMIT)) << session.Failure();
    EXPECT_EQ("2", dsql_it::ReadBack("SELECT count(*) FROM " + table.Name()).value_or("<failed>"));
}

// 12 (H1, slow): the connection reports itself dead after MAX_CONNECTION_LIFETIME, and only when the option is set.
TEST_F(AuroraDsqlIntegrationTest, ConnectionDead_OnlyWithMaxConnectionLifetime) {
    DSQL_REQUIRE_SLOW();
    Session limited(env);
    Session unlimited(env);
    ASSERT_EQ(SQL_SUCCESS, limited.Connect(target.ConnStr({{"MAX_CONNECTION_LIFETIME", "120"}})))
        << limited.Failure();
    ASSERT_EQ(SQL_SUCCESS, unlimited.Connect(target.ConnStr())) << unlimited.Failure();

    std::this_thread::sleep_for(std::chrono::seconds(125));

    SQLUINTEGER dead = 99;
    EXPECT_TRUE(SQL_SUCCEEDED(SQLGetConnectAttr(limited.Dbc(), SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr)));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_CD_TRUE), dead);
    EXPECT_EQ("1", limited.Fetch("SELECT 1").value_or("<failed>")) << "a retired connection still runs statements";

    dead = 99;
    EXPECT_TRUE(SQL_SUCCEEDED(SQLGetConnectAttr(unlimited.Dbc(), SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr)));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_CD_FALSE), dead);
}

// BEHAV-2: a COMMIT after a statement failed in the transaction saves nothing, and says so with 25S03.
TEST_F(AuroraDsqlIntegrationTest, CommitAfterAnError_Returns25S03AndSavesNothing) {
    const TempTable table;
    ASSERT_TRUE(table.Ok());

    Session session(env);
    ASSERT_EQ(SQL_SUCCESS, session.Connect(target.ConnStr())) << session.Failure();
    ASSERT_TRUE(SQL_SUCCEEDED(session.SetAutocommit(false)));
    ASSERT_TRUE(SQL_SUCCEEDED(session.Exec("INSERT INTO " + table.Name() + " VALUES (1, 1, 'a')")));
    std::string state;
    EXPECT_EQ(SQL_ERROR, session.Exec("SELECT 1 / 0", &state));
    EXPECT_EQ("22012", state);

    EXPECT_EQ(SQL_ERROR, session.EndTran(SQL_COMMIT));
    EXPECT_EQ("25S03", session.State());
    EXPECT_EQ(SQL_SUCCESS, session.EndTran(SQL_ROLLBACK)) << session.Failure();

    EXPECT_EQ("0", dsql_it::ReadBack("SELECT count(*) FROM " + table.Name()).value_or("<failed>"));
}

// P2V-5: switching to autocommit commits the open transaction first, and a failed COMMIT is the call's result.
TEST_F(AuroraDsqlIntegrationTest, AutocommitChange_ReturnsTheCommitsConflict) {
    const TempTable table;
    ASSERT_TRUE(table.Ok());
    ASSERT_TRUE(SeedRow(table));

    Session first(env);
    Session second(env);
    ASSERT_EQ(SQL_SUCCESS, first.Connect(target.ConnStr())) << first.Failure();
    ASSERT_EQ(SQL_SUCCESS, second.Connect(target.ConnStr())) << second.Failure();
    ASSERT_TRUE(SQL_SUCCEEDED(first.SetAutocommit(false)));
    ASSERT_TRUE(SQL_SUCCEEDED(second.SetAutocommit(false)));
    ASSERT_TRUE(SQL_SUCCEEDED(first.Exec("UPDATE " + table.Name() + " SET v = 100 WHERE id = 1")));
    ASSERT_TRUE(SQL_SUCCEEDED(second.Exec("UPDATE " + table.Name() + " SET v = 200 WHERE id = 1")));
    ASSERT_EQ(SQL_SUCCESS, first.EndTran(SQL_COMMIT)) << first.Failure();

    EXPECT_EQ(SQL_ERROR, second.SetAutocommit(true));
    EXPECT_EQ("40001", second.State());
    SQLUINTEGER autocommit = 0;
    EXPECT_TRUE(SQL_SUCCEEDED(SQLGetConnectAttr(second.Dbc(), SQL_ATTR_AUTOCOMMIT, &autocommit, 0, nullptr)));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_AUTOCOMMIT_OFF), autocommit) << "autocommit changed after the failed commit";
    second.EndTran(SQL_ROLLBACK);

    EXPECT_EQ("100", dsql_it::ReadBack("SELECT v FROM " + table.Name() + " WHERE id = 1").value_or("<failed>"));
}
