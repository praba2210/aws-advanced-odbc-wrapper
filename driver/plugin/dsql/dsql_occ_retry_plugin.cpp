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

#include "dsql_occ_retry_plugin.h"

#include "../../util/windows_headers.h"

#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../driver.h"
#include "../../odbcapi.h"
#include "../../util/logger_wrapper.h"
#include "../../util/rds_lib_loader.h"
#include "dsql_plugin_helper.h"

namespace {
    // More doublings could overflow; the delay reaches any cap long before.
    constexpr int MAX_DOUBLINGS = 31;

    // The most words a transaction-control statement has: COMMIT TRANSACTION AND NO CHAIN.
    constexpr size_t MAX_CONTROL_WORDS = 5;

    // What may follow the last statement of a text.
    constexpr std::string_view TRAILING_CHARACTERS = " \t\n\v\f\r;";

    // PostgreSQL counts every byte past ASCII as a letter of a word.
    constexpr unsigned char FIRST_NON_ASCII = 0x80;

    // First words of statements that never run twice: they start, end or shape a transaction, or set session state. A
    // COMMIT that failed and ran again would report success, with no transaction in progress.
    constexpr auto NOT_RETRIED_KEYWORDS = std::to_array<std::string_view>({
        "ABORT", "BEGIN", "COMMIT", "END", "PREPARE", "RELEASE", "ROLLBACK", "SAVEPOINT", "SET", "START",
    });

    // First words that start a transaction block.
    constexpr auto OPENING_KEYWORDS = std::to_array<std::string_view>({"BEGIN", "START"});

    // First words that end one, and the words that may follow them in a statement that only ends it.
    constexpr auto CLOSING_KEYWORDS = std::to_array<std::string_view>({"ABORT", "COMMIT", "END", "ROLLBACK"});
    constexpr auto CLOSING_MODIFIERS = std::to_array<std::string_view>({"AND", "CHAIN", "NO", "TRANSACTION", "WORK"});

    // DSQL sends 40001; OC000 and OC001 as SQLSTATEs are matched too, as in the DSQL connectors.
    constexpr auto OCC_SQL_STATES = std::to_array<std::string_view>({"40001", "OC000", "OC001"});

    template <size_t N>
    bool IsOneOf(std::string_view word, const std::array<std::string_view, N>& words)
    {
        return std::ranges::find(words, word) != words.end();
    }

    bool IsSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }

    // A character of a PostgreSQL word: an ASCII letter or digit, '_', '$', or any byte past ASCII.
    bool IsWordChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$'
            || static_cast<unsigned char>(c) >= FIRST_NON_ASCII;
    }

    // The position after any whitespace, -- comments and /* */ comments (which nest in PostgreSQL) from pos. A --
    // comment ends at a line feed or a carriage return, as in PostgreSQL's lexer.
    size_t SkipSpaceAndComments(const std::string& text, size_t pos)
    {
        while (pos < text.size()) {
            if (IsSpace(text[pos])) {
                ++pos;
            } else if (text.compare(pos, 2, "--") == 0) {
                const size_t line_end = text.find_first_of("\r\n", pos);
                pos = line_end == std::string::npos ? text.size() : line_end + 1;
            } else if (text.compare(pos, 2, "/*") == 0) {
                int depth = 0;
                do {
                    if (text.compare(pos, 2, "/*") == 0) {
                        ++depth;
                        pos += 2;
                    } else if (text.compare(pos, 2, "*/") == 0) {
                        --depth;
                        pos += 2;
                    } else {
                        ++pos;
                    }
                } while (depth > 0 && pos < text.size());
            } else {
                break;
            }
        }
        return pos;
    }

    // Up to MAX_CONTROL_WORDS upper-cased words from pos, separated by whitespace and comments. Reading stops at the
    // first other character; end is set to where it stopped.
    std::vector<std::string> ReadWords(const std::string& text, size_t pos, size_t& end)
    {
        std::vector<std::string> words;
        pos = SkipSpaceAndComments(text, pos);
        while (words.size() < MAX_CONTROL_WORDS && pos < text.size() && IsWordChar(text[pos])) {
            const size_t start = pos;
            while (pos < text.size() && IsWordChar(text[pos])) {
                ++pos;
            }
            words.push_back(DsqlPluginHelper::AsciiUpper(text.substr(start, pos - start)));
            pos = SkipSpaceAndComments(text, pos);
        }
        end = pos;
        return words;
    }

    // True when no ';' comes before the trailing whitespace and ';'s, so the text can't hold a second statement. A ';'
    // anywhere else, even in a literal or a comment, counts as a batch.
    bool IsSingleStatement(const std::string& statement)
    {
        const size_t last = statement.find_last_not_of(TRAILING_CHARACTERS);
        return last != std::string::npos && statement.find(';') > last;
    }

    // Words read at the start of a statement that open a transaction block: BEGIN, START TRANSACTION, or COMMIT, END,
    // ABORT or ROLLBACK with AND CHAIN, which starts the next transaction at once.
    bool OpensTransaction(const std::vector<std::string>& words)
    {
        if (words.empty()) {
            return false;
        }
        if (IsOneOf(words.front(), OPENING_KEYWORDS)) {
            return true;
        }
        if (!IsOneOf(words.front(), CLOSING_KEYWORDS)) {
            return false;
        }
        for (size_t i = 1; i < words.size(); ++i) {
            if (words[i] == "CHAIN" && words[i - 1] != "NO") {
                return true;
            }
        }
        return false;
    }

    // A single statement that only ends the transaction block: COMMIT, END, ABORT or ROLLBACK, then only WORK,
    // TRANSACTION or AND NO CHAIN, then nothing but whitespace, comments and ';'.
    bool ClosesTransaction(const std::string& statement)
    {
        if (!IsSingleStatement(statement)) {
            return false;
        }
        size_t end = 0;
        const std::vector<std::string> words = ReadWords(statement, 0, end);
        if (words.empty() || !IsOneOf(words.front(), CLOSING_KEYWORDS)) {
            return false;
        }
        for (size_t i = 1; i < words.size(); ++i) {
            if (!IsOneOf(words[i], CLOSING_MODIFIERS)) {
                return false;
            }
        }
        return statement.find_first_not_of(TRAILING_CHARACTERS, end) == std::string::npos;
    }

    // Each parameter set of an array runs as its own transaction in autocommit, so an array that failed may have
    // committed some sets. The base driver knows the size however it was set (SQLSetStmtAttr, SQLParamOptions or the
    // APD). psqlODBC clears the statement's diagnostics on this call, so it's made before the first attempt.
    bool HasOneParameterSet(const STMT* stmt)
    {
        if (stmt->wrapped_stmt == nullptr) {
            return false;
        }
        SQLULEN paramset_size = 0;
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetStmtAttr>(
            stmt->dbc->env->driver_lib_loader, RdsFuncNames::GetStmtAttr, stmt->wrapped_stmt, SQL_ATTR_PARAMSET_SIZE,
            &paramset_size, 0, nullptr);
        return res.fn_load_success && SQL_SUCCEEDED(res.fn_result) && paramset_size <= 1;
    }

    // The OC code in the SQLSTATE or the message, or "" when there is none.
    std::string_view OccCode(const std::string& sql_state, const std::string& message)
    {
        if (sql_state == "OC000" || message.find("(OC000)") != std::string::npos) {
            return "OC000";
        }
        if (sql_state == "OC001" || message.find("(OC001)") != std::string::npos) {
            return "OC001";
        }
        return "";
    }

    // "OC000, SQLSTATE 40001": the OC code tells a data conflict (OC000) from a schema one (OC001).
    std::string DescribeConflict(const std::string& sql_state, const std::string& message)
    {
        const std::string_view code = OccCode(sql_state, message);
        return code.empty() ? "SQLSTATE " + sql_state : std::string(code) + ", SQLSTATE " + sql_state;
    }

    // "1 retry", "2 retries".
    std::string RetryCount(int retries)
    {
        return std::to_string(retries) + (retries == 1 ? " retry" : " retries");
    }

    // SQLCancel from another thread ends the wait at once.
    bool SleepFor(CancelState& cancel, std::chrono::milliseconds delay)
    {
        return cancel.WaitFor(delay);
    }

    double UniformRandom()
    {
        thread_local std::mt19937_64 engine{std::random_device{}()};
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        return distribution(engine);
    }

    std::chrono::steady_clock::time_point SteadyNow()
    {
        return std::chrono::steady_clock::now();
    }
}  // namespace

DsqlOccRetryPlugin::DsqlOccRetryPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin)
    : DsqlOccRetryPlugin(dbc, std::move(next_plugin), nullptr, nullptr, nullptr) {}

DsqlOccRetryPlugin::DsqlOccRetryPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin, SleepFunction sleep,
                                       RandomFunction random, ClockFunction clock)
    : BasePlugin(dbc, std::move(next_plugin)),
      sleep_(sleep ? std::move(sleep) : SleepFunction(SleepFor)),
      random_(random ? std::move(random) : RandomFunction(UniformRandom)),
      clock_(clock ? std::move(clock) : ClockFunction(SteadyNow))
{
    this->plugin_name_ = "DSQL_OCC_RETRY";
    std::string error;
    const std::optional<OccRetryConfig> config = OccRetryConfig::Parse(dbc->conn_attr, error);
    if (!config.has_value()) {
        throw std::invalid_argument("Invalid value specified for connection string attribute: " + error);
    }
    config_ = *config;
}

SQLRETURN DsqlOccRetryPlugin::Execute(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     StatementText,
    SQLINTEGER     TextLength)
{
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    // With autocommit off nothing is retried, and the transaction state is DefaultPlugin's and SQLEndTran's to follow.
    // Likewise on a handle reconnected to another engine: its chain keeps this plugin from its Aurora DSQL connect.
    if (stmt == nullptr || stmt->dbc == nullptr || !stmt->dbc->auto_commit || !stmt->dbc->is_dsql) {
        return ExecuteNext(StatementHandle, StatementText, TextLength);
    }
    DBC* dbc = stmt->dbc;
    // SQLExecute passes no text; RDS_SQLPrepare kept it.
    const std::string statement =
        StatementText != nullptr ? DsqlPluginHelper::StatementTextToUtf8(StatementText) : stmt->prepared_text;

    // Decided before the first attempt: asking the base driver for the parameter set size clears its diagnostics.
    const bool may_retry = config_.max_retries > 0 && dbc->transaction_status == TRANSACTION_CLOSED
        && IsRetryableStatement(statement) && HasOneParameterSet(stmt);
    const std::chrono::steady_clock::time_point started_at = clock_();
    SQLRETURN ret = ExecuteNext(StatementHandle, StatementText, TextLength);
    if (ret == SQL_ERROR && may_retry) {
        ret = RetryOccConflicts(stmt, StatementText, TextLength, started_at, clock_() - started_at);
    }

    // Transaction control DefaultPlugin doesn't record: prepared text, text that starts with a newline, a tab or a --
    // comment, and a BEGIN after a ';'. Opening counts whatever the result (a batch can fail after its BEGIN); closing
    // needs a single statement that succeeded.
    const TransactionChange change = GetTransactionChange(statement);
    if (change == TransactionChange::OPENS) {
        dbc->transaction_status = TRANSACTION_OPEN;
    } else if (change == TransactionChange::CLOSES && SQL_SUCCEEDED(ret)) {
        dbc->transaction_status = TRANSACTION_CLOSED;
    }
    return ret;
}

SQLRETURN DsqlOccRetryPlugin::RetryOccConflicts(STMT* stmt, SQLTCHAR* statement_text, SQLINTEGER text_length,
                                                std::chrono::steady_clock::time_point started_at,
                                                std::chrono::steady_clock::duration first_attempt)
{
    const std::chrono::steady_clock::time_point deadline =
        started_at + std::chrono::milliseconds(OccRetryConfig::CALL_TIME_LIMIT_MS);
    std::chrono::steady_clock::duration last_attempt = first_attempt;
    SQLRETURN ret = SQL_ERROR;
    std::string sql_state;
    std::string message;
    DsqlPluginHelper::ReadFirstRecord(stmt, sql_state, message);
    for (int retry = 1; IsOccConflict(sql_state); ++retry) {
        if (retry > config_.max_retries) {
            LOG(WARNING) << "[DSQL] OCC conflict (" << DescribeConflict(sql_state, message) << ") persisted after "
                         << RetryCount(config_.max_retries) << "; returning the error";
            return ret;
        }
        const std::chrono::milliseconds delay = GetBackoffDelay(config_, retry, random_());
        // The next attempt is taken to last as long as the last one.
        if (clock_() + delay + last_attempt > deadline) {
            const std::string why = "; a wait of " + std::to_string(delay.count()) + " ms and an attempt of "
                + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(last_attempt).count())
                + " ms would take the call past " + std::to_string(OccRetryConfig::CALL_TIME_LIMIT_MS)
                + " ms; returning the error";
            if (retry == 1) {
                LOG(WARNING) << "[DSQL] OCC conflict (" << DescribeConflict(sql_state, message) << ") not retried"
                             << why;
            } else {
                LOG(WARNING) << "[DSQL] OCC conflict (" << DescribeConflict(sql_state, message) << ") persisted after "
                             << RetryCount(retry - 1) << why;
            }
            return ret;
        }
        LOG(INFO) << "[DSQL] OCC conflict (" << DescribeConflict(sql_state, message) << "): retry " << retry << "/"
                  << config_.max_retries << " in " << delay.count() << " ms";
        if (sleep_(stmt->cancel, delay)) {
            // No attempt after it: HY008 replaces the conflict's record (the wrapper keeps one record per handle).
            LOG(INFO) << "[DSQL] OCC retry " << retry << " canceled during its wait";
            ClearError(stmt);
            stmt->err = std::make_unique<ErrInfo>("Operation canceled: SQLCancel was called while an OCC retry waited",
                ERR_OPERATION_CANCELED);
            return SQL_ERROR;
        }
        ClearError(stmt);
        const std::chrono::steady_clock::time_point attempt_started_at = clock_();
        ret = ExecuteNext(stmt, statement_text, text_length);
        last_attempt = clock_() - attempt_started_at;
        // SQLCancel came after the wait and before the base driver (DefaultPlugin's EnterBaseDriver): the wrapper's
        // HY008 says the statement never ran. The first attempt did; say which one didn't. stmt->err holds only the
        // wrapper's records, so a server-side cancel (psqlODBC's 57014) or an HY008 from psqlODBC isn't taken for it.
        if (ret == SQL_ERROR && stmt->err != nullptr && stmt->err->sqlstate == "HY008") {
            LOG(INFO) << "[DSQL] OCC retry " << retry << " canceled before it ran";
            stmt->err = std::make_unique<ErrInfo>(
                "Operation canceled: SQLCancel was called before OCC retry " + std::to_string(retry) + " ran",
                ERR_OPERATION_CANCELED);
            return SQL_ERROR;
        }
        if (ret != SQL_ERROR) {
            return ret;
        }
        DsqlPluginHelper::ReadFirstRecord(stmt, sql_state, message);
    }
    return ret;
}

bool DsqlOccRetryPlugin::IsOccConflict(const std::string& sql_state)
{
    return IsOneOf(sql_state, OCC_SQL_STATES);
}

bool DsqlOccRetryPlugin::IsRetryableStatement(const std::string& statement)
{
    if (!IsSingleStatement(statement) || SkipSpaceAndComments(statement, 0) >= statement.size()) {
        return false;
    }
    size_t end = 0;
    const std::vector<std::string> words = ReadWords(statement, 0, end);
    return words.empty() || !IsOneOf(words.front(), NOT_RETRIED_KEYWORDS);
}

DsqlOccRetryPlugin::TransactionChange DsqlOccRetryPlugin::GetTransactionChange(const std::string& statement)
{
    // Every statement of a batch starts at the beginning of the text or right after a ';'. Looking after every ';',
    // even one inside a literal or a comment, sees every real statement; the extra places can only find a transaction
    // that isn't there.
    for (size_t start = 0; start != std::string::npos;) {
        size_t end = 0;
        if (OpensTransaction(ReadWords(statement, start, end))) {
            return TransactionChange::OPENS;
        }
        const size_t semicolon = statement.find(';', start);
        start = semicolon == std::string::npos ? std::string::npos : semicolon + 1;
    }
    return ClosesTransaction(statement) ? TransactionChange::CLOSES : TransactionChange::NONE;
}

std::chrono::milliseconds DsqlOccRetryPlugin::GetBackoffDelay(const OccRetryConfig& config, int retry,
                                                              double random_unit)
{
    const int doublings = std::clamp(retry - 1, 0, MAX_DOUBLINGS);
    // 2^doublings, shifted unsigned; at most 2^31, so base_delay_ms (an int) times it fits in an int64_t.
    const auto factor = static_cast<int64_t>(uint64_t{1} << static_cast<unsigned int>(doublings));
    const int64_t delay = std::min<int64_t>(static_cast<int64_t>(config.base_delay_ms) * factor, config.max_delay_ms);
    const double jitter = static_cast<double>(delay) * config.jitter_factor * std::clamp(random_unit, 0.0, 1.0);
    return std::chrono::milliseconds(delay + static_cast<int64_t>(jitter));
}
