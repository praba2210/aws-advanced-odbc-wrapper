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
