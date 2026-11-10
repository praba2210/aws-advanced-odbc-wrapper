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
#include <tuple>
#include <utility>
#include <vector>

#include "../../driver/driver.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/default_plugin.h"
#include "../../driver/plugin/dsql/dsql_compatibility.h"
#include "../../driver/util/connection_string_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/dsql_connection_validator.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string AURORA_HOST = "database-test-name.cluster-XYZ.us-east-2.rds.amazonaws.com";
    const std::string PSQLODBC_PATH = "/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so";

    // The connection string the last SQLDriverConnect got, as ASCII.
    std::string captured_conn_str;
    // What the base SQLDriverConnect returns.
    SQLRETURN driver_connect_result = SQL_ERROR;
    // The base driver's libpq version, as its PQlibVersion returns it; 0 for a base driver without that function.
    int base_libpq_version = 0;

    int FakePQlibVersion() {
        return base_libpq_version;
    }

    SQLRETURN FakeAllocHandle(SQLSMALLINT, SQLHANDLE, SQLHANDLE* output_handle) {
        *output_handle = static_cast<SQLHANDLE>(&captured_conn_str);
        return SQL_SUCCESS;
    }

    // ASCII text in the base driver's characters, to its NUL.
    std::string AsciiText(const SQLTCHAR* text) {
        std::string ascii;
        for (const SQLTCHAR* c = text; *c != 0; ++c) {
            ascii.push_back(static_cast<char>(*c));
        }
        return ascii;
    }

    SQLRETURN FakeDriverConnect(SQLHDBC, SQLHWND, SQLTCHAR* in_connection_string, SQLSMALLINT, SQLTCHAR*, SQLSMALLINT,
                                SQLSMALLINT*, SQLUSMALLINT) {
        captured_conn_str = AsciiText(in_connection_string);
        // Most tests only need the string psqlODBC would get.
        return driver_connect_result;
    }

    // The base SQLEndTran, SQLSetConnectAttr and SQLExecDirect calls, in order, as EndTranCall, SetConnectAttrCall and
    // ExecDirectCall write them.
    std::vector<std::string> base_calls;
    // What the base SQLEndTran and SQLExecDirect return.
    SQLRETURN end_tran_result = SQL_SUCCESS;
    SQLRETURN exec_direct_result = SQL_SUCCESS;
    // The SQLSTATE of the base driver's record 1 on any handle; none when empty.
    std::string base_sql_state;

    std::string EndTranCall(SQLSMALLINT completion_type) {
        return std::string("SQLEndTran(") + (completion_type == SQL_COMMIT ? "SQL_COMMIT" : "SQL_ROLLBACK") + ")";
    }

    std::string SetConnectAttrCall(SQLINTEGER attribute, SQLULEN value) {
        return "SQLSetConnectAttr(" + std::to_string(attribute) + ", " + std::to_string(value) + ")";
    }

    std::string ExecDirectCall(const std::string& text) {
        return "SQLExecDirect(" + text + ")";
    }

    SQLRETURN FakeEndTran(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT completion_type) {
        base_calls.push_back(EndTranCall(completion_type));
        return end_tran_result;
    }

    SQLRETURN FakeSetConnectAttr(SQLHDBC /*connection*/, SQLINTEGER attribute, SQLPOINTER value,
                                 SQLINTEGER /*length*/) {
        base_calls.push_back(SetConnectAttrCall(attribute, static_cast<SQLULEN>(reinterpret_cast<uintptr_t>(value))));
        return SQL_SUCCESS;
    }

    SQLRETURN FakeExecDirect(SQLHSTMT /*statement*/, SQLTCHAR* text, SQLINTEGER /*length*/) {
        base_calls.push_back(ExecDirectCall(AsciiText(text)));
        return exec_direct_result;
    }

    SQLRETURN FakeGetInfo(SQLHDBC /*connection*/, SQLUSMALLINT /*info_type*/, SQLPOINTER /*value*/,
                          SQLSMALLINT /*buffer_length*/, SQLSMALLINT* /*length*/) {
        return SQL_SUCCESS;
    }

    SQLRETURN FakeGetDiagRec(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT record, SQLTCHAR* state,
                             SQLINTEGER* native_error, SQLTCHAR* message, SQLSMALLINT buffer_length,
                             SQLSMALLINT* text_length) {
        if (record != 1 || base_sql_state.empty()) {
            return SQL_NO_DATA;
        }
        if (state != nullptr) {
            for (size_t i = 0; i <= base_sql_state.size(); ++i) {
                state[i] = static_cast<SQLTCHAR>(i < base_sql_state.size() ? base_sql_state[i] : 0);
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

    // Answers SQLAllocHandle, SQLDriverConnect, SQLEndTran, SQLSetConnectAttr, SQLExecDirect, SQLGetDiagRec and
    // SQLGetInfo, and PQlibVersion when base_libpq_version is set, records every lookup, and returns nullptr for
    // anything else, so a forwarded call fails without reaching a real driver.
    class FakeBaseDriver : public RdsLibLoader {
    public:
        RdsPlatform::FuncHandle GetFunction(const std::string& function_name) override {
            requested_functions.push_back(function_name);
            if (function_name == "PQlibVersion") {
                return base_libpq_version != 0 ? reinterpret_cast<RdsPlatform::FuncHandle>(&FakePQlibVersion) : nullptr;
            }
            const std::map<std::string_view, RdsPlatform::FuncHandle> functions = {
                {RdsFuncNames::AllocHandle, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeAllocHandle)},
                {RdsFuncNames::DriverConnect, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeDriverConnect)},
                {RdsFuncNames::EndTran, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeEndTran)},
                {RdsFuncNames::SetConnectAttr, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeSetConnectAttr)},
                {RdsFuncNames::ExecDirect, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeExecDirect)},
                {RdsFuncNames::GetDiagRec, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetDiagRec)},
                {RdsFuncNames::GetInfo, reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetInfo)},
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

    // Collects every log line while it exists.
    class CapturingLogSink : public nglog::LogSink {
    public:
        CapturingLogSink() { nglog::AddLogSink(this); }
        ~CapturingLogSink() override { nglog::RemoveLogSink(this); }
        CapturingLogSink(const CapturingLogSink&) = delete;
        CapturingLogSink& operator=(const CapturingLogSink&) = delete;

        void send(nglog::LogSeverity /*severity*/, const char* /*full_filename*/, const char* /*base_filename*/,
                  int /*line*/, const nglog::LogMessageTime& /*time*/, const char* message, size_t message_len) override {
            const std::lock_guard<std::mutex> lock(mutex_);
            messages_.emplace_back(message, message_len);
        }

        // How many lines contain the text.
        std::ptrdiff_t Count(const std::string& text) {
            const std::lock_guard<std::mutex> lock(mutex_);
            return std::ranges::count_if(messages_, [&text](const std::string& m) { return m.find(text) != std::string::npos; });
        }

    private:
        std::mutex mutex_;
        std::vector<std::string> messages_;
    };
}  // namespace

class DsqlCompatibilityTest : public testing::Test {
protected:
    std::shared_ptr<FakeBaseDriver> base_driver_ = std::make_shared<FakeBaseDriver>();
    ENV env_;
    DBC dbc_{};
    STMT stmt_{};

    void SetUp() override {
        env_.driver_lib_loader = base_driver_;
        dbc_.env = &env_;
        stmt_.dbc = &dbc_;
        captured_conn_str.clear();
        driver_connect_result = SQL_ERROR;
        base_libpq_version = 0;
        base_calls.clear();
        end_tran_result = SQL_SUCCESS;
        exec_direct_result = SQL_SUCCESS;
        base_sql_state.clear();
    }

    void TearDown() override {
        // The fake handles point at test objects; never let them outlive the test.
        OdbcHelper::SetWrappedStmt(&stmt_, SQL_NULL_HSTMT);
        dbc_.wrapped_dbc = SQL_NULL_HDBC;
        env_.wrapped_env = SQL_NULL_HENV;
        dbc_.plugin_service = nullptr;
    }

    // The attributes RDS_InitializeConnection leaves for this server, its answer to whether the connect is Aurora DSQL,
    // and the plugin service it builds.
    void UseServer(const std::string& server) {
        UseConnAttr({{KEY_SERVER, server}});
    }

    // The same for these attributes and the base driver.
    void UseConnAttr(std::map<std::string, std::string> conn_attr) {
        dbc_.conn_attr = std::move(conn_attr);
        dbc_.conn_attr.insert_or_assign(KEY_DRIVER, PSQLODBC_PATH);
        dbc_.is_dsql = DsqlConnectionValidator::IsDsqlTarget(dbc_.conn_attr);
        dbc_.plugin_service = std::make_shared<PluginService>(base_driver_, &env_, dbc_.conn_attr, "");
    }

    // Any non-null handles and the status a connect leaves, so the wrapper treats the connection and the statement as
    // open.
    void Connect() {
        dbc_.wrapped_dbc = static_cast<SQLHDBC>(&dbc_);
        dbc_.conn_status = CONN_CONNECTED;
        OdbcHelper::SetWrappedStmt(&stmt_, static_cast<SQLHSTMT>(&stmt_));
    }
};

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_TurnServerSidePrepareOff) {
    std::map<std::string, std::string> conn_attr;
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    EXPECT_EQ("0", conn_attr.at("USESERVERSIDEPREPARE"));
}

// psqlODBC's DSN setup writes UseServerSidePrepare=1 and BASE_DSN copies it into conn_attr, so a value there isn't a
// choice.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_OverrideUserServerSidePrepare) {
    std::map<std::string, std::string> long_form = {{"USESERVERSIDEPREPARE", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(long_form);
    EXPECT_EQ("0", long_form.at("USESERVERSIDEPREPARE"));

    std::map<std::string, std::string> short_form = {{"C8", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("C8"));
    EXPECT_EQ("0", short_form.at("USESERVERSIDEPREPARE"));
}

// CX=1c2305bab: 0x1c flags, bitmask 0x2305bab, which has UseServerSidePrepare (1 << 25) and UseDeclareFetch (1 << 14)
// on. psqlODBC reads the connection string from left to right, so the long keyword must come after CX.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_LongKeyFollowsCxFlags) {
    std::map<std::string, std::string> conn_attr = {{"CX", "1c2305bab"}};
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    const std::string conn_str = ConnectionStringHelper::BuildMinimumConnectionString(conn_attr);
    const size_t cx = conn_str.find("CX=1c2305bab");
    const size_t server_side_prepare = conn_str.find("USESERVERSIDEPREPARE=0");
    ASSERT_NE(std::string::npos, cx);
    ASSERT_NE(std::string::npos, server_side_prepare);
    EXPECT_LT(cx, server_side_prepare);
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_LogOnlyChangedValues) {
    CapturingLogSink log;
    std::map<std::string, std::string> changed = {{"USESERVERSIDEPREPARE", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(changed);
    std::map<std::string, std::string> unchanged = {{"USESERVERSIDEPREPARE", "0"}};
    DsqlCompatibility::ApplyBaseDriverOptions(unchanged);
    EXPECT_EQ(1, log.Count("[DSQL] Overriding UseServerSidePrepare=1 with 0: freeing a prepared statement inside a "
                           "transaction can silently roll the transaction back on Aurora DSQL"));
    EXPECT_EQ(0, log.Count("[DSQL] Overriding UseServerSidePrepare=0"));
}

// RDS_InitializeConnection's answer for the connect (DsqlConnectionValidator::IsDsqlTarget, tested there); conn_attr
// isn't read again.
TEST_F(DsqlCompatibilityTest, IsDsql_ReadsTheConnectsAnswer) {
    EXPECT_FALSE(DsqlCompatibility::IsDsql(nullptr));
    EXPECT_FALSE(DsqlCompatibility::IsDsql(&dbc_));  // before the first connect
    dbc_.conn_attr = {{KEY_SERVER, DSQL_HOST}};
    EXPECT_FALSE(DsqlCompatibility::IsDsql(&dbc_));
    dbc_.is_dsql = true;
    dbc_.conn_attr = {{KEY_SERVER, AURORA_HOST}};
    EXPECT_TRUE(DsqlCompatibility::IsDsql(&dbc_));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_Dsql_TurnsServerSidePrepareOff) {
    UseServer(DSQL_HOST);
    dbc_.conn_attr.insert_or_assign("C8", "1");
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_FALSE(dbc_.conn_attr.contains("C8"));
    EXPECT_EQ("0", dbc_.conn_attr.at("USESERVERSIDEPREPARE"));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherDatabase_ChangesNothing) {
    UseServer(AURORA_HOST);
    dbc_.conn_attr.insert_or_assign("USESERVERSIDEPREPARE", "1");
    const std::map<std::string, std::string> before = dbc_.conn_attr;
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_EQ(before, dbc_.conn_attr);
}

// A reconnect on the same DBC keeps its plugin chain and conn_attr, so PrepareConnect runs again on its own output.
TEST_F(DsqlCompatibilityTest, PrepareConnect_Twice_IsIdempotent) {
    UseServer(DSQL_HOST);
    DsqlCompatibility::PrepareConnect(&dbc_);
    const std::map<std::string, std::string> once = dbc_.conn_attr;
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_EQ(once, dbc_.conn_attr);
}

TEST_F(DsqlCompatibilityTest, DefaultPluginConnect_Dsql_BaseDriverGetsServerSidePrepareOff) {
    UseServer(DSQL_HOST);
    env_.wrapped_env = static_cast<SQLHENV>(&env_);
    DefaultPlugin plugin(&dbc_);
    EXPECT_EQ(SQL_ERROR, plugin.Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_NE(std::string::npos, captured_conn_str.find("SERVER=" + DSQL_HOST));
    EXPECT_NE(std::string::npos, captured_conn_str.find("USESERVERSIDEPREPARE=0"));
    EXPECT_NE(std::string::npos, captured_conn_str.find("UPDATABLECURSORS=0"));
}

TEST_F(DsqlCompatibilityTest, DefaultPluginConnect_OtherDatabase_BaseDriverGetsNoDsqlOptions) {
    UseServer(AURORA_HOST);
    env_.wrapped_env = static_cast<SQLHENV>(&env_);
    DefaultPlugin plugin(&dbc_);
    EXPECT_EQ(SQL_ERROR, plugin.Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_NE(std::string::npos, captured_conn_str.find("SERVER=" + AURORA_HOST));
    for (const char* key : {"PROTOCOL=", "IGNORETIMEOUT=", "USEDECLAREFETCH=", "USESERVERSIDEPREPARE=",
                            "UPDATABLECURSORS=", "SSLMODE=", "PQOPT=", "DATABASE=",}) {
        EXPECT_EQ(std::string::npos, captured_conn_str.find(key)) << key;
    }
}

namespace {
    const std::string APPLICATION_NAME = std::string("aurora-dsql-odbc/") + DRIVER_VERSION;
    // The base driver's pqopt for verify-full: the root certificate (the fake base driver has no PQlibVersion, so it is
    // libpq 16's system) and the application name.
    const std::string VERIFY_FULL_PQOPT = "{sslrootcert=system application_name=" + APPLICATION_NAME + "}";

    // The 01S02 text for an SSLmode replaced with verify-full, worded as the Aurora DSQL auth plugin's.
    std::string SslModeChanged(const std::string& mode) {
        return "SSLmode=" + mode + " was changed to verify-full: Aurora DSQL requires TLS. Set SSLmode to verify-full, "
               "verify-ca or require.";
    }
}  // namespace

// psqlODBC runs an unset SSLmode as disable, and Aurora DSQL refuses that before any authentication. An auth type other
// than DSQL (none, which is DATABASE: a token in PWD; or SECRETS_MANAGER) gets verify-full with the Aurora DSQL
// plugin's root certificate default, and postgres, Aurora DSQL's only database, where libpq would use the user name.
// No user name is added: the admin default is RDS_AUTH_TYPE=DSQL's, whose token is signed for it.
TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherAuth_UnsetSslMode_BecomesVerifyFull) {
    for (const std::string auth_type : {"", VALUE_AUTH_DATABASE, VALUE_AUTH_SECRETS}) {
        SCOPED_TRACE(auth_type);
        std::map<std::string, std::string> conn_attr = {{KEY_SERVER, DSQL_HOST}};
        if (!auth_type.empty()) {
            conn_attr.emplace(KEY_AUTH_TYPE, auth_type);
        }
        UseConnAttr(conn_attr);
        const DsqlCompatibility::PreparedConnect prepared = DsqlCompatibility::PrepareConnect(&dbc_);
        EXPECT_EQ("verify-full", dbc_.conn_attr.at("SSLMODE"));
        EXPECT_EQ(VERIFY_FULL_PQOPT, dbc_.conn_attr.at("PQOPT"));
        EXPECT_EQ("postgres", dbc_.conn_attr.at(KEY_DATABASE));
        EXPECT_FALSE(dbc_.conn_attr.contains(KEY_DB_USERNAME));
        EXPECT_EQ("", prepared.tls_warning);
    }
}

// The database the user set stays, with a mode that is raised (none) or kept (require).
TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherAuth_KeepsTheDatabaseSet) {
    for (const std::string mode : {"", "require"}) {
        SCOPED_TRACE(mode);
        std::map<std::string, std::string> conn_attr = {{KEY_SERVER, DSQL_HOST}, {KEY_DATABASE, "mydb"}};
        if (!mode.empty()) {
            conn_attr.emplace("SSLMODE", mode);
        }
        UseConnAttr(conn_attr);
        DsqlCompatibility::PrepareConnect(&dbc_);
        EXPECT_EQ("mydb", dbc_.conn_attr.at(KEY_DATABASE));

        conn_attr.erase(KEY_DATABASE);
        conn_attr.emplace("DX", "mydb");
        UseConnAttr(conn_attr);
        DsqlCompatibility::PrepareConnect(&dbc_);
        EXPECT_EQ("mydb", dbc_.conn_attr.at("DX"));
        EXPECT_FALSE(dbc_.conn_attr.contains(KEY_DATABASE));
    }
}

// psqlODBC reads the mode by its first byte, case-sensitively, and runs disable, d, Disable or a value it doesn't know
// as disable. Each becomes verify-full, under either keyword, and the text for the connect's 01S02 says so.
TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherAuth_Disable_BecomesVerifyFullWith01S02Text) {
    CapturingLogSink log;
    for (const auto& [key, mode] : std::vector<std::pair<std::string, std::string>>{
             {"SSLMODE", "disable"}, {"SSLMODE", "d"}, {"SSLMODE", "Disable"}, {"SSLMODE", "on"}, {"CA", "d"},}) {
        SCOPED_TRACE(testing::Message() << key << "=" << mode);
        UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}, {key, mode}});
        const DsqlCompatibility::PreparedConnect prepared = DsqlCompatibility::PrepareConnect(&dbc_);
        EXPECT_EQ("verify-full", dbc_.conn_attr.at("SSLMODE"));
        EXPECT_FALSE(dbc_.conn_attr.contains("CA"));
        EXPECT_EQ(VERIFY_FULL_PQOPT, dbc_.conn_attr.at("PQOPT"));
        EXPECT_EQ(SslModeChanged(mode), prepared.tls_warning);
    }
    EXPECT_EQ(1, log.Count("[DSQL] Overriding SSLmode=Disable with verify-full"));
    EXPECT_EQ(5, log.Count("[DSQL] Overriding SSLmode="));
}

// A mode psqlODBC runs (allow, prefer, require, verify-ca, verify-full, or its short forms r, vc and vf, under either
// keyword) connects today, maybe with the user's own root certificate (~/.postgresql/root.crt, PGSSLROOTCERT) or
// PGAPPNAME, which a pqopt default would override. Its SSLmode and pqopt are left exactly as set, and nothing is
// reported. Only the database default is added: without one libpq uses the user name, and Aurora DSQL has only
// postgres, so such a connect can't work today.
TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherAuth_KeepsWorkingModes) {
    CapturingLogSink log;
    for (const auto& [key, mode] : std::vector<std::pair<std::string, std::string>>{
             {"SSLMODE", "allow"}, {"SSLMODE", "prefer"}, {"SSLMODE", "require"}, {"SSLMODE", "verify-ca"},
             {"SSLMODE", "verify-full"}, {"SSLMODE", "r"}, {"SSLMODE", "vc"}, {"SSLMODE", "vf"}, {"CA", "require"},}) {
        SCOPED_TRACE(testing::Message() << key << "=" << mode);
        UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}, {key, mode}});
        std::map<std::string, std::string> expected = dbc_.conn_attr;
        DsqlCompatibility::ApplyBaseDriverOptions(expected);
        expected.emplace(KEY_DATABASE, "postgres");
        const DsqlCompatibility::PreparedConnect prepared = DsqlCompatibility::PrepareConnect(&dbc_);
        EXPECT_EQ(expected, dbc_.conn_attr);
        EXPECT_EQ("", prepared.tls_warning);
    }
    UseConnAttr({{KEY_SERVER, DSQL_HOST}, {"SSLMODE", "verify-full"}, {"PQOPT", "{sslrootcert=/etc/ssl/corp-ca.pem}"}});
    std::map<std::string, std::string> expected = dbc_.conn_attr;
    DsqlCompatibility::ApplyBaseDriverOptions(expected);
    expected.emplace(KEY_DATABASE, "postgres");
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_EQ(expected, dbc_.conn_attr);
    EXPECT_EQ(0, log.Count("Overriding SSLmode"));
}

// A full mode name in another case is disable to psqlODBC, which reads the first byte case-sensitively. It is
// lowercased rather than replaced, so nothing is reported, and it gets the defaults of a raised mode. libpq rejects
// sslrootcert=system below verify-full, so only VERIFY-FULL gets the root certificate.
TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherAuth_ModeNameInAnotherCase_IsLowercased) {
    const std::string application_name_only = "{application_name=" + APPLICATION_NAME + "}";
    for (const auto& [requested, expected, pqopt] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"Allow", "allow", application_name_only},
             {"PREFER", "prefer", application_name_only},
             {"Require", "require", application_name_only},
             {"VERIFY-CA", "verify-ca", application_name_only},
             {"VERIFY-FULL", "verify-full", VERIFY_FULL_PQOPT},}) {
        SCOPED_TRACE(requested);
        UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}, {"SSLMODE", requested}});
        const DsqlCompatibility::PreparedConnect prepared = DsqlCompatibility::PrepareConnect(&dbc_);
        EXPECT_EQ(expected, dbc_.conn_attr.at("SSLMODE"));
        EXPECT_EQ(pqopt, dbc_.conn_attr.at("PQOPT"));
        EXPECT_EQ("postgres", dbc_.conn_attr.at(KEY_DATABASE));
        EXPECT_EQ("", prepared.tls_warning);
    }
}

// RDS_AUTH_TYPE=DSQL (any case) applied its own, stricter defaults in DsqlAuthPlugin before the chain reached
// DefaultPlugin: only the forced psqlODBC options change here.
TEST_F(DsqlCompatibilityTest, PrepareConnect_DsqlAuth_LeavesTlsToTheAuthPlugin) {
    for (const std::string auth_type : {VALUE_AUTH_DSQL, "dsql"}) {
        for (const std::string mode : {"require", "disable"}) {
            SCOPED_TRACE(testing::Message() << auth_type << " " << mode);
            UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, auth_type}, {"SSLMODE", mode}});
            std::map<std::string, std::string> expected = dbc_.conn_attr;
            DsqlCompatibility::ApplyBaseDriverOptions(expected);
            const DsqlCompatibility::PreparedConnect prepared = DsqlCompatibility::PrepareConnect(&dbc_);
            EXPECT_EQ(expected, dbc_.conn_attr);
            EXPECT_EQ("", prepared.tls_warning);
        }
    }
}

// DATABASE_DIALECT=AURORA_DSQL makes a connect to an IP address (a PrivateLink endpoint, say) an Aurora DSQL one. With
// no SSLmode it gets verify-full, as any Aurora DSQL target, which fails on the host name rather than in plaintext; the
// verify-ca the documentation gives for an IP address stays.
TEST_F(DsqlCompatibilityTest, PrepareConnect_DialectWithIp_GetsVerifyFull) {
    UseConnAttr({{KEY_SERVER, "10.0.0.1"}, {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_DSQL}});
    ASSERT_TRUE(DsqlCompatibility::IsDsql(&dbc_));
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_EQ("verify-full", dbc_.conn_attr.at("SSLMODE"));
    EXPECT_EQ(VERIFY_FULL_PQOPT, dbc_.conn_attr.at("PQOPT"));

    UseConnAttr(
        {{KEY_SERVER, "10.0.0.1"}, {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_DSQL}, {"SSLMODE", "verify-ca"}});
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_EQ("verify-ca", dbc_.conn_attr.at("SSLMODE"));
    EXPECT_FALSE(dbc_.conn_attr.contains("PQOPT"));
}

// Off Aurora DSQL nothing changes: psqlODBC's own TLS default and libpq's database default stay.
TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherDatabase_Untouched) {
    for (const std::string& server : {AURORA_HOST, std::string("10.0.0.1")}) {
        for (const auto& [key, mode] : std::vector<std::pair<std::string, std::string>>{
                 {"", ""}, {"SSLMODE", "disable"}, {"SSLMODE", "Disable"}, {"CA", "d"},}) {
            SCOPED_TRACE(testing::Message() << server << " " << key << "=" << mode);
            std::map<std::string, std::string> conn_attr = {{KEY_SERVER, server}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}};
            if (!key.empty()) {
                conn_attr.emplace(key, mode);
            }
            UseConnAttr(conn_attr);
            ASSERT_FALSE(DsqlCompatibility::IsDsql(&dbc_));
            const std::map<std::string, std::string> before = dbc_.conn_attr;
            const DsqlCompatibility::PreparedConnect prepared = DsqlCompatibility::PrepareConnect(&dbc_);
            EXPECT_EQ(before, dbc_.conn_attr);
            EXPECT_EQ("", prepared.tls_warning);
        }
    }
}

#ifndef _WIN32
// The root certificate is the Aurora DSQL plugin's choice for the base driver's libpq: before 16, libpq reads
// sslrootcert=system as a file in the working directory. (On Windows the version comes from libpq.dll, not the loader.)
TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherAuth_RootCertFollowsTheBaseLibpqVersion) {
    for (const auto& [version, expect_system] : std::map<int, bool>{{150004, false}, {160004, true}}) {
        base_libpq_version = version;
        UseServer(DSQL_HOST);
        DsqlCompatibility::PrepareConnect(&dbc_);
        const std::string pqopt = dbc_.conn_attr.at("PQOPT");
        // Before libpq 16 this holds with a system CA bundle or without one.
        EXPECT_EQ(expect_system, pqopt.find("sslrootcert=system") != std::string::npos) << version << " " << pqopt;
    }
}
#endif

// A connect that succeeds reports the replaced mode, as RDS_AUTH_TYPE=DSQL does: SQL_SUCCESS_WITH_INFO and 01S02. The
// base driver got verify-full and the database.
TEST_F(DsqlCompatibilityTest, DefaultPluginConnect_DsqlTargetDisable_Returns01S02) {
    UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}, {"SSLMODE", "disable"}});
    env_.wrapped_env = static_cast<SQLHENV>(&env_);
    driver_connect_result = SQL_SUCCESS;
    DefaultPlugin plugin(&dbc_);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, plugin.Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);
    EXPECT_EQ(WARN_OPTION_VALUE_CHANGED, dbc_.err->native_err);
    EXPECT_EQ(SslModeChanged("disable"), dbc_.err->error_msg);
    EXPECT_NE(std::string::npos, captured_conn_str.find("SSLMODE=verify-full")) << captured_conn_str;
    EXPECT_NE(std::string::npos, captured_conn_str.find("DATABASE=postgres")) << captured_conn_str;
}

TEST_F(DsqlCompatibilityTest, DefaultPluginConnect_DsqlTargetUnsetSslMode_NoWarning) {
    UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}});
    env_.wrapped_env = static_cast<SQLHENV>(&env_);
    driver_connect_result = SQL_SUCCESS;
    DefaultPlugin plugin(&dbc_);
    EXPECT_EQ(SQL_SUCCESS, plugin.Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_NE(std::string::npos, captured_conn_str.find("SSLMODE=verify-full")) << captured_conn_str;
}

// The text psqlODBC gets is exact: SSLMODE bare (psqlODBC reads its first letter, so a braced mode would run as
// disable) and PQOPT in one pair of braces (psqlODBC removes one pair, and URL-decodes the value otherwise).
TEST_F(DsqlCompatibilityTest, DefaultPluginConnect_DsqlTargetDatabaseAuthUnsetSslMode_BaseDriverGetsExactTlsText) {
    UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}});
    env_.wrapped_env = static_cast<SQLHENV>(&env_);
    DefaultPlugin plugin(&dbc_);
    plugin.Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    const std::string padded = ";" + captured_conn_str + ";";
    EXPECT_NE(std::string::npos, padded.find(";SSLMODE=verify-full;")) << captured_conn_str;
    EXPECT_NE(std::string::npos, padded.find(";PQOPT=" + VERIFY_FULL_PQOPT + ";")) << captured_conn_str;
}

// The wrapper keeps one record per handle, and its record hides the base driver's: a connect that fails, or that the
// base driver warns about, keeps the base driver's result and records.
TEST_F(DsqlCompatibilityTest, DefaultPluginConnect_DsqlTargetDisable_KeepsTheBaseDriversResult) {
    for (const SQLRETURN result : std::initializer_list<SQLRETURN>{SQL_SUCCESS_WITH_INFO, SQL_ERROR}) {
        SCOPED_TRACE(result);
        UseConnAttr({{KEY_SERVER, DSQL_HOST}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}, {"SSLMODE", "disable"}});
        env_.wrapped_env = static_cast<SQLHENV>(&env_);
        driver_connect_result = result;
        DefaultPlugin plugin(&dbc_);
        EXPECT_EQ(result, plugin.Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
        EXPECT_EQ(nullptr, dbc_.err);
        dbc_.wrapped_dbc = SQL_NULL_HDBC;
        dbc_.conn_status = CONN_NOT_CONNECTED;
    }
}

namespace {
    // psqlODBC's own connection attributes for three of the forced options (psqlODBC pgapifunc.h).
    constexpr SQLINTEGER PGOPT_USE_DECLAREFETCH = 65539;
    constexpr SQLINTEGER PGOPT_SERVER_SIDE_PREPARE = 65540;
    constexpr SQLINTEGER PGOPT_IGNORETIMEOUT = 65551;
    const std::string SERVER_SIDE_PREPARE_REASON =
        "freeing a prepared statement inside a transaction can silently roll the transaction back on Aurora DSQL";
}  // namespace

// Set on a live connection, the attribute turns server-side prepare back on for new statements, and the wrapper sets
// it again after every reconnect: the transaction loss would be back.
TEST_F(DsqlCompatibilityTest, SetPgoptServerSidePrepare_Dsql_StaysOff) {
    UseServer(DSQL_HOST);
    Connect();
    CapturingLogSink log;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(1), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);
    EXPECT_EQ("SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE changed to 0: " + SERVER_SIDE_PREPARE_REASON,
              dbc_.err->error_msg);
    EXPECT_EQ(1, log.Count("[DSQL] SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE changed to 0: " + SERVER_SIDE_PREPARE_REASON
                           + " (requested 1)"));
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
    EXPECT_FALSE(dbc_.attr_map.contains(PGOPT_SERVER_SIDE_PREPARE));
}

TEST_F(DsqlCompatibilityTest, SetPgoptAttributes_DsqlForcedValues_SucceedLocally) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(0), 0));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, PGOPT_USE_DECLAREFETCH, AsPointer(0), 0));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, PGOPT_IGNORETIMEOUT, AsPointer(1), 0));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
    EXPECT_TRUE(dbc_.attr_map.empty());
}

TEST_F(DsqlCompatibilityTest, SetPgoptDeclareFetchAndIgnoreTimeout_Dsql_KeepTheForcedValues) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, PGOPT_USE_DECLAREFETCH, AsPointer(1), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("SQL_ATTR_PGOPT_USE_DECLAREFETCH changed to 0: Aurora DSQL doesn't support cursors", dbc_.err->error_msg);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, PGOPT_IGNORETIMEOUT, AsPointer(0), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("SQL_ATTR_PGOPT_IGNORETIMEOUT changed to 1: Aurora DSQL doesn't support statement_timeout",
              dbc_.err->error_msg);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
    EXPECT_TRUE(dbc_.attr_map.empty());
}

TEST_F(DsqlCompatibilityTest, SetPgoptServerSidePrepare_OtherDatabase_ReachesBaseDriver) {
    UseServer(AURORA_HOST);
    Connect();
    RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(1), 0);
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
}

// Before the first connect nothing says DSQL yet: the value waits in attr_map for PrepareConnect.
TEST_F(DsqlCompatibilityTest, SetPgoptServerSidePrepare_BeforeConnect_IsKept) {
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(1), 0));
    EXPECT_TRUE(dbc_.attr_map.contains(PGOPT_SERVER_SIDE_PREPARE));
}

// DefaultPlugin::Connect would set a value from attr_map again after psqlODBC connects, over USESERVERSIDEPREPARE=0.
TEST_F(DsqlCompatibilityTest, PrepareConnect_Dsql_DropsPgoptAttributesSetBeforeConnect) {
    UseServer(DSQL_HOST);
    CapturingLogSink log;
    dbc_.attr_map.insert_or_assign(PGOPT_SERVER_SIDE_PREPARE, std::make_pair(AsPointer(1), 0));
    dbc_.attr_map.insert_or_assign(PGOPT_USE_DECLAREFETCH, std::make_pair(AsPointer(0), 0));
    dbc_.attr_map.insert_or_assign(PGOPT_IGNORETIMEOUT, std::make_pair(AsPointer(1), 0));
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_TRUE(dbc_.attr_map.empty());
    EXPECT_EQ(1, log.Count("[DSQL] Ignoring SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE=1 set before connect: "
                           + SERVER_SIDE_PREPARE_REASON));
    EXPECT_EQ(1, log.Count("[DSQL] Ignoring "));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherDatabase_KeepsPgoptAttributes) {
    UseServer(AURORA_HOST);
    dbc_.attr_map.insert_or_assign(PGOPT_SERVER_SIDE_PREPARE, std::make_pair(AsPointer(1), 0));
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_TRUE(dbc_.attr_map.contains(PGOPT_SERVER_SIDE_PREPARE));
}

// Between SQLDisconnect and the next connect the handle answers as its last connect, Aurora DSQL, so the value is
// answered as on an open connection and never reaches attr_map, which the next connect would set again.
TEST_F(DsqlCompatibilityTest, SetPgoptServerSidePrepare_DsqlHandleBetweenConnects_IsNotKept) {
    UseServer(DSQL_HOST);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(1), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
    EXPECT_FALSE(dbc_.attr_map.contains(PGOPT_SERVER_SIDE_PREPARE));
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_SetEveryOption) {
    std::map<std::string, std::string> conn_attr;
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    const std::map<std::string, std::string> expected = {
        {"IGNORETIMEOUT", "1"},   {"PROTOCOL", "7.4-0"},        {"UPDATABLECURSORS", "0"},
        {"USEDECLAREFETCH", "0"}, {"USESERVERSIDEPREPARE", "0"}};
    EXPECT_EQ(expected, conn_attr);
}

// No "-<level>" means the default level, which sends SAVEPOINT; levels 1 and 2 send it too.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_OverrideUserProtocol) {
    for (const char* protocol : {"7.4", "7.4-2", "7.4-1"}) {
        std::map<std::string, std::string> conn_attr = {{"PROTOCOL", protocol}};
        DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
        EXPECT_EQ("7.4-0", conn_attr.at("PROTOCOL")) << protocol;
    }
    std::map<std::string, std::string> short_form = {{"A1", "7.4-1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("A1"));
    EXPECT_EQ("7.4-0", short_form.at("PROTOCOL"));
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_OverrideUserIgnoreTimeout) {
    std::map<std::string, std::string> long_form = {{"IGNORETIMEOUT", "0"}};
    DsqlCompatibility::ApplyBaseDriverOptions(long_form);
    EXPECT_EQ("1", long_form.at("IGNORETIMEOUT"));

    std::map<std::string, std::string> short_form = {{"D9", "0"}};
    DsqlCompatibility::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("D9"));
    EXPECT_EQ("1", short_form.at("IGNORETIMEOUT"));
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_OverrideUserDeclareFetch) {
    std::map<std::string, std::string> long_form = {{"USEDECLAREFETCH", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(long_form);
    EXPECT_EQ("0", long_form.at("USEDECLAREFETCH"));

    std::map<std::string, std::string> short_form = {{"B6", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("B6"));
    EXPECT_EQ("0", short_form.at("USEDECLAREFETCH"));
}

// What psqlODBC gets: each option once, under its long keyword, after the CX flags.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_BaseDriverStringHasOnlyLongKeysAfterCx) {
    std::map<std::string, std::string> conn_attr = {
        {"A1", "7.4-2"}, {"D9", "0"}, {"B6", "1"}, {"C8", "1"}, {"C4", "1"}, {"CX", "1c2305bab"}};
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    EXPECT_EQ("CX=1c2305bab;IGNORETIMEOUT=1;PROTOCOL=7.4-0;UPDATABLECURSORS=0;"
              "USEDECLAREFETCH=0;USESERVERSIDEPREPARE=0",
              ConnectionStringHelper::BuildMinimumConnectionString(conn_attr));
}

// UpdatableCursors=1 is psqlODBC's default; it makes keyset-driven and updatable cursors select ctid.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_TurnUpdatableCursorsOff) {
    CapturingLogSink log;
    std::map<std::string, std::string> long_form = {{"UPDATABLECURSORS", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(long_form);
    EXPECT_EQ("0", long_form.at("UPDATABLECURSORS"));

    std::map<std::string, std::string> short_form = {{"C4", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(short_form);
    EXPECT_FALSE(short_form.contains("C4"));
    EXPECT_EQ("0", short_form.at("UPDATABLECURSORS"));
    EXPECT_EQ(2, log.Count("[DSQL] Overriding UpdatableCursors=1 with 0: Aurora DSQL doesn't return system columns "
                           "such as ctid, which psqlODBC's keyset-driven and updatable cursors use"));

    std::map<std::string, std::string> already_off = {{"UPDATABLECURSORS", "0"}};
    DsqlCompatibility::ApplyBaseDriverOptions(already_off);
    EXPECT_EQ(0, log.Count("[DSQL] Overriding UpdatableCursors=0"));
}

TEST_F(DsqlCompatibilityTest, BaseDriverOptions_LogEachOverride) {
    CapturingLogSink log;
    std::map<std::string, std::string> user_values = {{"PROTOCOL", "7.4"}, {"IGNORETIMEOUT", "0"}, {"B6", "1"}};
    DsqlCompatibility::ApplyBaseDriverOptions(user_values);
    std::map<std::string, std::string> already_right = {
        {"PROTOCOL", "7.4-0"}, {"IGNORETIMEOUT", "1"}, {"USEDECLAREFETCH", "0"}};
    DsqlCompatibility::ApplyBaseDriverOptions(already_right);
    EXPECT_EQ(1, log.Count("[DSQL] Overriding Protocol=7.4 with 7.4-0: Aurora DSQL doesn't support savepoints"));
    EXPECT_EQ(1, log.Count("[DSQL] Overriding IgnoreTimeout=0 with 1: Aurora DSQL doesn't support statement_timeout"));
    EXPECT_EQ(1, log.Count("[DSQL] Overriding UseDeclareFetch=1 with 0: Aurora DSQL doesn't support cursors"));
    EXPECT_EQ(3, log.Count("[DSQL] Overriding "));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_Dsql_SetsEveryOption) {
    UseServer(DSQL_HOST);
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_EQ("7.4-0", dbc_.conn_attr.at("PROTOCOL"));
    EXPECT_EQ("1", dbc_.conn_attr.at("IGNORETIMEOUT"));
    EXPECT_EQ("0", dbc_.conn_attr.at("USEDECLAREFETCH"));
    EXPECT_EQ("0", dbc_.conn_attr.at("USESERVERSIDEPREPARE"));
    EXPECT_EQ("0", dbc_.conn_attr.at("UPDATABLECURSORS"));
}

// A connection string as users write it: psqlODBC's mixed-case keywords and its short forms. The wrapper's parser
// upper-cases every key, so each form is replaced.
TEST_F(DsqlCompatibilityTest, BaseDriverOptions_ReplaceOptionsAsUsersWriteThem) {
    std::map<std::string, std::string> conn_attr;
    ConnectionStringHelper::ParseConnectionString("UseServerSidePrepare=1;Protocol=7.4;usedeclarefetch=1;D9=0;", conn_attr);
    DsqlCompatibility::ApplyBaseDriverOptions(conn_attr);
    EXPECT_EQ("IGNORETIMEOUT=1;PROTOCOL=7.4-0;UPDATABLECURSORS=0;USEDECLAREFETCH=0;USESERVERSIDEPREPARE=0",
              ConnectionStringHelper::BuildMinimumConnectionString(conn_attr));
}

TEST_F(DsqlCompatibilityTest, SetQueryTimeout_Dsql_ReturnsOptionValueChanged) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(30), 0));
    ASSERT_NE(nullptr, stmt_.err);
    EXPECT_EQ("01S02", stmt_.err->sqlstate);
    EXPECT_EQ("SQL_ATTR_QUERY_TIMEOUT changed to 0: Aurora DSQL doesn't support query timeouts (it limits each "
              "transaction to 5 minutes)", stmt_.err->error_msg);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetStmtAttr));
    EXPECT_FALSE(stmt_.attr_map.contains(SQL_ATTR_QUERY_TIMEOUT));
}

TEST_F(DsqlCompatibilityTest, SetQueryTimeout_DsqlZero_ReachesBaseDriver) {
    UseServer(DSQL_HOST);
    Connect();
    RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(0), 0);
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::SetStmtAttr));
}

TEST_F(DsqlCompatibilityTest, SetQueryTimeout_OtherDatabase_ReachesBaseDriver) {
    UseServer(AURORA_HOST);
    Connect();
    RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(30), 0);
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::SetStmtAttr));
}

// SQLSetStmtAttr holds only the statement's lock, and failover rewrites SERVER in conn_attr from another thread's
// statement. The hook reads the connect's answer, not conn_attr, even when conn_attr now names an Aurora DSQL endpoint.
TEST_F(DsqlCompatibilityTest, SetQueryTimeout_ConnAttrRewrittenAfterConnect_KeepsTheConnectsAnswer) {
    UseServer(AURORA_HOST);
    Connect();
    dbc_.conn_attr.insert_or_assign(KEY_SERVER, DSQL_HOST);
    EXPECT_FALSE(DsqlCompatibility::IsDsql(&dbc_));
    EXPECT_NE(SQL_SUCCESS_WITH_INFO, RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(30), 0));
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::SetStmtAttr));
    EXPECT_TRUE(stmt_.attr_map.contains(SQL_ATTR_QUERY_TIMEOUT));
}

// A handle whose first connect was Aurora DSQL, connected to another database, forwards the timeout.
TEST_F(DsqlCompatibilityTest, SetQueryTimeout_DsqlHandleReusedForOtherDatabase_ReachesBaseDriver) {
    UseServer(DSQL_HOST);
    Connect();
    ASSERT_TRUE(DsqlCompatibility::IsDsql(&dbc_));
    UseServer(AURORA_HOST);
    RDS_SQLSetStmtAttr(&stmt_, SQL_ATTR_QUERY_TIMEOUT, AsPointer(30), 0);
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::SetStmtAttr));
}

// ODBC 2 sets the timeout for later statements on the connection.
TEST_F(DsqlCompatibilityTest, SetConnectQueryTimeout_Dsql_ReturnsOptionValueChanged) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectOption(&dbc_, SQL_QUERY_TIMEOUT, 30));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
    EXPECT_FALSE(dbc_.attr_map.contains(SQL_QUERY_TIMEOUT));
}

TEST_F(DsqlCompatibilityTest, SetConnectAttr_Dsql_OtherAttributesReachBaseDriver) {
    UseServer(DSQL_HOST);
    Connect();
    RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_OFF), 0);
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
    EXPECT_FALSE(dbc_.auto_commit);
}

TEST_F(DsqlCompatibilityTest, SetTxnIsolation_Dsql_RepeatableReadSucceedsLocally) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(SQL_TXN_REPEATABLE_READ), 0));
    EXPECT_EQ(nullptr, dbc_.err);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
    EXPECT_FALSE(dbc_.attr_map.contains(SQL_ATTR_TXN_ISOLATION));
}

// A higher level than requested, which ODBC lets a driver substitute.
TEST_F(DsqlCompatibilityTest, SetTxnIsolation_Dsql_LowerLevelsReturnOptionValueChanged) {
    UseServer(DSQL_HOST);
    Connect();
    for (const SQLULEN level : std::initializer_list<SQLULEN>{SQL_TXN_READ_UNCOMMITTED, SQL_TXN_READ_COMMITTED}) {
        EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(level), 0))
            << level;
        ASSERT_NE(nullptr, dbc_.err) << level;
        EXPECT_EQ("01S02", dbc_.err->sqlstate) << level;
        EXPECT_EQ("SQL_ATTR_TXN_ISOLATION changed to SQL_TXN_REPEATABLE_READ: Aurora DSQL runs every transaction at "
                  "REPEATABLE READ", dbc_.err->error_msg) << level;
    }
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
    EXPECT_FALSE(dbc_.attr_map.contains(SQL_ATTR_TXN_ISOLATION));
}

// REPEATABLE READ is lower than SERIALIZABLE (it allows write skew), which ODBC doesn't let a driver substitute.
TEST_F(DsqlCompatibilityTest, SetTxnIsolation_Dsql_SerializableIsHy024) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_ERROR, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(SQL_TXN_SERIALIZABLE), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("HY024", dbc_.err->sqlstate);
    EXPECT_EQ("SQL_TXN_SERIALIZABLE isn't supported: Aurora DSQL runs every transaction at REPEATABLE READ "
              "(snapshot isolation)", dbc_.err->error_msg);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
}

TEST_F(DsqlCompatibilityTest, SetTxnIsolation_Dsql_InvalidLevelIsHy024) {
    UseServer(DSQL_HOST);
    Connect();
    const SQLULEN not_a_level = 3;
    EXPECT_EQ(SQL_ERROR, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(not_a_level), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("HY024", dbc_.err->sqlstate);
    EXPECT_EQ("Invalid SQL_ATTR_TXN_ISOLATION value 3", dbc_.err->error_msg);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
}

// Before the first connect nothing says DSQL yet: the value waits in attr_map for PrepareConnect.
TEST_F(DsqlCompatibilityTest, SetTxnIsolation_BeforeConnect_IsKept) {
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(SQL_TXN_SERIALIZABLE), 0));
    EXPECT_TRUE(dbc_.attr_map.contains(SQL_ATTR_TXN_ISOLATION));
}

TEST_F(DsqlCompatibilityTest, SetTxnIsolation_OtherDatabase_ReachesBaseDriver) {
    UseServer(AURORA_HOST);
    Connect();
    RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(SQL_TXN_SERIALIZABLE), 0);
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
}

TEST_F(DsqlCompatibilityTest, SetConnectOption_Dsql_TxnIsolationIsAnsweredLocally) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RDS_SQLSetConnectOption(&dbc_, SQL_TXN_ISOLATION, SQL_TXN_READ_COMMITTED));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("01S02", dbc_.err->sqlstate);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::SetConnectAttr));
}

TEST_F(DsqlCompatibilityTest, GetTxnIsolation_Dsql_IsRepeatableRead) {
    UseServer(DSQL_HOST);
    Connect();
    SQLUINTEGER level = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, &level, 0, nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_TXN_REPEATABLE_READ), level);
    SQLUINTEGER option_level = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectOption(&dbc_, SQL_TXN_ISOLATION, &option_level));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_TXN_REPEATABLE_READ), option_level);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::GetConnectAttr));
}

TEST_F(DsqlCompatibilityTest, GetInfo_Dsql_ReportsOnlyRepeatableRead) {
    UseServer(DSQL_HOST);
    Connect();
    for (const SQLUSMALLINT info_type :
         std::initializer_list<SQLUSMALLINT>{SQL_DEFAULT_TXN_ISOLATION, SQL_TXN_ISOLATION_OPTION}) {
        SQLUINTEGER value = 0;
        SQLSMALLINT length = 0;
        EXPECT_EQ(SQL_SUCCESS,
                  RDS_SQLGetInfo(&dbc_, info_type, &value, static_cast<SQLSMALLINT>(sizeof(value)), &length))
            << info_type;
        EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_TXN_REPEATABLE_READ), value) << info_type;
        EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(SQLUINTEGER)), length) << info_type;
    }
    // StringLengthPtr is optional.
    SQLUINTEGER value = 0;
    EXPECT_EQ(SQL_SUCCESS,
              RDS_SQLGetInfo(&dbc_, SQL_TXN_ISOLATION_OPTION, &value, static_cast<SQLSMALLINT>(sizeof(value)), nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_TXN_REPEATABLE_READ), value);
    EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::GetInfo));
}

TEST_F(DsqlCompatibilityTest, GetInfo_OtherDatabase_ReachesBaseDriver) {
    UseServer(AURORA_HOST);
    Connect();
    SQLUINTEGER value = 0;
    RDS_SQLGetInfo(&dbc_, SQL_TXN_ISOLATION_OPTION, &value, static_cast<SQLSMALLINT>(sizeof(value)), nullptr);
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::GetInfo));
}

// ODBC-2: a record psqlODBC left on the connection before the wrapper's own answer isn't this call's.
class DsqlStaleDiagTest : public DsqlCompatibilityTest {
protected:
    // psqlODBC has an earlier record (HY000) on the connection.
    static void LeaveEarlierBaseRecord() { base_sql_state = "HY000"; }

    void ExpectNoBaseRecordReadable() {
        SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
        SQLTCHAR message[MAX_MSG_LEN] = {0};
        SQLINTEGER native_error = 0;
        SQLSMALLINT length = 0;
        EXPECT_EQ(SQL_NO_DATA, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state, &native_error, message, MAX_MSG_LEN,
                                                 &length, false));
        SQLINTEGER count = -1;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagField(SQL_HANDLE_DBC, &dbc_, 0, SQL_DIAG_NUMBER, &count, 0, nullptr));
        EXPECT_EQ(0, count);
        EXPECT_FALSE(base_driver_->Requested(RdsFuncNames::GetDiagRec));
    }
};

TEST_F(DsqlStaleDiagTest, GetInfo_Dsql_LeavesNoEarlierBaseRecord) {
    UseServer(DSQL_HOST);
    Connect();
    LeaveEarlierBaseRecord();
    SQLUINTEGER value = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetInfo(&dbc_, SQL_TXN_ISOLATION_OPTION, &value,
                                          static_cast<SQLSMALLINT>(sizeof(value)), nullptr));
    ExpectNoBaseRecordReadable();
}

TEST_F(DsqlStaleDiagTest, GetTxnIsolation_Dsql_LeavesNoEarlierBaseRecord) {
    UseServer(DSQL_HOST);
    Connect();
    LeaveEarlierBaseRecord();
    SQLUINTEGER level = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, &level, 0, nullptr));
    ExpectNoBaseRecordReadable();
}

TEST_F(DsqlStaleDiagTest, SetTxnIsolation_DsqlRepeatableRead_LeavesNoEarlierBaseRecord) {
    UseServer(DSQL_HOST);
    Connect();
    LeaveEarlierBaseRecord();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_TXN_ISOLATION, AsPointer(SQL_TXN_REPEATABLE_READ), 0));
    ExpectNoBaseRecordReadable();
}

TEST_F(DsqlStaleDiagTest, SetForcedPgopt_DsqlForcedValue_LeavesNoEarlierBaseRecord) {
    UseServer(DSQL_HOST);
    Connect();
    LeaveEarlierBaseRecord();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, PGOPT_SERVER_SIDE_PREPARE, AsPointer(0), 0));
    ExpectNoBaseRecordReadable();
}

// A failed commit before an autocommit change returns the commit's own error, whose records stay readable.
TEST_F(DsqlStaleDiagTest, SetAutocommit_DsqlCommitFails_KeepsTheCommitsBaseRecordReadable) {
    UseServer(DSQL_HOST);
    Connect();
    dbc_.auto_commit = false;
    dbc_.transaction_status = TRANSACTION_OPEN;
    end_tran_result = SQL_ERROR;
    LeaveEarlierBaseRecord();
    EXPECT_EQ(SQL_ERROR, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state, &native_error, message, MAX_MSG_LEN,
                                             &length, false));
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::GetDiagRec));
}

TEST_F(DsqlStaleDiagTest, GetInfo_OtherDatabase_StillReadsTheBaseRecord) {
    UseServer(AURORA_HOST);
    Connect();
    LeaveEarlierBaseRecord();
    SQLUINTEGER value = 0;
    RDS_SQLGetInfo(&dbc_, SQL_TXN_ISOLATION_OPTION, &value, static_cast<SQLSMALLINT>(sizeof(value)), nullptr);
    SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state, &native_error, message, MAX_MSG_LEN,
                                             &length, false));
    EXPECT_TRUE(base_driver_->Requested(RdsFuncNames::GetDiagRec));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_Dsql_DropsIsolationSetBeforeConnect) {
    UseServer(DSQL_HOST);
    CapturingLogSink log;
    dbc_.attr_map.insert_or_assign(SQL_ATTR_TXN_ISOLATION, std::make_pair(AsPointer(SQL_TXN_SERIALIZABLE), 0));
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_FALSE(dbc_.attr_map.contains(SQL_ATTR_TXN_ISOLATION));
    EXPECT_EQ(1, log.Count("[DSQL] Ignoring SQL_ATTR_TXN_ISOLATION=8 set before connect: Aurora DSQL runs every "
                           "transaction at REPEATABLE READ"));
}

TEST_F(DsqlCompatibilityTest, PrepareConnect_OtherDatabase_KeepsIsolationSetBeforeConnect) {
    UseServer(AURORA_HOST);
    dbc_.attr_map.insert_or_assign(SQL_ATTR_TXN_ISOLATION, std::make_pair(AsPointer(SQL_TXN_SERIALIZABLE), 0));
    DsqlCompatibility::PrepareConnect(&dbc_);
    EXPECT_TRUE(dbc_.attr_map.contains(SQL_ATTR_TXN_ISOLATION));
}

namespace {
    // The 25S03 message of an SQLEndTran(SQL_COMMIT) after a failed statement (DsqlCompatibility, BEHAV-2).
    const std::string ROLLED_BACK =
        "The transaction was rolled back, not committed: a statement in it failed, and Aurora DSQL ends a transaction "
        "at its first error (it has no savepoints). Roll back after an error, and run the whole transaction again.";
}  // namespace

// Switching autocommit inside a transaction commits it, and psqlODBC drops that COMMIT's result, while Aurora DSQL
// reports OCC conflicts at COMMIT: the wrapper commits through SQLEndTran first.
TEST_F(DsqlCompatibilityTest, SetAutocommitOn_Dsql_CommitsFirst) {
    UseServer(DSQL_HOST);
    Connect();
    dbc_.auto_commit = false;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    const std::vector<std::string> expected = {
        EndTranCall(SQL_COMMIT),
        SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_ON),
    };
    EXPECT_EQ(expected, base_calls);
    EXPECT_TRUE(dbc_.auto_commit);
}

// An OCC conflict, for example: the app gets the COMMIT's error, read from the base driver's record, and autocommit
// stays off.
TEST_F(DsqlCompatibilityTest, SetAutocommitOn_Dsql_CommitFails_ReturnsItsErrorAndStaysOff) {
    UseServer(DSQL_HOST);
    Connect();
    dbc_.auto_commit = false;
    dbc_.attr_map.insert_or_assign(SQL_ATTR_AUTOCOMMIT, std::make_pair(AsPointer(SQL_AUTOCOMMIT_OFF), 0));
    const auto attr_map = dbc_.attr_map;
    end_tran_result = SQL_ERROR;
    base_sql_state = "40001";
    EXPECT_EQ(SQL_ERROR, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_EQ(std::vector<std::string>{EndTranCall(SQL_COMMIT)}, base_calls);
    EXPECT_FALSE(dbc_.auto_commit);
    EXPECT_EQ(attr_map, dbc_.attr_map);
    EXPECT_EQ(nullptr, dbc_.err);
    SQLTCHAR state[MAX_SQL_STATE_LEN * 2] = {0};
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc_, 1, state, &native_error, message, MAX_MSG_LEN,
                                             &length, false));
    EXPECT_EQ("40001", AsciiText(state));
}

// Aurora DSQL ended the transaction at a failed statement: the commit before the switch rolls it back and returns
// 25S03, and autocommit stays off.
TEST_F(DsqlCompatibilityTest, SetAutocommitOn_Dsql_AfterAFailedStatement_Returns25S03) {
    UseServer(DSQL_HOST);
    Connect();
    dbc_.auto_commit = false;
    dbc_.transaction_status = TRANSACTION_OPEN;
    dbc_.dsql_statement_failed_in_transaction = true;
    exec_direct_result = SQL_ERROR;
    base_sql_state = "25P02";
    CapturingLogSink log;
    EXPECT_EQ(SQL_ERROR, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("25S03", dbc_.err->sqlstate);
    EXPECT_EQ(ROLLED_BACK, dbc_.err->error_msg);
    EXPECT_EQ(1, log.Count("[DSQL] " + ROLLED_BACK + " (the check before the commit got 25P02)"));
    const std::vector<std::string> expected = {ExecDirectCall("SELECT 1"), EndTranCall(SQL_ROLLBACK)};
    EXPECT_EQ(expected, base_calls);
    EXPECT_FALSE(dbc_.auto_commit);
    EXPECT_FALSE(dbc_.attr_map.contains(SQL_ATTR_AUTOCOMMIT));
    EXPECT_FALSE(dbc_.dsql_statement_failed_in_transaction);
}

// psqlODBC commits a transaction opened with BEGIN as SQL text on this switch too.
TEST_F(DsqlCompatibilityTest, SetAutocommitOff_Dsql_CommitsFirst) {
    UseServer(DSQL_HOST);
    Connect();
    dbc_.transaction_status = TRANSACTION_OPEN;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_OFF), 0));
    const std::vector<std::string> expected = {
        EndTranCall(SQL_COMMIT),
        SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF),
    };
    EXPECT_EQ(expected, base_calls);
    EXPECT_FALSE(dbc_.auto_commit);
}

// psqlODBC changes nothing then, and an open transaction stays open.
TEST_F(DsqlCompatibilityTest, SetAutocommit_Unchanged_DoesNotCommit) {
    UseServer(DSQL_HOST);
    Connect();
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    dbc_.auto_commit = false;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_OFF), 0));
    const std::vector<std::string> expected = {
        SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_ON),
        SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF),
    };
    EXPECT_EQ(expected, base_calls);
}

// psqlODBC rejects the value and leaves the mode, so nothing is committed, a transaction opened with BEGIN as SQL text
// in autocommit mode included. Not a mode, the value is no change from either.
TEST_F(DsqlCompatibilityTest, SetAutocommit_InvalidValue_DoesNotCommit) {
    UseServer(DSQL_HOST);
    Connect();
    const SQLULEN not_a_mode = 2;
    for (const bool auto_commit : {true, false}) {
        SCOPED_TRACE(auto_commit ? "autocommit on" : "autocommit off");
        dbc_.auto_commit = auto_commit;
        dbc_.transaction_status = TRANSACTION_OPEN;
        base_calls.clear();
        RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(not_a_mode), 0);
        EXPECT_EQ(std::vector<std::string>{SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, not_a_mode)}, base_calls);
    }
}

TEST_F(DsqlCompatibilityTest, SetAutocommitOn_OtherDatabase_DoesNotCommit) {
    UseServer(AURORA_HOST);
    Connect();
    dbc_.auto_commit = false;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_EQ(std::vector<std::string>{SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_ON)}, base_calls);
    EXPECT_TRUE(dbc_.auto_commit);
}

// Nothing is open to commit before a connect, nor between connects on a handle whose last connect was Aurora DSQL:
// without a base connection the value waits in attr_map, and on one that isn't connected it is psqlODBC's. Each part
// has only that clause against the commit: the status says connected in the first, the base connection is there in
// the second.
TEST_F(DsqlCompatibilityTest, SetAutocommitOn_BeforeConnect_DoesNotCommit) {
    UseServer(DSQL_HOST);
    dbc_.conn_status = CONN_CONNECTED;
    dbc_.auto_commit = false;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_TRUE(base_calls.empty());
    EXPECT_TRUE(dbc_.attr_map.contains(SQL_ATTR_AUTOCOMMIT));
    EXPECT_TRUE(dbc_.auto_commit);

    dbc_.wrapped_dbc = static_cast<SQLHDBC>(&dbc_);
    dbc_.conn_status = CONN_NOT_CONNECTED;
    dbc_.auto_commit = false;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, AsPointer(SQL_AUTOCOMMIT_ON), 0));
    EXPECT_EQ(std::vector<std::string>{SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_ON)}, base_calls);
}

// ODBC 2 switches autocommit this way.
TEST_F(DsqlCompatibilityTest, SetConnectOptionAutocommitOn_Dsql_CommitsFirst) {
    UseServer(DSQL_HOST);
    Connect();
    dbc_.auto_commit = false;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLSetConnectOption(&dbc_, SQL_AUTOCOMMIT, SQL_AUTOCOMMIT_ON));
    const std::vector<std::string> expected = {
        EndTranCall(SQL_COMMIT),
        SetConnectAttrCall(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_ON),
    };
    EXPECT_EQ(expected, base_calls);
    EXPECT_TRUE(dbc_.auto_commit);
}
