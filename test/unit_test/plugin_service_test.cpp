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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../driver/host_info.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/dialect/dialect.h"
#include "../../driver/driver.h"
#include "../../driver/host_list_providers/rds_host_list_provider.h"
#include "../../driver/util/rds_lib_loader.h"

namespace {
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string PSQLODBC_PATH = "/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so";

    // Records every base-driver function the wrapper looks up, and returns nullptr so nothing is called.
    class RecordingRdsLibLoader : public RdsLibLoader {
    public:
        FUNC_HANDLE GetFunction(const std::string& function_name) override {
            requested_functions.push_back(function_name);
            return nullptr;
        }
        std::vector<std::string> requested_functions;
    };
}  // namespace

class PluginServiceTest : public testing::Test {
protected:
    std::shared_ptr<PluginService> plugin_service_;
    std::vector<HostInfo> all_hosts_;
    HostInfo host_a_ = HostInfo("host_a.region.com", 1234, UP, READER, 0);
    HostInfo host_b_ = HostInfo("host_b.region.com", 1234, UP, READER, 0);
    HostInfo host_c_ = HostInfo("host_c.region.com", 1234, UP, READER, 0);

    // Runs once per suite
    static void SetUpTestSuite() {}
    static void TearDownTestSuite() {}

    void SetUp() override {
        all_hosts_.push_back(host_a_);
        all_hosts_.push_back(host_b_);
        all_hosts_.push_back(host_c_);
        std::map<std::string, std::string> empty_map;
        plugin_service_ = std::make_shared<PluginService>(nullptr, nullptr, empty_map, "ClusterId");
        plugin_service_->SetHosts(all_hosts_);
    }
    void TearDown() override {}
};

TEST_F(PluginServiceTest, GetHosts) {
    std::vector<HostInfo> topology_hosts = plugin_service_->GetHosts();
    EXPECT_EQ(all_hosts_, topology_hosts);
}

TEST_F(PluginServiceTest, GetHosts_WithFilter) {
    HostFilter filter;
    filter.blocked_host_ids.insert(host_a_.GetHostId());
    filter.blocked_host_ids.insert(host_b_.GetHostId());
    filter.blocked_host_ids.insert(host_c_.GetHostId());
    plugin_service_->SetHostFilter(filter);

    std::vector<HostInfo> topology_hosts = plugin_service_->GetHosts();
    EXPECT_EQ(all_hosts_, topology_hosts);
}

TEST_F(PluginServiceTest, GetFilteredHosts_Allowed) {
    std::vector<HostInfo> expected_hosts;
    expected_hosts.push_back(host_a_);

    HostFilter filter;
    filter.allowed_host_ids.insert(host_a_.GetHostId());
    plugin_service_->SetHostFilter(filter);

    std::vector<HostInfo> topology_hosts = plugin_service_->GetFilteredHosts();
    EXPECT_EQ(expected_hosts, topology_hosts);
}

TEST_F(PluginServiceTest, GetFilteredHosts_Blocked) {
    std::vector<HostInfo> expected_hosts;
    expected_hosts.push_back(host_b_);
    expected_hosts.push_back(host_c_);

    HostFilter filter;
    filter.blocked_host_ids.insert(host_a_.GetHostId());
    plugin_service_->SetHostFilter(filter);

    std::vector<HostInfo> topology_hosts = plugin_service_->GetFilteredHosts();
    EXPECT_EQ(expected_hosts, topology_hosts);
}

TEST_F(PluginServiceTest, InitClusterId_UserInput) {
    std::map<std::string, std::string> conn_info;
    std::string expected_id = "custom_id";
    conn_info.insert_or_assign(KEY_CLUSTER_ID, expected_id);
    std::string returned_id = PluginService::InitClusterId(conn_info);
    EXPECT_EQ(returned_id, expected_id);
}

TEST_F(PluginServiceTest, InitClusterId_Generated) {
    std::map<std::string, std::string> conn_info;
    conn_info.insert_or_assign(KEY_SERVER, "NON_RDS");
    std::string returned_id = PluginService::InitClusterId(conn_info);
    EXPECT_FALSE(returned_id.empty());
    std::string map_id = conn_info.at(KEY_CLUSTER_ID);
    EXPECT_EQ(map_id, returned_id);
}

TEST_F(PluginServiceTest, InitDialect_DsqlHostWithPsqlodbc) {
    const std::map<std::string, std::string> conn_info = {
        {KEY_SERVER, DSQL_HOST},
        {KEY_DRIVER, PSQLODBC_PATH},
    };
    EXPECT_EQ(DatabaseDialectType::AURORA_DSQL, PluginService::InitDialect(conn_info)->GetDialectType());
}

TEST_F(PluginServiceTest, InitDialect_DsqlExplicit) {
    const std::map<std::string, std::string> conn_info = {
        {KEY_SERVER, "my-endpoint.example.com"},
        {KEY_DATABASE_DIALECT, "aurora_dsql"},
    };
    EXPECT_EQ(DatabaseDialectType::AURORA_DSQL, PluginService::InitDialect(conn_info)->GetDialectType());
}

TEST_F(PluginServiceTest, InitDialect_ExplicitDialectWinsOverDsqlHost) {
    const std::map<std::string, std::string> conn_info = {
        {KEY_SERVER, DSQL_HOST},
        {KEY_DATABASE_DIALECT, VALUE_DB_DIALECT_AURORA_POSTGRESQL},
    };
    EXPECT_EQ(DatabaseDialectType::AURORA_POSTGRESQL, PluginService::InitDialect(conn_info)->GetDialectType());
}

TEST_F(PluginServiceTest, InitDialect_PsqlodbcAuroraHost_Unchanged) {
    const std::map<std::string, std::string> conn_info = {
        {KEY_SERVER, "database-test-name.cluster-XYZ.us-east-2.rds.amazonaws.com"},
        {KEY_DRIVER, PSQLODBC_PATH},
    };
    EXPECT_EQ(DatabaseDialectType::AURORA_POSTGRESQL, PluginService::InitDialect(conn_info)->GetDialectType());
}

TEST_F(PluginServiceTest, DsqlDialect_Defaults) {
    const std::map<std::string, std::string> conn_info = {{KEY_SERVER, DSQL_HOST}};
    const std::shared_ptr<Dialect> dialect = PluginService::InitDialect(conn_info);
    EXPECT_EQ(5432, dialect->GetDefaultPort());
    EXPECT_EQ("", dialect->GetTopologyQuery());
    EXPECT_TRUE(dialect->IsSqlStateAccessError("28P01", ""));
    EXPECT_TRUE(dialect->IsSqlStateNetworkError("08006"));
    EXPECT_FALSE(dialect->IsSqlStateNetworkError("40001"));
}

TEST_F(PluginServiceTest, InitHostListProvider_Dsql_NoTopologyMonitor) {
    PluginService service(nullptr, nullptr, {{KEY_SERVER, DSQL_HOST}, {KEY_DRIVER, PSQLODBC_PATH}}, "");
    service.InitHostListProvider();
    const std::shared_ptr<HostListProvider> provider = service.GetHostListProvider();
    ASSERT_NE(nullptr, provider);
    EXPECT_EQ(nullptr, std::dynamic_pointer_cast<RdsHostListProvider>(provider));
}

TEST_F(PluginServiceTest, UpdateDialect_Dsql_SkipsTopologyQuery) {
    const auto lib_loader = std::make_shared<RecordingRdsLibLoader>();
    PluginService service(lib_loader, nullptr, {{KEY_SERVER, DSQL_HOST}}, "");
    DBC dbc;
    // Any non-null handle, so a topology query would get as far as allocating a statement.
    dbc.wrapped_dbc = reinterpret_cast<SQLHDBC>(&dbc);

    service.UpdateDialect(&dbc);

    EXPECT_TRUE(lib_loader->requested_functions.empty());
    dbc.wrapped_dbc = SQL_NULL_HDBC;
}

// A custom DNS name or VPC endpoint doesn't match the DSQL pattern, but the auth type still says DSQL.
TEST_F(PluginServiceTest, InitDialect_DsqlAuthTypeCustomHost) {
    const std::map<std::string, std::string> conn_info = {
        {KEY_SERVER, "db.internal.example.com"},
        {KEY_DRIVER, PSQLODBC_PATH},
        {KEY_AUTH_TYPE, "dsql"},
    };
    EXPECT_EQ(DatabaseDialectType::AURORA_DSQL, PluginService::InitDialect(conn_info)->GetDialectType());
}

// psqlODBC reports every failed libpq connect as 08001 with the server's message, so a rejected token is only
// recognizable from the message. A plain connection failure must stay a non-access error.
TEST_F(PluginServiceTest, DsqlDialect_PsqlodbcAuthFailureIsAccessError) {
    const std::map<std::string, std::string> conn_info = {{KEY_SERVER, DSQL_HOST}};
    const std::shared_ptr<Dialect> dialect = PluginService::InitDialect(conn_info);
    EXPECT_TRUE(dialect->IsSqlStateAccessError("08001", "FATAL:  password authentication failed for user \"admin\""));
    // What DSQL returns for a token it rejects (smoke run, 2026-10-19).
    EXPECT_TRUE(dialect->IsSqlStateAccessError("08001",
        "connection to server at \"x.dsql.us-east-1.on.aws\", port 5432 failed: FATAL:  unable to accept connection, "
        "access denied\nDETAIL:  Session Id: abc\nHINT:  Credential should be scoped to a valid region."));
    EXPECT_FALSE(dialect->IsSqlStateAccessError("08001", "could not translate host name to address"));
}
