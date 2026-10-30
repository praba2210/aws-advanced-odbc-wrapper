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
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../../driver.h"
#include "../../odbcapi.h"
#include "../../odbcapi_rds_helper.h"
#include "../../util/logger_wrapper.h"
#include "../../util/rds_lib_loader.h"
#include "../../util/rds_strings.h"

namespace {
    // More doublings could overflow; the delay reaches any cap long before.
    constexpr int MAX_DOUBLINGS = 31;

    // The diagnostic message buffer, in characters. psqlODBC fixes a message's record size on the first read
    // (cbErrorMsgMax - 1); 512 gives its own default, 511, so this read doesn't change what the application gets.
    constexpr SQLSMALLINT DIAG_MESSAGE_CHARS = 512;

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
            words.push_back(RDS_STR_UPPER(text.substr(start, pos - start)));
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

    // The statement text as UTF-8, to its terminator: what DefaultPlugin runs (it passes SQL_NTS whatever TextLength
    // says). Unicode builds get RDS_SQLExecDirect's NUL-terminated UTF-16 copy.
    std::string StatementTextToUtf8(SQLTCHAR* text)
    {
#if UNICODE
        return ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(text));
#else
        return reinterpret_cast<const char*>(text);
#endif
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
        const RdsLibResult res = NULL_CHECK_CALL_LIB_FUNC(stmt->dbc->env->driver_lib_loader, RDS_FP_SQLGetStmtAttr,
            RDS_STR_SQLGetStmtAttr, stmt->wrapped_stmt, SQL_ATTR_PARAMSET_SIZE, &paramset_size, 0, nullptr);
        return res.fn_load_success && SQL_SUCCEEDED(res.fn_result) && paramset_size <= 1;
    }

    // The statement's first diagnostic record: the wrapper's own error if it set one, else the base driver's. Read with
    // RDS_SQLGetDiagRec, which leaves the record for the application (RDS_SQLError would use up SQLError's one record).
    void ReadDiagnostic(STMT* stmt, std::string& sql_state, std::string& message)
    {
        SQLTCHAR state_buffer[MAX_SQL_STATE_LEN * 2] = {0};
        SQLTCHAR message_buffer[DIAG_MESSAGE_CHARS * 2] = {0};
        SQLINTEGER native_error = 0;
        SQLSMALLINT message_length = 0;
        RDS_SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state_buffer, &native_error, message_buffer, DIAG_MESSAGE_CHARS,
            &message_length, true);
#if UNICODE
        sql_state = ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(state_buffer));
        message = ConvertUTF16ToUTF8(reinterpret_cast<uint16_t*>(message_buffer));
#else
        sql_state = reinterpret_cast<const char*>(state_buffer);
        message = reinterpret_cast<const char*>(message_buffer);
#endif
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

    void SleepFor(std::chrono::milliseconds delay)
    {
        std::this_thread::sleep_for(delay);
    }

    double UniformRandom()
    {
        thread_local std::mt19937_64 engine{std::random_device{}()};
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        return distribution(engine);
    }
}  // namespace

DsqlOccRetryPlugin::DsqlOccRetryPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin)
    : DsqlOccRetryPlugin(dbc, std::move(next_plugin), nullptr, nullptr) {}

DsqlOccRetryPlugin::DsqlOccRetryPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin, SleepFunction sleep,
                                       RandomFunction random)
    : BasePlugin(dbc, std::move(next_plugin)),
      sleep_(sleep ? std::move(sleep) : SleepFunction(SleepFor)),
      random_(random ? std::move(random) : RandomFunction(UniformRandom))
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
    if (stmt == nullptr || stmt->dbc == nullptr || !stmt->dbc->auto_commit) {
        return ExecuteNext(StatementHandle, StatementText, TextLength);
    }
    DBC* dbc = stmt->dbc;
    // SQLExecute passes no text; RDS_SQLPrepare kept it.
    const std::string statement = StatementText != nullptr ? StatementTextToUtf8(StatementText) : stmt->prepared_text;

    // Decided before the first attempt: asking the base driver for the parameter set size clears its diagnostics.
    const bool may_retry = config_.max_retries > 0 && dbc->transaction_status == TRANSACTION_CLOSED
        && IsRetryableStatement(statement) && HasOneParameterSet(stmt);
    SQLRETURN ret = ExecuteNext(StatementHandle, StatementText, TextLength);
    if (ret == SQL_ERROR && may_retry) {
        ret = RetryOccConflicts(stmt, StatementText, TextLength);
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

SQLRETURN DsqlOccRetryPlugin::RetryOccConflicts(STMT* stmt, SQLTCHAR* statement_text, SQLINTEGER text_length)
{
    SQLRETURN ret = SQL_ERROR;
    std::string sql_state;
    std::string message;
    ReadDiagnostic(stmt, sql_state, message);
    for (int retry = 1; IsOccConflict(sql_state); ++retry) {
        if (retry > config_.max_retries) {
            LOG(WARNING) << "[DSQL] OCC conflict (" << DescribeConflict(sql_state, message) << ") persisted after "
                         << config_.max_retries << " retries; returning the error";
            return ret;
        }
        const std::chrono::milliseconds delay = GetBackoffDelay(config_, retry, random_());
        LOG(INFO) << "[DSQL] OCC conflict (" << DescribeConflict(sql_state, message) << "): retry " << retry << "/"
                  << config_.max_retries << " in " << delay.count() << " ms";
        sleep_(delay);
        ClearError(stmt);
        ret = ExecuteNext(stmt, statement_text, text_length);
        if (ret != SQL_ERROR) {
            return ret;
        }
        ReadDiagnostic(stmt, sql_state, message);
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
    const int64_t delay = std::min<int64_t>(static_cast<int64_t>(config.base_delay_ms) << doublings,
                                            config.max_delay_ms);
    const double jitter = static_cast<double>(delay) * config.jitter_factor * std::clamp(random_unit, 0.0, 1.0);
    return std::chrono::milliseconds(delay + static_cast<int64_t>(jitter));
}
