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

// The driver manager's connection pool against a live Aurora DSQL cluster. unixODBC pooling is process-wide, so run
// these cases in a process of their own, with ODBCSYSINI set to the directory that holds the pooled odbcinst.ini the
// build generates (<build>/dsql-pool):
//   ODBCSYSINI=<build>/dsql-pool TEST_DSQL_ENDPOINT=... TEST_DSQL_SLOW=1 \
//       ./integration-test --gtest_filter='AuroraDsqlPoolIntegrationTest.*'
// Every case skips unless TEST_DSQL_ENDPOINT and TEST_DSQL_SLOW=1 are set (each waits past the 120 s lifetime), and
// when the driver manager doesn't list the pooled driver.

#include <gtest/gtest.h>

#include "../../driver/util/windows_headers.h"

#include <sql.h>
#include <sqlext.h>

#include <chrono>
#include <string>
#include <thread>

#include "aurora_dsql_test_helper.h"

using dsql_it::DsqlTarget;
using dsql_it::Session;

namespace {
    constexpr const char* POOLED_DRIVER = "AWS DSQL Pooled";  // in <build>/dsql-pool/odbcinst.ini
}  // namespace

class AuroraDsqlPoolIntegrationTest : public testing::Test {
protected:
    const DsqlTarget& target = DsqlTarget::Get();
    SQLHENV env = SQL_NULL_HENV;

    void SetUp() override {
        if (!target.Enabled()) {
            GTEST_SKIP() << "TEST_DSQL_ENDPOINT is not set";
        }
        if (!target.slow) {
            GTEST_SKIP() << "slow case: set TEST_DSQL_SLOW=1";
        }
        if (TEST_UTILS::GetEnvVar("ODBCSYSINI").empty()) {
            GTEST_SKIP() << "ODBCSYSINI is not set to the pooled odbcinst.ini directory";
        }
        // Pooling has to be on before the first environment is allocated.
        ASSERT_TRUE(SQL_SUCCEEDED(SQLSetEnvAttr(SQL_NULL_HENV, SQL_ATTR_CONNECTION_POOLING,
                                                reinterpret_cast<SQLPOINTER>(SQL_CP_ONE_PER_DRIVER), 0)));
        ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env));
        ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
        // unixODBC doesn't answer an unknown DRIVER={...} with IM002: it tries to load the name as a library path.
        if (!DriverListed(POOLED_DRIVER)) {
            GTEST_SKIP() << "the driver manager doesn't list " << POOLED_DRIVER
                         << ": set ODBCSYSINI to the directory of the pooled odbcinst.ini";
        }
    }

    void TearDown() override {
        if (env != SQL_NULL_HENV) {
            SQLFreeHandle(SQL_HANDLE_ENV, env);
        }
    }

    // Whether SQLDrivers lists the driver: ODBCSYSINI can name a directory without the pooled odbcinst.ini.
    [[nodiscard]] bool DriverListed(const std::string& name) const {
        SQLTCHAR description[dsql_it::TEXT_CHARS] = {0};
        SQLTCHAR attributes[dsql_it::TEXT_CHARS] = {0};
        SQLSMALLINT description_length = 0;
        SQLSMALLINT attributes_length = 0;
        for (SQLUSMALLINT direction = SQL_FETCH_FIRST;
             SQL_SUCCEEDED(SQLDrivers(env, direction, description, dsql_it::TEXT_CHARS, &description_length, attributes,
                                      dsql_it::TEXT_CHARS, &attributes_length));
             direction = SQL_FETCH_NEXT) {
            if (dsql_it::FromT(description, dsql_it::TEXT_CHARS) == name) {
                return true;
            }
        }
        return false;
    }

    // True when the connection the pool hands out still has the marker search_path set on an earlier one.
    bool Reused(const std::string& conn_str, bool set_marker) {
        Session session(env);
        if (!SQL_SUCCEEDED(session.Connect(conn_str))) {
            ADD_FAILURE() << session.Failure();
            return false;
        }
        const bool marked = session.Fetch("SHOW search_path").value_or("").find("it_marker") != std::string::npos;
        if (set_marker) {
            EXPECT_TRUE(SQL_SUCCEEDED(session.Exec("SET search_path TO it_marker, public")));
        }
        session.Disconnect();  // returns the connection to the pool
        return marked;
    }

    [[nodiscard]] std::string PooledConnStr(bool with_lifetime) const {
        const std::string driver = "{" + std::string(POOLED_DRIVER) + "}";
        return with_lifetime ? target.ConnStr({{"DRIVER", driver}, {"MAX_CONNECTION_LIFETIME", "120"}})
                             : target.ConnStr({{"DRIVER", driver}});
    }
};

// With MAX_CONNECTION_LIFETIME set, the pool replaces a connection older than the limit.
TEST_F(AuroraDsqlPoolIntegrationTest, SetLifetime_ThePoolReplacesTheConnection) {
    const std::string conn_str = PooledConnStr(true);
    EXPECT_FALSE(Reused(conn_str, true)) << "the first connection already had the marker";
    EXPECT_TRUE(Reused(conn_str, false)) << "the pool did not hand the connection out again";
    std::this_thread::sleep_for(std::chrono::seconds(125));
    EXPECT_FALSE(Reused(conn_str, false)) << "the pool reused a connection past MAX_CONNECTION_LIFETIME";
}

// Without MAX_CONNECTION_LIFETIME the pool keeps the connection.
TEST_F(AuroraDsqlPoolIntegrationTest, UnsetLifetime_ThePoolKeepsTheConnection) {
    const std::string conn_str = PooledConnStr(false);
    EXPECT_FALSE(Reused(conn_str, true)) << "the first connection already had the marker";
    EXPECT_TRUE(Reused(conn_str, false)) << "the pool did not hand the connection out again";
    std::this_thread::sleep_for(std::chrono::seconds(125));
    EXPECT_TRUE(Reused(conn_str, false)) << "the pool replaced the connection with no MAX_CONNECTION_LIFETIME";
}
