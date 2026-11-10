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

#include "dsql_compatibility.h"

#include <sqlext.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "../../driver.h"
#include "../../error.h"
#include "../../odbcapi.h"
#include "../../odbcapi_rds_helper.h"
#include "../../util/auth_provider.h"
#include "../../util/connection_string_keys.h"
#include "../../util/dsql_connection_lifetime.h"
#include "../../util/logger_wrapper.h"
#include "../../util/map_utils.h"
#include "../../util/odbc_helper.h"
#include "../../util/plugin_service.h"
#include "../../util/rds_lib_loader.h"
#include "dsql_auth_plugin.h"
#include "dsql_plugin_helper.h"

namespace {
    // A psqlODBC option the wrapper sets on every Aurora DSQL connection. key and short_key are psqlODBC's keywords,
    // upper-cased as in conn_attr; they aren't wrapper keys, so they reach the base driver.
    struct ForcedOption {
        const char* name;  // psqlODBC's spelling, for the log line
        const char* key;
        const char* short_key;
        const char* value;
        const char* reason;
    };

    constexpr std::array FORCED_OPTIONS = {
        // Rollback level 0: no SAVEPOINT before the second and later statements of a transaction, none around
        // psqlODBC's own queries (psqlODBC 18.00.0003 and later), and no automatic ROLLBACK. Aurora DSQL rejects
        // SAVEPOINT.
        ForcedOption{
            .name = "Protocol",
            .key = "PROTOCOL",
            .short_key = "A1",
            .value = "7.4-0",
            .reason = "Aurora DSQL doesn't support savepoints",
        },
        // A query timeout makes psqlODBC send SET statement_timeout before the statement. Aurora DSQL rejects that
        // parameter and enforces its own limits.
        ForcedOption{
            .name = "IgnoreTimeout",
            .key = "IGNORETIMEOUT",
            .short_key = "D9",
            .value = "1",
            .reason = "Aurora DSQL doesn't support statement_timeout",
        },
        // UseDeclareFetch=1 turns every SELECT into DECLARE ... CURSOR WITH HOLD. Aurora DSQL has no cursors.
        ForcedOption{
            .name = "UseDeclareFetch",
            .key = "USEDECLAREFETCH",
            .short_key = "B6",
            .value = "0",
            .reason = "Aurora DSQL doesn't support cursors",
        },
        // No named plans, so no DEALLOCATE when a prepared statement is freed or prepared again. psqlODBC wraps that
        // DEALLOCATE in SAVEPOINT (before 18.00.0003 at every rollback level, since then at every level but 0) and
        // ignores its error. Aurora DSQL rejects SAVEPOINT, which aborts the transaction, and the COMMIT after it
        // reports success without committing anything.
        ForcedOption{
            .name = "UseServerSidePrepare",
            .key = "USESERVERSIDEPREPARE",
            .short_key = "C8",
            .value = "0",
            .reason = "freeing a prepared statement inside a transaction can silently roll the transaction back on "
                      "Aurora DSQL",
        },
        // psqlODBC's keyset-driven and updatable cursors add ctid to the select list (and keyset-driven ones refetch
        // by ctid). Aurora DSQL returns no system columns, so each such SELECT fails, and in a transaction that aborts
        // it. Off, psqlODBC makes them static and read-only (01S02) and refuses positioned updates.
        ForcedOption{
            .name = "UpdatableCursors",
            .key = "UPDATABLECURSORS",
            .short_key = "C4",
            .value = "0",
            .reason = "Aurora DSQL doesn't return system columns such as ctid, which psqlODBC's keyset-driven and "
                      "updatable cursors use",
        },
    };

    // Writes the option under its long keyword and removes the short one. The base driver's connection string is
    // sorted by key and psqlODBC reads it from left to right, so the long keyword also wins over the CX flags. Logs
    // when it replaces a different value.
    void Force(std::map<std::string, std::string>& conn_attr, const ForcedOption& option)
    {
        for (const char* key : {option.key, option.short_key}) {
            const auto it = conn_attr.find(key);
            if (it == conn_attr.end()) {
                continue;
            }
            if (it->second != option.value) {
                LOG(WARNING) << "[DSQL] Overriding " << option.name << "=" << it->second << " with " << option.value
                             << ": " << option.reason;
            }
            conn_attr.erase(it);
        }
        conn_attr.emplace(option.key, option.value);
    }

    // psqlODBC's short keyword for Database, upper-cased as in conn_attr, and Aurora DSQL's only database.
    constexpr const char* PG_KEY_DATABASE_SHORT = "DX";
    constexpr const char* DSQL_DATABASE = "postgres";

    // An integer attribute value, which ODBC passes in the pointer itself.
    SQLULEN IntegerValue(SQLPOINTER value)
    {
        return static_cast<SQLULEN>(reinterpret_cast<uintptr_t>(value));
    }

    // psqlODBC's own connection attributes for three of the forced options (psqlODBC pgapifunc.h). Set on a live
    // connection they change psqlODBC's setting for later statements, and the wrapper keeps every connection
    // attribute and sets it again after each connect, after the connection string.
    constexpr SQLINTEGER PGOPT_USE_DECLAREFETCH = 65539;
    constexpr SQLINTEGER PGOPT_SERVER_SIDE_PREPARE = 65540;
    constexpr SQLINTEGER PGOPT_IGNORETIMEOUT = 65551;

    struct ForcedAttribute {
        SQLINTEGER attribute;
        const char* name;  // psqlODBC's name, for the message
        // The forced setting. The wrapper takes any non-zero value as on. psqlODBC reads only the low byte (65539,
        // 65540) or the low 32 bits (65551), but on Aurora DSQL the value never reaches it.
        bool on;
        const char* reason;
    };

    constexpr std::array FORCED_ATTRIBUTES = {
        ForcedAttribute{
            .attribute = PGOPT_SERVER_SIDE_PREPARE,
            .name = "SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE",
            .on = false,
            .reason = "freeing a prepared statement inside a transaction can silently roll the transaction back on "
                      "Aurora DSQL",
        },
        ForcedAttribute{
            .attribute = PGOPT_USE_DECLAREFETCH,
            .name = "SQL_ATTR_PGOPT_USE_DECLAREFETCH",
            .on = false,
            .reason = "Aurora DSQL doesn't support cursors",
        },
        ForcedAttribute{
            .attribute = PGOPT_IGNORETIMEOUT,
            .name = "SQL_ATTR_PGOPT_IGNORETIMEOUT",
            .on = true,
            .reason = "Aurora DSQL doesn't support statement_timeout",
        },
    };

    const ForcedAttribute* FindForcedAttribute(SQLINTEGER attribute)
    {
        for (const ForcedAttribute& forced : FORCED_ATTRIBUTES) {
            if (forced.attribute == attribute) {
                return &forced;
            }
        }
        return nullptr;
    }

    // The answer to the application setting one of FORCED_ATTRIBUTES on an Aurora DSQL connection. psqlODBC isn't
    // called and the value isn't kept, so psqlODBC keeps the value of the connection string.
    SQLRETURN KeepForcedAttribute(DBC* dbc, const ForcedAttribute& forced, SQLULEN requested)
    {
        if ((requested != 0) == forced.on) {
            return SQL_SUCCESS;
        }
        const std::string message =
            std::string(forced.name) + " changed to " + (forced.on ? "1" : "0") + ": " + forced.reason;
        LOG(WARNING) << "[DSQL] " << message << " (requested " << requested << ")";
        dbc->err = std::make_unique<ErrInfo>(message.c_str(), WARN_OPTION_VALUE_CHANGED);
        return SQL_SUCCESS_WITH_INFO;
    }

    const std::string QUERY_TIMEOUT_CHANGED =
        "SQL_ATTR_QUERY_TIMEOUT changed to 0: Aurora DSQL doesn't support query timeouts (it limits each transaction to "
        "5 minutes)";

    // The answer to a non-zero query timeout. psqlODBC isn't called, so it keeps the timeout it had: 0, unless a
    // connection-level timeout set before connect was replayed after it. IgnoreTimeout=1 keeps psqlODBC from sending
    // SET statement_timeout either way. INFO, not WARNING: tools set a timeout on every statement.
    SQLRETURN QueryTimeoutChanged(std::unique_ptr<ErrInfo>& err, SQLULEN requested)
    {
        LOG(INFO) << "[DSQL] " << QUERY_TIMEOUT_CHANGED << " (requested " << requested << ")";
        err = std::make_unique<ErrInfo>(QUERY_TIMEOUT_CHANGED.c_str(), WARN_OPTION_VALUE_CHANGED);
        return SQL_SUCCESS_WITH_INFO;
    }

    // Aurora DSQL's only isolation level: snapshot isolation, reported as REPEATABLE READ.
    constexpr SQLUINTEGER DSQL_ISOLATION = SQL_TXN_REPEATABLE_READ;

    const std::string ISOLATION_CHANGED =
        "SQL_ATTR_TXN_ISOLATION changed to SQL_TXN_REPEATABLE_READ: Aurora DSQL runs every transaction at REPEATABLE "
        "READ";
    const std::string SERIALIZABLE_NOT_SUPPORTED =
        "SQL_TXN_SERIALIZABLE isn't supported: Aurora DSQL runs every transaction at REPEATABLE READ (snapshot "
        "isolation)";

    // ODBC lets a driver substitute a higher isolation level, never a lower one; DSQL's REPEATABLE READ allows write
    // skew, which SERIALIZABLE forbids.
    SQLRETURN SetIsolation(DBC* dbc, SQLPOINTER value)
    {
        switch (const auto level = static_cast<SQLUINTEGER>(IntegerValue(value))) {
            case DSQL_ISOLATION:
                return SQL_SUCCESS;
            case SQL_TXN_READ_UNCOMMITTED:
            case SQL_TXN_READ_COMMITTED:
                LOG(WARNING) << "[DSQL] " << ISOLATION_CHANGED << " (requested " << level << ")";
                dbc->err = std::make_unique<ErrInfo>(ISOLATION_CHANGED.c_str(), WARN_OPTION_VALUE_CHANGED);
                return SQL_SUCCESS_WITH_INFO;
            case SQL_TXN_SERIALIZABLE:
                LOG(ERROR) << "[DSQL] " << SERIALIZABLE_NOT_SUPPORTED;
                dbc->err = std::make_unique<ErrInfo>(SERIALIZABLE_NOT_SUPPORTED.c_str(), ERR_INVALID_ATTRIBUTE_VALUE);
                return SQL_ERROR;
            default: {
                const std::string message = "Invalid SQL_ATTR_TXN_ISOLATION value " + std::to_string(level);
                LOG(ERROR) << "[DSQL] " << message;
                dbc->err = std::make_unique<ErrInfo>(message.c_str(), ERR_INVALID_ATTRIBUTE_VALUE);
                return SQL_ERROR;
            }
        }
    }

    const std::string TRANSACTION_ROLLED_BACK =
        "The transaction was rolled back, not committed: a statement in it failed, and Aurora DSQL ends a transaction "
        "at its first error (it has no savepoints). Roll back after an error, and run the whole transaction again.";

    // What every statement fails with in a transaction the server has ended.
    constexpr std::string_view IN_FAILED_TRANSACTION = "25P02";

    // The end of the warning when the check can't tell: the commit goes ahead, as psqlODBC would make it.
    constexpr std::string_view COMMITTING_UNCHECKED =
        ": committing. If Aurora DSQL ended the transaction, the commit saves nothing.";

    std::string StateForLog(const std::string& state)
    {
        return state.empty() ? "no SQLSTATE" : state;
    }
}  // namespace

bool DsqlCompatibility::IsDsql(const DBC* dbc)
{
    return dbc != nullptr && dbc->is_dsql;
}

void DsqlCompatibility::ApplyBaseDriverOptions(std::map<std::string, std::string>& conn_attr)
{
    for (const ForcedOption& option : FORCED_OPTIONS) {
        Force(conn_attr, option);
    }
}

DsqlCompatibility::PreparedConnect DsqlCompatibility::PrepareConnect(DBC* dbc)
{
    if (!IsDsql(dbc)) {
        return {};
    }
    ApplyBaseDriverOptions(dbc->conn_attr);
    PreparedConnect prepared;

    // RDS_AUTH_TYPE=DSQL applied its own, stricter defaults in DsqlAuthPlugin. Another auth type (a token in PWD, a
    // secret) gets TLS where psqlODBC would connect without it, which Aurora DSQL refuses, and keeps any other mode.
    if (AuthProvider::AuthTypeFromString(MapUtils::GetStringValue(dbc->conn_attr, KEY_AUTH_TYPE, ""))
        != AuthType::DSQL) {
        prepared.tls_warning = DsqlAuthPlugin::ApplyTlsDefaults(dbc->conn_attr, DsqlAuthPlugin::TlsPolicy::OTHER_AUTH,
            DsqlAuthPlugin::DefaultRootCert(
                DsqlPluginHelper::BaseLibpqVersion(dbc->env ? dbc->env->driver_lib_loader : nullptr)));
        // Aurora DSQL has only the postgres database; without one libpq would use the user name.
        if (!dbc->conn_attr.contains(KEY_DATABASE) && !dbc->conn_attr.contains(PG_KEY_DATABASE_SHORT)) {
            dbc->conn_attr.insert_or_assign(KEY_DATABASE, DSQL_DATABASE);
        }
    }

    // A level set before connect waits in attr_map; DefaultPlugin::Connect would replay it after psqlODBC connects,
    // and psqlODBC would send SET SESSION CHARACTERISTICS.
    if (const auto it = dbc->attr_map.find(SQL_ATTR_TXN_ISOLATION); it != dbc->attr_map.end()) {
        if (const auto level = static_cast<SQLUINTEGER>(IntegerValue(it->second.first)); level != DSQL_ISOLATION) {
            LOG(WARNING) << "[DSQL] Ignoring SQL_ATTR_TXN_ISOLATION=" << level
                         << " set before connect: Aurora DSQL runs every transaction at REPEATABLE READ";
        }
        dbc->attr_map.erase(it);
    }

    // psqlODBC's attributes for the forced options, set before connect, wait in attr_map; DefaultPlugin::Connect would
    // set them again after psqlODBC connects, over the connection string.
    for (const ForcedAttribute& forced : FORCED_ATTRIBUTES) {
        if (const auto it = dbc->attr_map.find(forced.attribute); it != dbc->attr_map.end()) {
            if (const SQLULEN requested = IntegerValue(it->second.first); (requested != 0) != forced.on) {
                LOG(WARNING) << "[DSQL] Ignoring " << forced.name << "=" << requested << " set before connect: "
                             << forced.reason;
            }
            dbc->attr_map.erase(it);
        }
    }

    // The age counts from before the base connect, so it is never less than the server's; a failed connect leaves
    // none.
    const std::shared_ptr<DsqlConnectionLifetime> lifetime = dbc->dsql_lifetime;
    if (lifetime == nullptr) {
        return prepared;
    }
    lifetime->Clear();
    prepared.started_at = lifetime->Now();
    return prepared;
}

void DsqlCompatibility::ConnectSucceeded(DBC* dbc, const std::optional<DsqlConnectionLifetime::Instant>& started_at)
{
    if (dbc == nullptr || !started_at.has_value() || dbc->dsql_lifetime == nullptr) {
        return;
    }
    // RDS_InitializeConnection has checked the value, which is read again on every connect.
    std::string unused_error;
    dbc->dsql_lifetime->Start(started_at.value(), DsqlConnectionLifetime::ParseMaxLifetime(dbc->conn_attr, unused_error)
                                                      .value_or(DsqlConnectionLifetime::DEFAULT_MAX_LIFETIME));
}

std::optional<SQLRETURN> DsqlCompatibility::TrySetConnectAttr(DBC* dbc, SQLINTEGER attribute, SQLPOINTER value)
{
    switch (attribute) {
        case PGOPT_USE_DECLAREFETCH:
        case PGOPT_SERVER_SIDE_PREPARE:
        case PGOPT_IGNORETIMEOUT:
            // Never forwarded or kept: psqlODBC keeps the forced value of the connection string.
            if (const ForcedAttribute* forced = FindForcedAttribute(attribute); forced != nullptr && IsDsql(dbc)) {
                return KeepForcedAttribute(dbc, *forced, IntegerValue(value));
            }
            return std::nullopt;
        case SQL_ATTR_QUERY_TIMEOUT:
            // ODBC 2 sets the timeout for later statements this way (SQLSetConnectOption).
            if (const SQLULEN timeout = IntegerValue(value); timeout != 0 && IsDsql(dbc)) {
                return QueryTimeoutChanged(dbc->err, timeout);
            }
            return std::nullopt;
        case SQL_ATTR_TXN_ISOLATION:
            // Never forwarded: psqlODBC would send SET SESSION CHARACTERISTICS, which Aurora DSQL rejects.
            if (IsDsql(dbc)) {
                return SetIsolation(dbc, value);
            }
            return std::nullopt;
        case SQL_ATTR_AUTOCOMMIT:
            // Switching autocommit inside a transaction commits it, and psqlODBC drops that COMMIT's result
            // (CC_set_autocommit), while Aurora DSQL reports OCC conflicts at COMMIT. So commit through SQLEndTran
            // first, which also rolls back a failed transaction (25S03), and on an error leave autocommit as it is.
            // SQLEndTran takes only the connection's lock, which is recursive and held here.
            if (const SQLULEN mode = IntegerValue(value);
                (mode == SQL_AUTOCOMMIT_ON || mode == SQL_AUTOCOMMIT_OFF)
                && (mode == SQL_AUTOCOMMIT_ON) != dbc->auto_commit && IsDsql(dbc) && HasWrappedHandle(dbc)
                && dbc->conn_status == CONN_CONNECTED) {
                if (const SQLRETURN commit = RDS_SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT); !SQL_SUCCEEDED(commit)) {
                    return commit;
                }
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

std::optional<SQLRETURN> DsqlCompatibility::TrySetStmtAttr(STMT* stmt, SQLINTEGER attribute, SQLPOINTER value)
{
    const SQLULEN timeout = IntegerValue(value);
    if (attribute != SQL_ATTR_QUERY_TIMEOUT || timeout == 0) {
        return std::nullopt;
    }
    if (!IsDsql(stmt->dbc)) {
        return std::nullopt;
    }
    return QueryTimeoutChanged(stmt->err, timeout);
}

bool DsqlCompatibility::TryGetConnectAttr(const DBC* dbc, SQLINTEGER attribute, SQLPOINTER value)
{
    if (attribute != SQL_ATTR_TXN_ISOLATION || !HasWrappedHandle(dbc) || !IsDsql(dbc)) {
        return false;
    }
    if (value != nullptr) {
        *static_cast<SQLUINTEGER*>(value) = DSQL_ISOLATION;
    }
    return true;
}

bool DsqlCompatibility::TryGetInfo(DBC* dbc, SQLUSMALLINT info_type, SQLPOINTER value, SQLSMALLINT* length)
{
    if (dbc == nullptr || (info_type != SQL_DEFAULT_TXN_ISOLATION && info_type != SQL_TXN_ISOLATION_OPTION)) {
        return false;
    }
    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    if (!HasWrappedHandle(dbc) || !IsDsql(dbc)) {
        return false;
    }
    ClearError(dbc);
    dbc->base_diag_stale = true;  // the wrapper answers: psqlODBC's records are older (HasWrappedHandle was checked)
    // SQL_TXN_ISOLATION_OPTION is a bitmask of the supported levels; Aurora DSQL has one.
    if (value != nullptr) {
        *static_cast<SQLUINTEGER*>(value) = DSQL_ISOLATION;
    }
    if (length != nullptr) {
        *length = static_cast<SQLSMALLINT>(sizeof(SQLUINTEGER));
    }
    return true;
}

// SQL_SUCCESS_WITH_INFO too: psqlODBC can report failed rows of a parameter array that way.
void DsqlCompatibility::NoteStatementResult(DBC* dbc, SQLRETURN ret)
{
    if ((ret == SQL_ERROR || ret == SQL_SUCCESS_WITH_INFO) && IsDsql(dbc)
        && (!dbc->auto_commit || dbc->transaction_status == TRANSACTION_OPEN)) {
        dbc->dsql_statement_failed_in_transaction = true;
    }
}

std::optional<SQLRETURN> DsqlCompatibility::RollBackAbortedTransaction(DBC* dbc)
{
    if (!IsDsql(dbc) || !HasWrappedHandle(dbc) || !dbc->dsql_statement_failed_in_transaction.exchange(false)) {
        return std::nullopt;
    }
    const std::shared_ptr<OdbcHelper> helper = dbc->plugin_service->GetOdbcHelper();
    SQLHSTMT probe = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(helper->BaseAllocStmt(&dbc->wrapped_dbc, &probe).fn_result) || probe == SQL_NULL_HSTMT) {
        // psqlODBC reports a failed allocation on the connection.
        LOG(WARNING) << "[DSQL] A statement in this transaction failed, and the check before the commit couldn't "
                     << "allocate a statement (" << StateForLog(helper->GetDbcSqlState(dbc->wrapped_dbc)) << ")"
                     << COMMITTING_UNCHECKED;
        return std::nullopt;
    }
    // In a transaction Aurora DSQL has ended, every statement fails with 25P02.
    const bool alive = SQL_SUCCEEDED(helper->ExecDirect(&probe, "SELECT 1").fn_result);
    const std::string state = alive ? "" : helper->GetStmtSqlState(probe);
    helper->BaseFreeStmt(&probe);
    if (alive) {
        return std::nullopt;
    }
    if (state != IN_FAILED_TRANSACTION) {
        // Any other failure doesn't show that the transaction ended.
        LOG(WARNING) << "[DSQL] A statement in this transaction failed, and the check before the commit got "
                     << StateForLog(state) << ", not " << IN_FAILED_TRANSACTION << COMMITTING_UNCHECKED;
        return std::nullopt;
    }
    // psqlODBC's SQLEndTran rolls back in manual-commit mode and after a BEGIN sent as SQL text, which it marks as a
    // manual transaction.
    RdsLibLoader::CallFunctionChecked<RDS_FP_SQLEndTran>(dbc->env->driver_lib_loader, RdsFuncNames::EndTran,
        SQL_HANDLE_DBC, dbc->wrapped_dbc, SQL_ROLLBACK);
    dbc->transaction_status = TRANSACTION_CLOSED;
    LOG(WARNING) << "[DSQL] " << TRANSACTION_ROLLED_BACK << " (the check before the commit got " << state << ")";
    dbc->err = std::make_unique<ErrInfo>(TRANSACTION_ROLLED_BACK.c_str(), ERR_TRANSACTION_IS_ROLLED_BACK);  // 25S03
    return SQL_ERROR;
}
