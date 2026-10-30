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
