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

#ifndef DSQL_OCC_RETRY_PLUGIN_H_
#define DSQL_OCC_RETRY_PLUGIN_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "../../driver.h"
#include "../../util/occ_retry_config.h"
#include "../base_plugin.h"

// Runs a statement again after an Aurora DSQL optimistic concurrency control (OCC) conflict when it ran alone in
// autocommit mode with no transaction open: waits with exponential backoff and jitter, clears the statement's
// diagnostics and executes it again on the same handle. After the last retry it returns the last error with the base
// driver's diagnostics as they were. It follows the transaction-control text it runs, so a transaction the application
// opened with BEGIN while autocommit is on is never retried into. On with ENABLE_OCC_RETRY=1, on Aurora DSQL only.
class DsqlOccRetryPlugin : public BasePlugin {
public:
    // Waits out a backoff delay. Tests pass one that records the delay.
    using SleepFunction = std::function<void(std::chrono::milliseconds)>;
    // Returns a number in [0, 1) that scales the jitter.
    using RandomFunction = std::function<double()>;

    // How a statement moves the session's transaction state.
    enum class TransactionChange : std::uint8_t { NONE, OPENS, CLOSES };

    DsqlOccRetryPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin);
    // A null sleep or random function means a real sleep or a seeded generator.
    DsqlOccRetryPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin, SleepFunction sleep, RandomFunction random);

    SQLRETURN Execute(
        SQLHSTMT       StatementHandle,
        SQLTCHAR *     StatementText,
        SQLINTEGER     TextLength) override;

    // SQLSTATE 40001, OC000 or OC001. Public for tests.
    static bool IsOccConflict(const std::string& sql_state);

    // True for a single statement that isn't transaction control or a session setting. Public for tests.
    static bool IsRetryableStatement(const std::string& statement);

    // OPENS when any statement of the text starts a transaction block (BEGIN, START TRANSACTION, ... AND CHAIN),
    // CLOSES when the text is a single statement that only ends one, NONE otherwise. Public for tests.
    static TransactionChange GetTransactionChange(const std::string& statement);

    // The wait before retry number `retry` (from 1): the base delay doubled for each earlier retry, capped at the max
    // delay, plus random_unit x jitter_factor of that. Public for tests.
    static std::chrono::milliseconds GetBackoffDelay(const OccRetryConfig& config, int retry, double random_unit);

private:
    // The retries after a first attempt that failed; returns the final result.
    SQLRETURN RetryOccConflicts(STMT* stmt, SQLTCHAR* statement_text, SQLINTEGER text_length);

    OccRetryConfig config_;
    SleepFunction sleep_;
    RandomFunction random_;
};

#endif // DSQL_OCC_RETRY_PLUGIN_H_
