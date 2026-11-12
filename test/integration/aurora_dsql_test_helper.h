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

// Helpers for the Aurora DSQL integration tests. They log no connection string and no token: a connection string holds
// the endpoint and, for the wrapper's output string, could echo a credential, so nothing here prints one. Failure
// messages carry SQLSTATEs and the driver's diagnostic text only.

#ifndef AURORA_DSQL_TEST_HELPER_H_
#define AURORA_DSQL_TEST_HELPER_H_

#include <gtest/gtest.h>

#include "../../driver/util/windows_headers.h"

#include <sql.h>
#include <sqlext.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "../common/test_utils.h"

namespace dsql_it {

    // Buffer sizes in characters: output connection strings and diagnostic text, column data, SQLSTATE fields.
    constexpr SQLSMALLINT TEXT_CHARS = 1024;
    constexpr size_t STATE_CHARS = 5;
    constexpr SQLSMALLINT STATE_BUFFER_CHARS = 16;

    // What the live cluster and the driver paths are, from the environment the caller provides. Nothing is read from
    // ~/.aws: the AWS SDK inside the wrapper finds credentials from the same environment.
    struct DsqlTarget {
        std::string endpoint = TEST_UTILS::GetEnvVar("TEST_DSQL_ENDPOINT");
        std::string region = TEST_UTILS::GetEnvVar("TEST_DSQL_REGION");
        std::string cluster_id = TEST_UTILS::GetEnvVar("TEST_DSQL_CLUSTER_ID");
        std::string driver_path = TEST_UTILS::GetEnvVar("TEST_DRIVER_PATH");
        std::string base_driver_path = TEST_UTILS::GetEnvVar("BASE_DRIVER_PATH");
        std::string iam_role_arn = TEST_UTILS::GetEnvVar("TEST_DSQL_IAM_ROLE_ARN");
        std::string psqlodbc_version = TEST_UTILS::GetEnvVar("TEST_PSQLODBC_VERSION");
        bool slow = TEST_UTILS::GetEnvVar("TEST_DSQL_SLOW") == "1";

        static const DsqlTarget& Get() {
            static const DsqlTarget target;
            return target;
        }

        // The suite runs only against a live cluster.
        [[nodiscard]] bool Enabled() const { return !endpoint.empty(); }

        // DRIVER=<wrapper>;SERVER=<endpoint>;RDS_AUTH_TYPE=DSQL;BASE_DRIVER=<psqlodbc>; plus extra. An extra whose key
        // matches (case-insensitively) a default replaces its value; an extra with an empty value removes the key.
        [[nodiscard]] std::string ConnStr(std::initializer_list<std::pair<std::string, std::string>> extra = {}) const {
            std::vector<std::pair<std::string, std::string>> entries = {
                {"DRIVER", driver_path},
                {"SERVER", endpoint},
                {"RDS_AUTH_TYPE", "DSQL"},
                {"BASE_DRIVER", base_driver_path},
            };
            for (const auto& [key, value] : extra) {
                bool found = false;
                for (auto& entry : entries) {
                    if (SameKey(entry.first, key)) {
                        entry.second = value;
                        found = true;
                    }
                }
                if (!found) {
                    entries.emplace_back(key, value);
                }
            }
            std::string out;
            for (const auto& [key, value] : entries) {
                if (!value.empty()) {
                    out.append(key).append("=").append(value).append(";");
                }
            }
            return out;
        }

    private:
        static bool SameKey(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) {
                return false;
            }
            for (size_t i = 0; i < a.size(); i++) {
                if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return true;
        }
    };

    // The suite's strings are ASCII: each char becomes one SQLTCHAR.
    inline std::vector<SQLTCHAR> ToT(const std::string& s) {
        std::vector<SQLTCHAR> out;
        out.reserve(s.size() + 1);
        for (const char c : s) {
            out.push_back(static_cast<SQLTCHAR>(static_cast<unsigned char>(c)));
        }
        out.push_back(0);
        return out;
    }

    inline std::string FromT(const SQLTCHAR* in, size_t count) {
        std::string out;
        for (size_t i = 0; i < count && in[i] != 0; i++) {
            out.push_back(static_cast<char>(static_cast<unsigned int>(in[i]) % 256U));
        }
        return out;
    }

    // The SQLSTATE of the first diagnostic record of a handle, with its text in *message when asked for.
    inline std::string DiagState(SQLHANDLE handle, SQLSMALLINT handle_type, std::string* message = nullptr) {
        SQLTCHAR state[STATE_BUFFER_CHARS] = {0};
        SQLTCHAR text[TEXT_CHARS] = {0};
        SQLINTEGER native = 0;
        SQLSMALLINT length = 0;
        if (!SQL_SUCCEEDED(SQLGetDiagRec(handle_type, handle, 1, state, &native, text, TEXT_CHARS, &length))) {
            return "";
        }
        if (message != nullptr) {
            *message = FromT(text, TEXT_CHARS);
        }
        return FromT(state, STATE_CHARS);
    }

    // The same SQLSTATE read through SQLGetDiagField.
    inline std::string DiagFieldState(SQLHANDLE handle, SQLSMALLINT handle_type) {
        SQLTCHAR state[STATE_BUFFER_CHARS] = {0};
        SQLSMALLINT length = 0;
        if (!SQL_SUCCEEDED(SQLGetDiagField(handle_type, handle, 1, SQL_DIAG_SQLSTATE, state, sizeof(state), &length))) {
            return "";
        }
        return FromT(state, STATE_CHARS);
    }

    // A one-line description of a failure for an assertion message: the SQLSTATE and the driver's text.
    inline std::string Describe(SQLHANDLE handle, SQLSMALLINT handle_type) {
        std::string message;
        const std::string state = DiagState(handle, handle_type, &message);
        return "SQLSTATE " + (state.empty() ? std::string("none") : state) + ": " + message;
    }

    // Executes sql on a new statement of dbc and frees it. *state gets the statement's SQLSTATE when it didn't succeed.
    inline SQLRETURN Exec(SQLHDBC dbc, const std::string& sql, std::string* state = nullptr) {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt))) {
            if (state != nullptr) {
                *state = DiagState(dbc, SQL_HANDLE_DBC);
            }
            return SQL_ERROR;
        }
        std::vector<SQLTCHAR> text = ToT(sql);
        const SQLRETURN ret = SQLExecDirect(stmt, text.data(), SQL_NTS);
        if (state != nullptr) {
            *state = ret == SQL_SUCCESS ? "" : DiagState(stmt, SQL_HANDLE_STMT);
        }
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return ret;
    }

    // The first column of the first row of a query, or nullopt when the query or the fetch fails.
    inline std::optional<std::string> FetchString(SQLHDBC dbc, const std::string& sql) {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt))) {
            return std::nullopt;
        }
        std::optional<std::string> result;
        std::vector<SQLTCHAR> text = ToT(sql);
        if (SQL_SUCCEEDED(SQLExecDirect(stmt, text.data(), SQL_NTS)) && SQL_SUCCEEDED(SQLFetch(stmt))) {
            SQLTCHAR buffer[TEXT_CHARS] = {0};
            SQLLEN indicator = 0;
            if (SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_TCHAR, buffer, sizeof(buffer), &indicator))) {
                result = indicator == SQL_NULL_DATA ? std::string("NULL") : FromT(buffer, TEXT_CHARS);
            }
        }
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return result;
    }

    // One connection handle on an environment the caller owns. The destructor disconnects and frees it.
    class Session {
    public:
        explicit Session(SQLHENV env) {
            SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc_);
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        ~Session() {
            if (dbc_ != SQL_NULL_HDBC) {
                SQLDisconnect(dbc_);
                SQLFreeHandle(SQL_HANDLE_DBC, dbc_);
            }
        }

        [[nodiscard]] SQLHDBC Dbc() const { return dbc_; }

        // SQLDriverConnect. *out_string gets the driver's output connection string.
        SQLRETURN Connect(const std::string& conn_str, std::string* out_string = nullptr) {
            std::vector<SQLTCHAR> in = ToT(conn_str);
            SQLTCHAR out[TEXT_CHARS] = {0};
            SQLSMALLINT out_length = 0;
            const SQLRETURN ret = SQLDriverConnect(dbc_, nullptr, in.data(), SQL_NTS, out, TEXT_CHARS, &out_length,
                                                   SQL_DRIVER_NOPROMPT);
            if (out_string != nullptr) {
                *out_string = FromT(out, TEXT_CHARS);
            }
            return ret;
        }

        // SQLConnect (SQLConnectW in the Unicode build).
        SQLRETURN ConnectDsn(const std::string& dsn, const std::string& uid, const std::string& pwd) {
            std::vector<SQLTCHAR> d = ToT(dsn);
            std::vector<SQLTCHAR> u = ToT(uid);
            std::vector<SQLTCHAR> p = ToT(pwd);
            return SQLConnect(dbc_, d.data(), SQL_NTS, u.data(), SQL_NTS, p.data(), SQL_NTS);
        }

        SQLRETURN Disconnect() { return SQLDisconnect(dbc_); }

        [[nodiscard]] std::string State(std::string* message = nullptr) const { return DiagState(dbc_, SQL_HANDLE_DBC, message); }
        [[nodiscard]] std::string Failure() const { return Describe(dbc_, SQL_HANDLE_DBC); }

        SQLRETURN Exec(const std::string& sql, std::string* state = nullptr) { return dsql_it::Exec(dbc_, sql, state); }
        std::optional<std::string> Fetch(const std::string& sql) { return FetchString(dbc_, sql); }

        SQLRETURN SetAutocommit(bool on) {
            return SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(on ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF)), 0);
        }

        SQLRETURN EndTran(SQLSMALLINT completion) { return SQLEndTran(SQL_HANDLE_DBC, dbc_, completion); }

    private:
        SQLHDBC dbc_ = SQL_NULL_HDBC;
    };

    // A short-lived environment and connection for a read of what another connection committed.
    inline std::optional<std::string> ReadBack(const std::string& sql) {
        SQLHENV env = SQL_NULL_HENV;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
            return std::nullopt;
        }
        SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
        std::optional<std::string> result;
        {
            Session session(env);
            if (SQL_SUCCEEDED(session.Connect(DsqlTarget::Get().ConnStr()))) {
                result = session.Fetch(sql);
            }
        }
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return result;
    }

    // Runs sql on a new connection (for cleanup that must not depend on the test's connection state).
    inline SQLRETURN ExecOnFreshConnection(const std::string& sql) {
        SQLHENV env = SQL_NULL_HENV;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
            return SQL_ERROR;
        }
        SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
        SQLRETURN ret = SQL_ERROR;
        {
            Session session(env);
            if (SQL_SUCCEEDED(session.Connect(DsqlTarget::Get().ConnStr()))) {
                ret = session.Exec(sql);
            }
        }
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return ret;
    }

    inline int ProcessId() {
#ifdef _WIN32
        return _getpid();
#else
        return static_cast<int>(getpid());
#endif
    }

    // A table with a unique name (odbc_it_<pid>_<random>_<n>), created in autocommit on a connection of its own and dropped on
    // another at the end, also when the test fails.
    class TempTable {
    public:
        TempTable() : name_("odbc_it_" + std::to_string(ProcessId()) + "_" + RunToken() + "_" + std::to_string(++counter_)) {
            created_ = SQL_SUCCEEDED(ExecOnFreshConnection(
                "CREATE TABLE " + name_ + " (id INT PRIMARY KEY, v INT, s TEXT)"));
        }
        TempTable(const TempTable&) = delete;
        TempTable& operator=(const TempTable&) = delete;
        ~TempTable() {
            // Only a table this object created: the name is unique to this process, so no other run's table is hit.
            if (created_) {
                ExecOnFreshConnection("DROP TABLE IF EXISTS " + name_);
            }
        }

        [[nodiscard]] bool Ok() const { return created_; }
        [[nodiscard]] const std::string& Name() const { return name_; }

    private:
        // 8 hex characters chosen once per process: a container's pids repeat from run to run.
        static const std::string& RunToken() {
            static const std::string token = [] {
                std::random_device device;
                char buffer[9];
                std::snprintf(buffer, sizeof(buffer), "%08x", static_cast<unsigned int>(device()));
                return std::string(buffer);
            }();
            return token;
        }

        static inline std::atomic<int> counter_ = 0;
        std::string name_;
        bool created_ = false;
    };

    // Sets an environment variable for the life of the object and restores the old value (nullopt: unset).
    class ScopedEnv {
    public:
        ScopedEnv(std::string name, const std::optional<std::string>& value) : name_(std::move(name)) {
            if (const char* old = std::getenv(name_.c_str()); old != nullptr) {
                old_ = std::string(old);
            }
            Set(value);
        }
        ScopedEnv(const ScopedEnv&) = delete;
        ScopedEnv& operator=(const ScopedEnv&) = delete;
        ~ScopedEnv() { Set(old_); }

    private:
        void Set(const std::optional<std::string>& value) const {
#ifdef _WIN32
            _putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
            if (value) {
                setenv(name_.c_str(), value->c_str(), 1);
            } else {
                unsetenv(name_.c_str());
            }
#endif
        }

        std::string name_;
        std::optional<std::string> old_;
    };

}  // namespace dsql_it

// Skip the rest of a test that needs the slow opt-in. Used in the test body, as GTEST_SKIP returns from the function.
#define DSQL_REQUIRE_SLOW() \
    if (!target.slow) {     \
        GTEST_SKIP() << "slow case: set TEST_DSQL_SLOW=1"; \
    }

#endif  // AURORA_DSQL_TEST_HELPER_H_
