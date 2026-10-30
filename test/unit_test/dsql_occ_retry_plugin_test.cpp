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

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../../driver/driver.h"
#include "../../driver/error.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/base_plugin.h"
#include "../../driver/util/attribute_validator.h"
#include "../../driver/util/connection_string_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/logger_wrapper.h"
#include "../../driver/util/occ_retry_config.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/rds_lib_loader.h"
#include "../../driver/util/rds_strings.h"
#include "common_mock_objects.h"

// The OCC options are the wrapper's: they never reach psqlODBC's connection string.
TEST(DsqlOccRetrySetupTest, OccOptions_AreWrapperKeys) {
    for (const char* key : {KEY_ENABLE_OCC_RETRY, KEY_OCC_MAX_RETRIES, KEY_OCC_BASE_DELAY_MS, KEY_OCC_MAX_DELAY_MS,
                            KEY_OCC_JITTER_FACTOR}) {
        EXPECT_TRUE(ConnectionStringHelper::IsAwsOdbcKey(key)) << key;
    }
    const std::map<std::string, std::string> conn_attr = {
        {KEY_SERVER, "host"}, {KEY_ENABLE_OCC_RETRY, "1"}, {KEY_OCC_MAX_RETRIES, "5"}};
    EXPECT_EQ("SERVER=host", ConnectionStringHelper::BuildMinimumConnectionString(conn_attr));
}

TEST(DsqlOccRetrySetupTest, Statement_StartsWithNoPreparedText) {
    const STMT stmt{};
    EXPECT_TRUE(stmt.prepared_text.empty());
}

// Chain-order tests walk the chain from its head.
TEST(DsqlOccRetrySetupTest, BasePlugin_ShowsTheNextPluginAndItsName) {
    const auto bottom = std::make_shared<BasePlugin>(nullptr);
    const BasePlugin top(nullptr, bottom);
    EXPECT_EQ(bottom.get(), top.GetNextPlugin());
    EXPECT_EQ(nullptr, bottom->GetNextPlugin());
    EXPECT_EQ("BasePlugin", top.GetPluginName());
}

// The Aurora DSQL connection lifetime option is the wrapper's too, and a new connection handle has no lifetime yet.
TEST(DsqlConnectionLifetimeSetupTest, MaxConnectionLifetime_IsAWrapperKey) {
    EXPECT_TRUE(ConnectionStringHelper::IsAwsOdbcKey(KEY_MAX_CONNECTION_LIFETIME));
    const std::map<std::string, std::string> conn_attr = {{KEY_SERVER, "host"}, {KEY_MAX_CONNECTION_LIFETIME, "600"}};
    EXPECT_EQ("SERVER=host", ConnectionStringHelper::BuildMinimumConnectionString(conn_attr));
}

TEST(DsqlConnectionLifetimeSetupTest, Connection_StartsWithoutALifetime) {
    const DBC dbc{};
    EXPECT_TRUE(dbc.dsql_lifetime == nullptr);
}

TEST(DsqlOccRetryConfigTest, Parse_Defaults) {
    std::string error;
    const std::optional<OccRetryConfig> config = OccRetryConfig::Parse({}, error);
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->enabled);
    EXPECT_EQ(3, config->max_retries);
    EXPECT_EQ(100, config->base_delay_ms);
    EXPECT_EQ(5000, config->max_delay_ms);
    EXPECT_DOUBLE_EQ(0.25, config->jitter_factor);
    EXPECT_TRUE(error.empty());
}

TEST(DsqlOccRetryConfigTest, Parse_ReadsEveryOption) {
    std::string error;
    const std::optional<OccRetryConfig> config = OccRetryConfig::Parse({
        {KEY_ENABLE_OCC_RETRY, "1"}, {KEY_OCC_MAX_RETRIES, "10"}, {KEY_OCC_BASE_DELAY_MS, "1"},
        {KEY_OCC_MAX_DELAY_MS, "100"}, {KEY_OCC_JITTER_FACTOR, "0.5"}}, error);
    ASSERT_TRUE(config.has_value()) << error;
    EXPECT_TRUE(config->enabled);
    EXPECT_EQ(10, config->max_retries);
    EXPECT_EQ(1, config->base_delay_ms);
    EXPECT_EQ(100, config->max_delay_ms);
    EXPECT_DOUBLE_EQ(0.5, config->jitter_factor);
}

TEST(DsqlOccRetryConfigTest, Parse_AcceptsRangeEnds) {
    const std::vector<std::map<std::string, std::string>> cases = {
        {{KEY_ENABLE_OCC_RETRY, "0"}},
        {{KEY_OCC_MAX_RETRIES, "0"}},
        {{KEY_OCC_MAX_RETRIES, "100"}},
        {{KEY_OCC_MAX_RETRIES, " +7 "}},
        {{KEY_OCC_BASE_DELAY_MS, "1"}},
        {{KEY_OCC_BASE_DELAY_MS, "250"}, {KEY_OCC_MAX_DELAY_MS, "250"}},
        {{KEY_OCC_BASE_DELAY_MS, "60000"}, {KEY_OCC_MAX_DELAY_MS, "60000"}},
        {{KEY_OCC_JITTER_FACTOR, "0"}},
        {{KEY_OCC_JITTER_FACTOR, "1.0"}},
        {{KEY_OCC_JITTER_FACTOR, " 0.5 "}},
    };
    for (const auto& conn_attr : cases) {
        std::string error;
        EXPECT_TRUE(OccRetryConfig::Parse(conn_attr, error).has_value()) << error;
    }
}

TEST(DsqlOccRetryConfigTest, Parse_RejectsBadValuesWithWhatToUse) {
    const std::vector<std::pair<std::map<std::string, std::string>, std::string>> cases = {
        {{{KEY_ENABLE_OCC_RETRY, "yes"}}, "ENABLE_OCC_RETRY=yes (use 0 or 1)"},
        {{{KEY_OCC_MAX_RETRIES, "101"}}, "OCC_MAX_RETRIES=101 (use an integer from 0 to 100)"},
        {{{KEY_OCC_MAX_RETRIES, "-1"}}, "OCC_MAX_RETRIES=-1 (use an integer from 0 to 100)"},
        {{{KEY_OCC_MAX_RETRIES, "three"}}, "OCC_MAX_RETRIES=three (use an integer from 0 to 100)"},
        {{{KEY_OCC_BASE_DELAY_MS, "0"}}, "OCC_BASE_DELAY_MS=0 (use an integer from 1 to 60000)"},
        {{{KEY_OCC_BASE_DELAY_MS, "1.5"}}, "OCC_BASE_DELAY_MS=1.5 (use an integer from 1 to 60000)"},
        {{{KEY_OCC_BASE_DELAY_MS, "60001"}}, "OCC_BASE_DELAY_MS=60001 (use an integer from 1 to 60000)"},
        {{{KEY_OCC_MAX_DELAY_MS, "50"}},
         "OCC_MAX_DELAY_MS=50 (use an integer from OCC_BASE_DELAY_MS, 100, to 60000)"},
        {{{KEY_OCC_MAX_DELAY_MS, "soon"}},
         "OCC_MAX_DELAY_MS=soon (use an integer from OCC_BASE_DELAY_MS, 100, to 60000)"},
        {{{KEY_OCC_MAX_DELAY_MS, "60001"}},
         "OCC_MAX_DELAY_MS=60001 (use an integer from OCC_BASE_DELAY_MS, 100, to 60000)"},
        // The 5000 ms default cap is below this base, so the cap has to be set too: the entry says it's the default.
        {{{KEY_OCC_BASE_DELAY_MS, "10000"}},
         "OCC_MAX_DELAY_MS=5000 (the default; set it to an integer from OCC_BASE_DELAY_MS, 10000, to 60000)"},
        {{{KEY_OCC_JITTER_FACTOR, "1.5"}}, "OCC_JITTER_FACTOR=1.5 (use a number from 0.0 to 1.0)"},
        {{{KEY_OCC_JITTER_FACTOR, "-0.1"}}, "OCC_JITTER_FACTOR=-0.1 (use a number from 0.0 to 1.0)"},
        // A decimal comma is never read, whatever locale the application set.
        {{{KEY_OCC_JITTER_FACTOR, "0,25"}}, "OCC_JITTER_FACTOR=0,25 (use a number from 0.0 to 1.0)"},
        {{{KEY_OCC_JITTER_FACTOR, "nan"}}, "OCC_JITTER_FACTOR=nan (use a number from 0.0 to 1.0)"},
        {{{KEY_OCC_JITTER_FACTOR, ""}}, "OCC_JITTER_FACTOR= (use a number from 0.0 to 1.0)"},
    };
    for (const auto& [conn_attr, expected] : cases) {
        std::string error;
        EXPECT_FALSE(OccRetryConfig::Parse(conn_attr, error).has_value()) << expected;
        EXPECT_EQ(expected, error);
    }
}

TEST(DsqlOccRetryConfigTest, ValidateMap_ReportsTheBadOccOption) {
    EXPECT_EQ((std::unordered_set<std::string>{"OCC_MAX_RETRIES=101 (use an integer from 0 to 100)"}),
              AttributeValidator::ValidateMap({{KEY_OCC_MAX_RETRIES, "101"}, {KEY_PORT, "5432"}}));
    EXPECT_TRUE(AttributeValidator::ValidateMap({{KEY_OCC_MAX_RETRIES, "3"}, {KEY_ENABLE_OCC_RETRY, "0"}}).empty());
}

// Checked at every connect, before the base driver loads, whether or not ENABLE_OCC_RETRY is on.
TEST(DsqlOccRetryConfigTest, InitializeConnection_BadOccOptionFailsTheConnect) {
    ENV env;
    DBC dbc;
    dbc.env = &env;
    dbc.conn_attr = {{KEY_OCC_JITTER_FACTOR, "2"}};

    EXPECT_EQ(SQL_ERROR, RDS_InitializeConnection(&dbc, ""));

    ASSERT_TRUE(dbc.err);
    EXPECT_STREQ("01S00", dbc.err->sqlstate);
    EXPECT_NE(nullptr, std::strstr(dbc.err->error_msg, "OCC_JITTER_FACTOR=2 (use a number from 0.0 to 1.0)"));
}
