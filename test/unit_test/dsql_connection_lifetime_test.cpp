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
#include <chrono>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../driver/driver.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/util/attribute_validator.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/dsql_connection_lifetime.h"
#include "../../driver/util/dsql_connection_validator.h"
#include "../../driver/util/logger_wrapper.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string AURORA_HOST = "database-test-name.cluster-XYZ.us-east-2.rds.amazonaws.com";
    // Behind a PrivateLink endpoint without private DNS, SERVER is the endpoint's address.
    const std::string PRIVATELINK_ADDRESS = "10.0.0.5";
    const std::string MISSING_DRIVER = "/nonexistent/path/no-such-driver.so";
    const std::string LIFETIME_NEEDS_DSQL =
        "MAX_CONNECTION_LIFETIME=1800 works only with Aurora DSQL. Use an Aurora DSQL endpoint as SERVER, or set "
        "RDS_AUTH_TYPE=DSQL or DATABASE_DIALECT=AURORA_DSQL; otherwise set MAX_CONNECTION_LIFETIME=0.";

    // The attribute validator's entry for a bad value.
    std::string InvalidEntry(const std::string& value) {
        return "MAX_CONNECTION_LIFETIME=" + value + " (use 0 for off, or an integer from "
            + std::to_string(DsqlConnectionLifetime::MIN_MAX_LIFETIME.count()) + " to "
            + std::to_string(DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME.count()) + ")";
    }
}  // namespace

// The Aurora DSQL connectors' 55 minutes.
TEST(DsqlConnectionLifetimeTest, ParseMaxLifetime_UnsetIsTheDefault) {
    EXPECT_EQ(3300, DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME.count());
    EXPECT_EQ(120, DsqlConnectionLifetime::MIN_MAX_LIFETIME.count());
    std::string error;
    const std::optional<std::chrono::seconds> lifetime = DsqlConnectionLifetime::ParseMaxLifetime({}, error);
    ASSERT_TRUE(lifetime.has_value());
    EXPECT_EQ(DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME.count(), lifetime->count());
    EXPECT_TRUE(error.empty());
}

TEST(DsqlConnectionLifetimeTest, ParseMaxLifetime_AcceptsOffAndTheRange) {
    const std::chrono::seconds::rep max = DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME.count();
    const std::vector<std::pair<std::string, std::chrono::seconds::rep>> cases = {
        {"0", 0}, {"120", 120}, {"1800", 1800}, {" +600 ", 600}, {std::to_string(max), max}};
    for (const auto& [value, seconds] : cases) {
        std::string error;
        const std::optional<std::chrono::seconds> lifetime =
            DsqlConnectionLifetime::ParseMaxLifetime({{KEY_MAX_CONNECTION_LIFETIME, value}}, error);
        ASSERT_TRUE(lifetime.has_value()) << error;
        EXPECT_EQ(seconds, lifetime->count()) << value;
    }
}

TEST(DsqlConnectionLifetimeTest, ParseMaxLifetime_RejectsBadValuesWithWhatToUse) {
    const std::vector<std::string> values = {
        "119", std::to_string(DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME.count() + 1), "3600", "-1", "1.5", "55m",
        "", "off"};
    for (const std::string& value : values) {
        std::string error;
        EXPECT_FALSE(DsqlConnectionLifetime::ParseMaxLifetime({{KEY_MAX_CONNECTION_LIFETIME, value}}, error)
                         .has_value()) << value;
        EXPECT_EQ(InvalidEntry(value), error);
    }
}

TEST(DsqlConnectionLifetimeTest, ValidateMap_ReportsTheBadLifetime) {
    EXPECT_EQ((std::unordered_set<std::string>{InvalidEntry("3600")}),
              AttributeValidator::ValidateMap({{KEY_MAX_CONNECTION_LIFETIME, "3600"}, {KEY_PORT, "5432"}}));
    EXPECT_TRUE(AttributeValidator::ValidateMap({{KEY_MAX_CONNECTION_LIFETIME, "0"}}).empty());
    EXPECT_TRUE(AttributeValidator::ValidateMap({{KEY_MAX_CONNECTION_LIFETIME, "600"}}).empty());
}

// Checked at every connect, on any database, before the base driver loads.
TEST(DsqlConnectionLifetimeTest, InitializeConnection_BadLifetimeFailsTheConnectWith01S00) {
    ENV env;
    DBC dbc{};
    dbc.env = &env;
    dbc.conn_attr = {{KEY_SERVER, AURORA_HOST}, {KEY_MAX_CONNECTION_LIFETIME, "10"}, {KEY_BASE_DRIVER, MISSING_DRIVER}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(&dbc, ""));

    ASSERT_TRUE(dbc.err);
    EXPECT_STREQ("01S00", dbc.err->sqlstate);
    EXPECT_NE(nullptr, std::strstr(dbc.err->error_msg, InvalidEntry("10").c_str()));
}

TEST(DsqlConnectionLifetimeTest, Age_CountsFromTheConnectUntilCleared) {
    std::chrono::steady_clock::time_point now{};
    DsqlConnectionLifetime lifetime([&now] { return now; });
    EXPECT_FALSE(lifetime.Age().has_value());

    lifetime.Start(lifetime.Now(), DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME);
    now += std::chrono::seconds(90);
    const std::optional<std::chrono::steady_clock::duration> age = lifetime.Age();
    ASSERT_TRUE(age.has_value());
    EXPECT_EQ(90, std::chrono::duration_cast<std::chrono::seconds>(age.value()).count());

    lifetime.Clear();
    EXPECT_FALSE(lifetime.Age().has_value());
}

TEST(DsqlConnectionLifetimeTest, DefaultClock_IsTheSteadyClock) {
    const DsqlConnectionLifetime lifetime;
    const std::chrono::steady_clock::time_point before = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point now = lifetime.Now();
    EXPECT_LE(before, now);
    EXPECT_LE(now, std::chrono::steady_clock::now());
}

TEST(DsqlConnectionLifetimeTest, CheckSettings_RejectsALifetimeOffAuroraDsql) {
    const std::vector<std::map<std::string, std::string>> cases = {
        {{KEY_SERVER, AURORA_HOST}, {KEY_MAX_CONNECTION_LIFETIME, "1800"}},
        {{KEY_MAX_CONNECTION_LIFETIME, "1800"}},
        // DATABASE auth behind PrivateLink without the dialect isn't Aurora DSQL to the driver.
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_AUTH_TYPE, VALUE_AUTH_DATABASE}, {KEY_MAX_CONNECTION_LIFETIME, "1800"}},
    };
    for (const auto& conn_attr : cases) {
        DBC dbc{};
        dbc.conn_attr = conn_attr;
        EXPECT_FALSE(DsqlConnectionValidator::CheckSettings(&dbc));
        ASSERT_TRUE(dbc.err);
        EXPECT_STREQ("08001", dbc.err->sqlstate);
        EXPECT_EQ(LIFETIME_NEEDS_DSQL, std::string(dbc.err->error_msg));
    }
}

TEST(DsqlConnectionLifetimeTest, CheckSettings_AllowsOffElsewhereAndAnyValueOnAuroraDsql) {
    const std::vector<std::map<std::string, std::string>> cases = {
        {{KEY_SERVER, AURORA_HOST}, {KEY_MAX_CONNECTION_LIFETIME, "0"}},
        {{KEY_SERVER, AURORA_HOST}},
        {{KEY_SERVER, DSQL_HOST}, {KEY_MAX_CONNECTION_LIFETIME, "1800"}},
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_AUTH_TYPE, VALUE_AUTH_DSQL}, {KEY_MAX_CONNECTION_LIFETIME, "1800"}},
        {{KEY_SERVER, PRIVATELINK_ADDRESS}, {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_DSQL},
         {KEY_MAX_CONNECTION_LIFETIME, "1800"}},
    };
    for (const auto& conn_attr : cases) {
        DBC dbc{};
        dbc.conn_attr = conn_attr;
        EXPECT_TRUE(DsqlConnectionValidator::CheckSettings(&dbc));
        EXPECT_FALSE(dbc.err);
    }
}

// After the attribute validator, before the base driver loads: the missing driver is never reached.
TEST(DsqlConnectionLifetimeTest, InitializeConnection_LifetimeOffAuroraDsql_Fails08001BeforeTheDriverLoad) {
    ENV env;
    DBC dbc{};
    dbc.env = &env;
    dbc.conn_attr = {{KEY_SERVER, AURORA_HOST}, {KEY_MAX_CONNECTION_LIFETIME, "1800"}, {KEY_BASE_DRIVER, MISSING_DRIVER}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(&dbc, ""));

    ASSERT_TRUE(dbc.err);
    EXPECT_STREQ("08001", dbc.err->sqlstate);
    EXPECT_EQ(LIFETIME_NEEDS_DSQL, std::string(dbc.err->error_msg));
    EXPECT_FALSE(env.driver_lib_loader);
}

namespace {
    const std::string RETIRED_LINE = "[DSQL] Retiring the connection after ";
    // No "psql" in it, so the path alone gives no dialect.
    const char* const FAKE_DRIVER_PATH = "/nonexistent/lifetime-test-driver.so";

    // How often the base driver's SQLGetConnectAttr ran, and the attribute it was last asked for.
    int base_get_connect_attr_calls = 0;
    SQLINTEGER base_last_attribute = 0;

    // psqlODBC's answer on an open connection: alive.
    SQLRETURN FakeGetConnectAttr(SQLHDBC /*ConnectionHandle*/, SQLINTEGER attribute, SQLPOINTER value,
                                 SQLINTEGER /*BufferLength*/, SQLINTEGER* /*StringLengthPtr*/) {
        ++base_get_connect_attr_calls;
        base_last_attribute = attribute;
        if (value != nullptr) {
            *static_cast<SQLUINTEGER*>(value) = static_cast<SQLUINTEGER>(SQL_CD_FALSE);
        }
        return SQL_SUCCESS;
    }

    // Answers SQLGetConnectAttr (W in Unicode builds) and nothing else.
    class FakeBaseDriver : public RdsLibLoader {
    public:
        FUNC_HANDLE GetFunction(const std::string& function_name) override {
            return function_name == RDS_STR_SQLGetConnectAttr ? reinterpret_cast<FUNC_HANDLE>(&FakeGetConnectAttr)
                                                              : nullptr;
        }
    };

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

// Through RDS_SQLGetConnectAttr and the ODBC 2 RDS_SQLGetConnectOption, which the SQLGetConnectAttr[W] and
// SQLGetConnectOption[W] entry points call with no other code.
class DsqlConnectionLifetimeAttrTest : public testing::Test {
protected:
    std::shared_ptr<FakeBaseDriver> base_driver_ = std::make_shared<FakeBaseDriver>();
    ENV env_;
    DBC dbc_{};
    std::chrono::steady_clock::time_point now_{};

    void SetUp() override {
        env_.driver_lib_loader = base_driver_;
        dbc_.env = &env_;
        base_get_connect_attr_calls = 0;
        base_last_attribute = 0;
    }

    void TearDown() override {
        // The fake handle points at the test's DBC; never let it outlive the test.
        dbc_.wrapped_dbc = SQL_NULL_HDBC;
        dbc_.plugin_service = nullptr;
        dbc_.dsql_lifetime = nullptr;
    }

    // An open connection to this server whose connect began now, with this MAX_CONNECTION_LIFETIME.
    void Connect(const std::string& server,
                 std::chrono::seconds max_lifetime = DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME) {
        dbc_.conn_attr = {{KEY_SERVER, server}};
        // Unicode builds reach the OdbcHelper through the plugin service when the call goes to the base driver.
        dbc_.plugin_service = std::make_shared<PluginService>(base_driver_, &env_, dbc_.conn_attr, "");
        dbc_.wrapped_dbc = static_cast<SQLHDBC>(&dbc_);
        dbc_.conn_status = CONN_CONNECTED;
        dbc_.dsql_lifetime = std::make_shared<DsqlConnectionLifetime>([this] { return now_; });
        dbc_.dsql_lifetime->Start(now_, max_lifetime);
    }
};

TEST_F(DsqlConnectionLifetimeAttrTest, IsRetired_FromTheLifetimeOn) {
    Connect(DSQL_HOST);
    now_ += DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME - std::chrono::milliseconds(1);
    EXPECT_FALSE(DsqlConnectionLifetime::IsRetired(&dbc_));
    now_ += std::chrono::milliseconds(1);
    EXPECT_TRUE(DsqlConnectionLifetime::IsRetired(&dbc_));
    now_ += std::chrono::minutes(30);
    EXPECT_TRUE(DsqlConnectionLifetime::IsRetired(&dbc_));
    EXPECT_FALSE(DsqlConnectionLifetime::IsRetired(nullptr));
}

TEST_F(DsqlConnectionLifetimeAttrTest, ConnectionDead_UnderTheLifetime_AsksTheBaseDriver) {
    Connect(DSQL_HOST);
    now_ += DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME - std::chrono::seconds(1);
    SQLUINTEGER dead = SQL_CD_TRUE;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_CD_FALSE), dead);
    EXPECT_EQ(1, base_get_connect_attr_calls);
}

// No round trip, as the driver managers require: psqlODBC isn't called, even for a caller that passes no buffer.
TEST_F(DsqlConnectionLifetimeAttrTest, ConnectionDead_AtTheLifetime_IsTrueWithoutTheBaseDriver) {
    Connect(DSQL_HOST);
    now_ += DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME;
    SQLUINTEGER dead = SQL_CD_FALSE;
    SQLINTEGER length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, SQL_IS_UINTEGER, &length));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_CD_TRUE), dead);
    EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(SQLUINTEGER)), length);
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, nullptr, 0, nullptr));
    EXPECT_EQ(0, base_get_connect_attr_calls);
    EXPECT_EQ(nullptr, dbc_.err);
}

// unixODBC's pool reads it into an SQLULEN set to 0, with SQL_IS_INTEGER, and asks on every reuse.
TEST_F(DsqlConnectionLifetimeAttrTest, ConnectionDead_OverTheLifetime_IsTrueAndLogsOnce) {
    Connect(DSQL_HOST);
    now_ += std::chrono::seconds(3400);
    CapturingLogSink sink;
    for (int i = 0; i < 3; ++i) {
        SQLULEN dead = 0;
        EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, SQL_IS_INTEGER, nullptr));
        EXPECT_EQ(static_cast<SQLULEN>(SQL_CD_TRUE), dead);
    }
    EXPECT_EQ(0, base_get_connect_attr_calls);
    EXPECT_EQ(1, sink.Count(RETIRED_LINE));
    EXPECT_EQ(1, sink.Count("[DSQL] Retiring the connection after 3400 seconds (MAX_CONNECTION_LIFETIME="
                           + std::to_string(DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME.count())
                           + "): SQL_ATTR_CONNECTION_DEAD now reports it dead, so a connection pool replaces it."));
}

// ODBC 2 applications, and unixODBC when SQLGetConnectAttr fails, ask through SQLGetConnectOption.
TEST_F(DsqlConnectionLifetimeAttrTest, ConnectionDead_Odbc2GetConnectOption_IsTrue) {
    Connect(DSQL_HOST);
    now_ += DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME;
    SQLULEN dead = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectOption(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead));
    EXPECT_EQ(static_cast<SQLULEN>(SQL_CD_TRUE), dead);
    EXPECT_EQ(0, base_get_connect_attr_calls);
}

// A reconnect on the same handle starts the age again, and its retirement is logged again.
TEST_F(DsqlConnectionLifetimeAttrTest, ConnectionDead_ANewConnectStartsOver) {
    Connect(DSQL_HOST);
    now_ += DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME;
    CapturingLogSink sink;
    EXPECT_TRUE(DsqlConnectionLifetime::IsRetired(&dbc_));

    dbc_.dsql_lifetime->Start(now_, DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME);
    EXPECT_FALSE(DsqlConnectionLifetime::IsRetired(&dbc_));
    now_ += DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME;
    EXPECT_TRUE(DsqlConnectionLifetime::IsRetired(&dbc_));
    EXPECT_EQ(2, sink.Count(RETIRED_LINE));
}

TEST_F(DsqlConnectionLifetimeAttrTest, ConnectionDead_ShorterLifetime_IsTrueFromIt) {
    Connect(DSQL_HOST, std::chrono::seconds(600));
    now_ += std::chrono::seconds(599);
    EXPECT_FALSE(DsqlConnectionLifetime::IsRetired(&dbc_));
    now_ += std::chrono::seconds(1);
    SQLUINTEGER dead = SQL_CD_FALSE;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_CD_TRUE), dead);
}

TEST_F(DsqlConnectionLifetimeAttrTest, ConnectionDead_LifetimeOff_AsksTheBaseDriver) {
    Connect(DSQL_HOST, std::chrono::seconds(0));
    now_ += std::chrono::hours(2);
    SQLUINTEGER dead = SQL_CD_TRUE;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_CD_FALSE), dead);
    EXPECT_EQ(1, base_get_connect_attr_calls);
}

// A handle whose chain came from an Aurora DSQL connect, reconnected to another database, isn't retired.
TEST_F(DsqlConnectionLifetimeAttrTest, ConnectionDead_OtherDatabase_AsksTheBaseDriver) {
    Connect(AURORA_HOST);
    now_ += std::chrono::hours(2);
    SQLUINTEGER dead = SQL_CD_TRUE;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
    EXPECT_EQ(static_cast<SQLUINTEGER>(SQL_CD_FALSE), dead);
    EXPECT_EQ(1, base_get_connect_attr_calls);
}

TEST_F(DsqlConnectionLifetimeAttrTest, ConnectionDead_NoOpenConnection_AsksTheBaseDriver) {
    Connect(DSQL_HOST);
    now_ += std::chrono::hours(2);
    SQLUINTEGER dead = SQL_CD_FALSE;
    // After SQLDisconnect: the wrapped handle is kept, the connection is gone.
    dbc_.conn_status = CONN_NOT_CONNECTED;
    RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr);
    // No age: no connect has succeeded since it was cleared.
    dbc_.conn_status = CONN_CONNECTED;
    dbc_.dsql_lifetime->Clear();
    RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr);
    // No lifetime: the chain has no Aurora DSQL error hint plugin.
    dbc_.dsql_lifetime = nullptr;
    RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr);
    EXPECT_EQ(3, base_get_connect_attr_calls);
}

TEST_F(DsqlConnectionLifetimeAttrTest, OtherAttributes_AreForwarded) {
    Connect(DSQL_HOST);
    now_ += std::chrono::hours(2);
    SQLUINTEGER autocommit = SQL_AUTOCOMMIT_ON;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetConnectAttr(&dbc_, SQL_ATTR_AUTOCOMMIT, &autocommit, 0, nullptr));
    EXPECT_EQ(1, base_get_connect_attr_calls);
    EXPECT_EQ(static_cast<SQLINTEGER>(SQL_ATTR_AUTOCOMMIT), base_last_attribute);
}

// The 60-minute hint asks psqlODBC whether the connection is closed: an old connection that still works isn't.
TEST_F(DsqlConnectionLifetimeAttrTest, IsClosed_StillAsksTheBaseDriver) {
    Connect(DSQL_HOST);
    now_ += std::chrono::hours(2);
    CapturingLogSink sink;
    OdbcHelper odbc_helper(base_driver_, &env_);
    EXPECT_FALSE(odbc_helper.IsClosed(&dbc_));
    EXPECT_EQ(1, base_get_connect_attr_calls);
    EXPECT_EQ(static_cast<SQLINTEGER>(SQL_ATTR_CONNECTION_DEAD), base_last_attribute);
    EXPECT_EQ(0, sink.Count(RETIRED_LINE));
}

// RDS_InitializeConnection without a base driver, as the OCC retry chain tests do: the environment already holds a
// loader for the same path, which doesn't load, and a base environment handle.
class DsqlConnectionLifetimeChainTest : public testing::Test {
protected:
    ENV* env_ = nullptr;
    DBC* dbc_ = nullptr;
    int base_env_stand_in_ = 0;

    void SetUp() override {
        env_ = new ENV();
        env_->driver_lib_loader = std::make_shared<RdsLibLoader>(FAKE_DRIVER_PATH);
        env_->wrapped_env = static_cast<SQLHENV>(&base_env_stand_in_);
        dbc_ = new DBC();
        dbc_->env = env_;
        dbc_->conn_attr = {{KEY_BASE_DRIVER, FAKE_DRIVER_PATH}};
    }

    void TearDown() override {
        delete dbc_;
        delete env_;
    }
};

TEST_F(DsqlConnectionLifetimeChainTest, AuroraDsqlChain_HasALifetimeWithNoAgeBeforeTheConnect) {
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, DSQL_HOST);
    ASSERT_EQ(SQL_SUCCESS, RDS_InitializeConnection(dbc_, ""));
    ASSERT_TRUE(dbc_->dsql_lifetime != nullptr);
    EXPECT_FALSE(dbc_->dsql_lifetime->Age().has_value());
}

TEST_F(DsqlConnectionLifetimeChainTest, OtherChain_HasNoLifetime) {
    dbc_->conn_attr.insert_or_assign(KEY_SERVER, "localhost");
    ASSERT_EQ(SQL_SUCCESS, RDS_InitializeConnection(dbc_, ""));
    EXPECT_TRUE(dbc_->dsql_lifetime == nullptr);
}
