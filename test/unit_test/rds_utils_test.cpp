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

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../../driver/util/rds_utils.h"

namespace {
    const std::string US_EAST_REGION_CLUSTER = "database-test-name.cluster-XYZ.us-east-2.rds.amazonaws.com";
    const std::string US_EAST_REGION_CLUSTER_READ_ONLY = "database-test-name.cluster-ro-XYZ.us-east-2.rds.amazonaws.com";
    const std::string US_EAST_REGION_INSTANCE = "instance-test-name.XYZ.us-east-2.rds.amazonaws.com";
    const std::string US_EAST_REGION_PROXY = "proxy-test-name.proxy-XYZ.us-east-2.rds.amazonaws.com";
    const std::string US_EAST_REGION_CUSTOM_DOMAIN = "custom-test-name.cluster-custom-XYZ.us-east-2.rds.amazonaws.com";
    const std::string US_EAST_LIMITLESS_SHARD_GROUP = "database-test-name.shardgrp-XYZ.us-east-2.rds.amazonaws.com";

    const std::string CHINA_REGION_CLUSTER = "database-test-name.cluster-XYZ.rds.cn-northwest-1.amazonaws.com.cn";
    const std::string CHINA_REGION_CLUSTER_READ_ONLY = "database-test-name.cluster-ro-XYZ.rds.cn-northwest-1.amazonaws.com.cn";
    const std::string CHINA_REGION_PROXY = "proxy-test-name.proxy-XYZ.rds.cn-northwest-1.amazonaws.com.cn";
    const std::string CHINA_REGION_CUSTOM_DOMAIN = "custom-test-name.cluster-custom-XYZ.rds.cn-northwest-1.amazonaws.com.cn";
    const std::string DSQL_CLUSTER = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string DSQL_CLUSTER_SUFFIXED = "abcdefghijklmnopqrstuvwxyz.dsql-gamma.us-west-2.on.aws";
    const std::string DSQL_CLUSTER_UPPER = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.DSQL.EU-WEST-1.ON.AWS";
}  // namespace

class RdsUtilsTest : public testing::Test {
   protected:
    static void SetUpTestSuite() {}
    static void TearDownTestSuite() {}

    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(RdsUtilsTest, IsRdsDns) {
    EXPECT_TRUE(RdsUtils::IsRdsDns(US_EAST_REGION_CLUSTER));
    EXPECT_TRUE(RdsUtils::IsRdsDns(US_EAST_REGION_CLUSTER_READ_ONLY));
    EXPECT_TRUE(RdsUtils::IsRdsDns(US_EAST_REGION_PROXY));
    EXPECT_TRUE(RdsUtils::IsRdsDns(US_EAST_REGION_CUSTOM_DOMAIN));
    EXPECT_TRUE(RdsUtils::IsRdsDns(US_EAST_LIMITLESS_SHARD_GROUP));

    EXPECT_TRUE(RdsUtils::IsRdsDns(CHINA_REGION_CLUSTER));
    EXPECT_TRUE(RdsUtils::IsRdsDns(CHINA_REGION_CLUSTER_READ_ONLY));
    EXPECT_TRUE(RdsUtils::IsRdsDns(CHINA_REGION_PROXY));
    EXPECT_TRUE(RdsUtils::IsRdsDns(CHINA_REGION_CUSTOM_DOMAIN));
}

TEST_F(RdsUtilsTest, IsRdsClusterDns) {
    EXPECT_TRUE(RdsUtils::IsRdsClusterDns(US_EAST_REGION_CLUSTER));
    EXPECT_TRUE(RdsUtils::IsRdsClusterDns(US_EAST_REGION_CLUSTER_READ_ONLY));
    EXPECT_FALSE(RdsUtils::IsRdsClusterDns(US_EAST_REGION_PROXY));
    EXPECT_FALSE(RdsUtils::IsRdsClusterDns(US_EAST_REGION_CUSTOM_DOMAIN));
    EXPECT_FALSE(RdsUtils::IsRdsClusterDns(US_EAST_LIMITLESS_SHARD_GROUP));

    EXPECT_TRUE(RdsUtils::IsRdsClusterDns(CHINA_REGION_CLUSTER));
    EXPECT_TRUE(RdsUtils::IsRdsClusterDns(CHINA_REGION_CLUSTER_READ_ONLY));
    EXPECT_FALSE(RdsUtils::IsRdsClusterDns(CHINA_REGION_PROXY));
    EXPECT_FALSE(RdsUtils::IsRdsClusterDns(CHINA_REGION_CUSTOM_DOMAIN));
}

TEST_F(RdsUtilsTest, IsRdsProxyDns) {
    EXPECT_FALSE(RdsUtils::IsRdsProxyDns(US_EAST_REGION_CLUSTER));
    EXPECT_FALSE(RdsUtils::IsRdsProxyDns(US_EAST_REGION_CLUSTER_READ_ONLY));
    EXPECT_TRUE(RdsUtils::IsRdsProxyDns(US_EAST_REGION_PROXY));
    EXPECT_FALSE(RdsUtils::IsRdsProxyDns(US_EAST_REGION_CUSTOM_DOMAIN));
    EXPECT_FALSE(RdsUtils::IsRdsProxyDns(US_EAST_LIMITLESS_SHARD_GROUP));

    EXPECT_FALSE(RdsUtils::IsRdsProxyDns(CHINA_REGION_CLUSTER));
    EXPECT_FALSE(RdsUtils::IsRdsProxyDns(CHINA_REGION_CLUSTER_READ_ONLY));
    EXPECT_TRUE(RdsUtils::IsRdsProxyDns(CHINA_REGION_PROXY));
    EXPECT_FALSE(RdsUtils::IsRdsProxyDns(CHINA_REGION_CUSTOM_DOMAIN));
}

TEST_F(RdsUtilsTest, IsRdsWriterClusterDns) {
    EXPECT_TRUE(RdsUtils::IsRdsWriterClusterDns(US_EAST_REGION_CLUSTER));
    EXPECT_FALSE(RdsUtils::IsRdsWriterClusterDns(US_EAST_REGION_CLUSTER_READ_ONLY));
    EXPECT_FALSE(RdsUtils::IsRdsWriterClusterDns(US_EAST_REGION_PROXY));
    EXPECT_FALSE(RdsUtils::IsRdsWriterClusterDns(US_EAST_REGION_CUSTOM_DOMAIN));
    EXPECT_FALSE(RdsUtils::IsRdsWriterClusterDns(US_EAST_LIMITLESS_SHARD_GROUP));

    EXPECT_TRUE(RdsUtils::IsRdsWriterClusterDns(CHINA_REGION_CLUSTER));
    EXPECT_FALSE(RdsUtils::IsRdsWriterClusterDns(CHINA_REGION_CLUSTER_READ_ONLY));
    EXPECT_FALSE(RdsUtils::IsRdsWriterClusterDns(CHINA_REGION_PROXY));
    EXPECT_FALSE(RdsUtils::IsRdsWriterClusterDns(CHINA_REGION_CUSTOM_DOMAIN));
}

TEST_F(RdsUtilsTest, IsRdsCustomClusterDns) {
    EXPECT_FALSE(RdsUtils::IsRdsCustomClusterDns(US_EAST_REGION_CLUSTER));
    EXPECT_FALSE(RdsUtils::IsRdsCustomClusterDns(US_EAST_REGION_CLUSTER_READ_ONLY));
    EXPECT_FALSE(RdsUtils::IsRdsCustomClusterDns(US_EAST_REGION_PROXY));
    EXPECT_TRUE(RdsUtils::IsRdsCustomClusterDns(US_EAST_REGION_CUSTOM_DOMAIN));
    EXPECT_FALSE(RdsUtils::IsRdsCustomClusterDns(US_EAST_LIMITLESS_SHARD_GROUP));

    EXPECT_FALSE(RdsUtils::IsRdsCustomClusterDns(CHINA_REGION_CLUSTER));
    EXPECT_FALSE(RdsUtils::IsRdsCustomClusterDns(CHINA_REGION_CLUSTER_READ_ONLY));
    EXPECT_FALSE(RdsUtils::IsRdsCustomClusterDns(CHINA_REGION_PROXY));
    EXPECT_TRUE(RdsUtils::IsRdsCustomClusterDns(CHINA_REGION_CUSTOM_DOMAIN));
}

TEST_F(RdsUtilsTest, GetRdsInstanceHostPattern) {
    const std::string expected_pattern = "?.XYZ.us-east-2.rds.amazonaws.com";
    EXPECT_EQ(expected_pattern, RdsUtils::GetRdsInstanceHostPattern(US_EAST_REGION_CLUSTER));
    EXPECT_EQ(expected_pattern, RdsUtils::GetRdsInstanceHostPattern(US_EAST_REGION_CLUSTER_READ_ONLY));
    EXPECT_EQ(expected_pattern, RdsUtils::GetRdsInstanceHostPattern(US_EAST_REGION_PROXY));
    EXPECT_EQ(expected_pattern, RdsUtils::GetRdsInstanceHostPattern(US_EAST_REGION_CUSTOM_DOMAIN));
    EXPECT_EQ(expected_pattern, RdsUtils::GetRdsInstanceHostPattern(US_EAST_LIMITLESS_SHARD_GROUP));

    const std::string expected_china_pattern = "?.XYZ.rds.cn-northwest-1.amazonaws.com.cn";
    EXPECT_EQ(expected_china_pattern, RdsUtils::GetRdsInstanceHostPattern(CHINA_REGION_CLUSTER));
    EXPECT_EQ(expected_china_pattern, RdsUtils::GetRdsInstanceHostPattern(CHINA_REGION_CLUSTER_READ_ONLY));
    EXPECT_EQ(expected_china_pattern, RdsUtils::GetRdsInstanceHostPattern(CHINA_REGION_PROXY));
    EXPECT_EQ(expected_china_pattern, RdsUtils::GetRdsInstanceHostPattern(CHINA_REGION_CUSTOM_DOMAIN));
}

TEST_F(RdsUtilsTest, GetRdsClusterHostUrl) {
    EXPECT_EQ(US_EAST_REGION_CLUSTER, RdsUtils::GetRdsClusterHostUrl(US_EAST_REGION_CLUSTER));
    EXPECT_EQ(US_EAST_REGION_CLUSTER, RdsUtils::GetRdsClusterHostUrl(US_EAST_REGION_CLUSTER_READ_ONLY));
    EXPECT_EQ(std::string(), RdsUtils::GetRdsClusterHostUrl(US_EAST_REGION_PROXY));
    EXPECT_EQ(std::string(), RdsUtils::GetRdsClusterHostUrl(US_EAST_REGION_CUSTOM_DOMAIN));
    EXPECT_EQ(std::string(), RdsUtils::GetRdsClusterHostUrl(US_EAST_LIMITLESS_SHARD_GROUP));

    EXPECT_EQ(CHINA_REGION_CLUSTER, RdsUtils::GetRdsClusterHostUrl(CHINA_REGION_CLUSTER));
    EXPECT_EQ(CHINA_REGION_CLUSTER, RdsUtils::GetRdsClusterHostUrl(CHINA_REGION_CLUSTER_READ_ONLY));
    EXPECT_EQ(std::string(), RdsUtils::GetRdsClusterHostUrl(CHINA_REGION_PROXY));
    EXPECT_EQ(std::string(), RdsUtils::GetRdsClusterHostUrl(CHINA_REGION_CUSTOM_DOMAIN));
}

TEST_F(RdsUtilsTest, GetRdsClusterId) {
    EXPECT_EQ("database-test-name", RdsUtils::GetRdsClusterId(US_EAST_REGION_CLUSTER));
    EXPECT_EQ("database-test-name", RdsUtils::GetRdsClusterId(US_EAST_REGION_CLUSTER_READ_ONLY));
    EXPECT_EQ(std::string(), RdsUtils::GetRdsClusterId(US_EAST_REGION_INSTANCE));

    EXPECT_EQ("proxy-test-name", RdsUtils::GetRdsClusterId(US_EAST_REGION_PROXY));
    EXPECT_EQ("custom-test-name", RdsUtils::GetRdsClusterId(US_EAST_REGION_CUSTOM_DOMAIN));

    EXPECT_EQ("database-test-name", RdsUtils::GetRdsClusterId(US_EAST_LIMITLESS_SHARD_GROUP));

    EXPECT_EQ("database-test-name", RdsUtils::GetRdsClusterId(CHINA_REGION_CLUSTER));
    EXPECT_EQ("database-test-name", RdsUtils::GetRdsClusterId(CHINA_REGION_CLUSTER_READ_ONLY));
    EXPECT_EQ("proxy-test-name", RdsUtils::GetRdsClusterId(CHINA_REGION_PROXY));
    EXPECT_EQ("custom-test-name", RdsUtils::GetRdsClusterId(CHINA_REGION_CUSTOM_DOMAIN));
}

TEST_F(RdsUtilsTest, GetRdsInstanceId) {
    EXPECT_EQ("instance-test-name", RdsUtils::GetRdsInstanceId(US_EAST_REGION_INSTANCE));
    EXPECT_EQ(std::string(), RdsUtils::GetRdsInstanceId(US_EAST_REGION_CLUSTER_READ_ONLY));
    EXPECT_EQ(std::string(), RdsUtils::GetRdsInstanceId(US_EAST_REGION_CLUSTER));
}

TEST_F(RdsUtilsTest, IsDsqlDns) {
    EXPECT_TRUE(RdsUtils::IsDsqlDns(DSQL_CLUSTER));
    EXPECT_TRUE(RdsUtils::IsDsqlDns(DSQL_CLUSTER_SUFFIXED));
    EXPECT_TRUE(RdsUtils::IsDsqlDns(DSQL_CLUSTER_UPPER));

    EXPECT_FALSE(RdsUtils::IsDsqlDns(US_EAST_REGION_CLUSTER));
    EXPECT_FALSE(RdsUtils::IsDsqlDns(US_EAST_LIMITLESS_SHARD_GROUP));
    EXPECT_FALSE(RdsUtils::IsDsqlDns("abcdefghijklmnopqrstuvwxyz"));
    EXPECT_FALSE(RdsUtils::IsDsqlDns("abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws.example.com"));
    EXPECT_FALSE(RdsUtils::IsDsqlDns(""));
}

TEST_F(RdsUtilsTest, GetDsqlRegion) {
    EXPECT_EQ("us-east-1", RdsUtils::GetDsqlRegion(DSQL_CLUSTER));
    EXPECT_EQ("us-west-2", RdsUtils::GetDsqlRegion(DSQL_CLUSTER_SUFFIXED));
    EXPECT_EQ("eu-west-1", RdsUtils::GetDsqlRegion(DSQL_CLUSTER_UPPER));

    EXPECT_EQ("", RdsUtils::GetDsqlRegion(US_EAST_REGION_CLUSTER));
    EXPECT_EQ("", RdsUtils::GetDsqlRegion("localhost"));
    EXPECT_EQ("", RdsUtils::GetDsqlRegion(""));
}

// The RDS parser must keep ignoring DSQL endpoints, so the IAM plugin doesn't sign for them by accident.
TEST_F(RdsUtilsTest, GetRdsRegion_DsqlHost_Empty) {
    EXPECT_EQ("", RdsUtils::GetRdsRegion(DSQL_CLUSTER));
}
