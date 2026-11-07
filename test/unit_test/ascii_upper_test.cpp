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

// Connection string and DSN keys, the option values that name an enum and SQL keywords are uppercased in ASCII.
// ICU's default locale comes from the environment (LANG, LC_ALL or LC_MESSAGES on Linux, the regional format on
// Windows), and under a Turkish one it uppercases i to a dotted capital I.

#include <map>
#include <string>

#include <gtest/gtest.h>

#include "unicode/locid.h"
#include "unicode/utypes.h"

#include "../../driver/dialect/dialect.h"
#include "../../driver/driver.h"
#include "../../driver/host_selector/host_selector.h"
#include "../../driver/util/auth_provider.h"
#include "../../driver/util/connection_string_helper.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/rds_strings.h"
#include "../../driver/util/sql_query_analyzer.h"

namespace {
    // Sets ICU's default locale for one test and restores the previous one afterwards.
    class IcuDefaultLocale {
    public:
        explicit IcuDefaultLocale(const char* name) : saved_(icu::Locale::getDefault()) {
            UErrorCode status = U_ZERO_ERROR;
            icu::Locale::setDefault(icu::Locale(name), status);
        }

        ~IcuDefaultLocale() {
            UErrorCode status = U_ZERO_ERROR;
            icu::Locale::setDefault(saved_, status);
        }

        IcuDefaultLocale(const IcuDefaultLocale&) = delete;
        IcuDefaultLocale& operator=(const IcuDefaultLocale&) = delete;
        IcuDefaultLocale(IcuDefaultLocale&&) = delete;
        IcuDefaultLocale& operator=(IcuDefaultLocale&&) = delete;

    private:
        icu::Locale saved_;
    };
}  // namespace

TEST(AsciiUpperTest, ToAsciiUpper_FoldsOnlyAsciiLetters) {
    EXPECT_EQ("REGION=IL-CENTRAL-1;UID_9", ToAsciiUpper("Region=il-central-1;Uid_9"));
    EXPECT_EQ("", ToAsciiUpper(""));
    // A dotless i and a u with diaeresis (UTF-8) stay as they are.
    EXPECT_EQ("\xC4\xB1\xC3\xBC", ToAsciiUpper("\xC4\xB1\xC3\xBC"));
}

TEST(AsciiUpperTest, TurkishIcuDefault_KeysAndEnumNamesStillMatch) {
    const IcuDefaultLocale turkish("tr_TR");
    // What ICU does under tr_TR, and RDS_STR_UPPER with it: region becomes REG<U+0130>ON.
    ASSERT_EQ("REG\xC4\xB0ON", RDS_STR_UPPER("region"));

    std::map<std::string, std::string> conn_map;
    ConnectionStringHelper::ParseConnectionString("Server=host;Region=il-central-1;Uid=admin;rds_auth_type=iam",
                                                  conn_map);
    const std::map<std::string, std::string> expected = {
        {KEY_SERVER, "host"}, {KEY_REGION, "il-central-1"}, {KEY_DB_USERNAME, "admin"}, {KEY_AUTH_TYPE, "iam"}};
    EXPECT_EQ(expected, conn_map);

    EXPECT_EQ(AuthType::IAM, AuthProvider::AuthTypeFromString("iam"));
    EXPECT_EQ(DatabaseDialectType::MULTI_AZ_PG, Dialect::DatabaseDialectFromString("multi_az_postgresql"));
    EXPECT_EQ(HostSelectorStrategies::HIGHEST_WEIGHT, HostSelector::GetHostSelectorStrategy("highest_weight"));
}

// The transaction tracking reads the statement's first keyword.
TEST(AsciiUpperTest, TurkishIcuDefault_SqlKeywordsStillMatch) {
    const IcuDefaultLocale turkish("tr_TR");
    DBC dbc{};
    EXPECT_TRUE(SqlQueryAnalyzer::DoesOpenTransaction("begin"));
    EXPECT_TRUE(SqlQueryAnalyzer::DoesCloseTransaction(&dbc, "commit"));
}
