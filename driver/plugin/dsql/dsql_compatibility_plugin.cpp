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

#include "dsql_compatibility_plugin.h"

#include <sqlext.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "../../driver.h"
#include "../../error.h"
#include "../../util/auth_provider.h"
#include "../../util/connection_string_keys.h"
#include "../../util/dsql_connection_validator.h"
#include "../../util/logger_wrapper.h"
#include "../../util/map_utils.h"
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
        "isolation). Use SQL_TXN_REPEATABLE_READ, and read the rows a decision depends on with SELECT ... FOR UPDATE.";

    std::string IsolationName(SQLUINTEGER level)
    {
        switch (level) {
            case SQL_TXN_READ_UNCOMMITTED:
                return "SQL_TXN_READ_UNCOMMITTED";
            case SQL_TXN_READ_COMMITTED:
                return "SQL_TXN_READ_COMMITTED";
            case SQL_TXN_SERIALIZABLE:
                return "SQL_TXN_SERIALIZABLE";
            default:
                return std::to_string(level);
        }
    }

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
}  // namespace

DsqlCompatibilityPlugin::DsqlCompatibilityPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin)
    : BasePlugin(dbc, std::move(next_plugin))
{
    this->plugin_name_ = "DSQL_COMPATIBILITY";
}

std::shared_ptr<BasePlugin> DsqlCompatibilityPlugin::AddToChain(DBC* dbc, std::shared_ptr<BasePlugin> next)
{
    if (!DsqlConnectionValidator::IsDsqlTarget(dbc->conn_attr)) {
        return next;
    }
    return std::make_shared<DsqlCompatibilityPlugin>(dbc, std::move(next));
}

void DsqlCompatibilityPlugin::ApplyBaseDriverOptions(std::map<std::string, std::string>& conn_attr)
{
    for (const ForcedOption& option : FORCED_OPTIONS) {
        Force(conn_attr, option);
    }
}

SQLRETURN DsqlCompatibilityPlugin::Connect(
    SQLHDBC        ConnectionHandle,
    SQLHWND        WindowHandle,
    SQLTCHAR *     OutConnectionString,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr,
    SQLUSMALLINT   DriverCompletion)
{
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    // What a failed base connect puts back, so that a retry within the connect (Secrets Manager's) or a later connect
    // on the handle starts again from the application's settings and reports the same changes.
    std::map<std::string, std::string> conn_attr_as_set = dbc->conn_attr;
    auto attr_map_as_set = dbc->attr_map;
    ApplyBaseDriverOptions(dbc->conn_attr);
    // The 01S02 text for what this connect changes.
    std::string notice;

    // RDS_AUTH_TYPE=DSQL applied its own, stricter defaults in DsqlAuthPlugin. With another auth type (a token in PWD,
    // a secret) a mode psqlODBC runs works today and is left as set. One it would run as disable, which Aurora DSQL
    // refuses, gets TLS. Either way the connect gets the database default, without which it can't work.
    if (AuthProvider::AuthTypeFromString(MapUtils::GetStringValue(dbc->conn_attr, KEY_AUTH_TYPE, "")) != AuthType::DSQL) {
        notice = DsqlAuthPlugin::ApplyTlsDefaults(dbc->conn_attr, DsqlAuthPlugin::TlsPolicy::OTHER_AUTH,
            DsqlAuthPlugin::DefaultRootCert(
                DsqlPluginHelper::BaseLibpqVersion(dbc->env ? dbc->env->driver_lib_loader : nullptr)));
        DsqlAuthPlugin::ApplyDatabaseDefault(dbc->conn_attr);
    }

    // A level set before connect waits in attr_map; DefaultPlugin::Connect would replay it after psqlODBC connects,
    // and psqlODBC would send SET SESSION CHARACTERISTICS. The app is told, as ODBC has it for a substituted value.
    if (const auto it = dbc->attr_map.find(SQL_ATTR_TXN_ISOLATION); it != dbc->attr_map.end()) {
        if (const auto level = static_cast<SQLUINTEGER>(IntegerValue(it->second.first)); level != DSQL_ISOLATION) {
            const std::string changed = "SQL_ATTR_TXN_ISOLATION=" + IsolationName(level)
                + ", set before connect, was changed to SQL_TXN_REPEATABLE_READ: Aurora DSQL runs every transaction at "
                  "REPEATABLE READ.";
            LOG(WARNING) << "[DSQL] " << changed;
            notice += notice.empty() ? changed : " " + changed;
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

    SQLRETURN ret = ConnectNext(
        ConnectionHandle, WindowHandle, OutConnectionString, BufferLength, StringLengthPtr, DriverCompletion);
    if (!SQL_SUCCEEDED(ret)) {
        dbc->conn_attr = std::move(conn_attr_as_set);
        dbc->attr_map = std::move(attr_map_as_set);
        return ret;
    }
    // The wrapper keeps one record per handle, so the changes are reported, in one record, only when nothing else is,
    // as DsqlAuthPlugin does.
    if (ret == SQL_SUCCESS && !notice.empty()) {
        ClearError(dbc);
        dbc->err = std::make_unique<ErrInfo>(notice.c_str(), WARN_OPTION_VALUE_CHANGED);
        ret = SQL_SUCCESS_WITH_INFO;
    }
    return ret;
}

std::optional<SQLRETURN> DsqlCompatibilityPlugin::TrySetConnectAttr(
    SQLHDBC        ConnectionHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     StringLength)
{
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    if (const ForcedAttribute* forced = FindForcedAttribute(Attribute)) {
        // Never forwarded or kept: psqlODBC keeps the forced value of the connection string.
        return KeepForcedAttribute(dbc, *forced, IntegerValue(ValuePtr));
    }
    switch (Attribute) {
        case SQL_ATTR_QUERY_TIMEOUT:
            // ODBC 2 sets the timeout for later statements this way (SQLSetConnectOption).
            if (const SQLULEN timeout = IntegerValue(ValuePtr); timeout != 0) {
                return QueryTimeoutChanged(dbc->err, timeout);
            }
            break;
        case SQL_ATTR_TXN_ISOLATION:
            // Never forwarded: psqlODBC would send SET SESSION CHARACTERISTICS, which Aurora DSQL rejects.
            return SetIsolation(dbc, ValuePtr);
        default:
            break;
    }
    return BasePlugin::TrySetConnectAttr(ConnectionHandle, Attribute, ValuePtr, StringLength);
}

std::optional<SQLRETURN> DsqlCompatibilityPlugin::TryGetInfo(
    SQLHDBC        ConnectionHandle,
    SQLUSMALLINT   InfoType,
    SQLPOINTER     InfoValuePtr,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr)
{
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    if (InfoType != SQL_TXN_ISOLATION_OPTION || !HasWrappedHandle(dbc)) {
        return BasePlugin::TryGetInfo(ConnectionHandle, InfoType, InfoValuePtr, BufferLength, StringLengthPtr);
    }
    ClearError(dbc);
    // SQL_TXN_ISOLATION_OPTION is a bitmask of the supported levels; Aurora DSQL has one.
    if (InfoValuePtr != nullptr) {
        *static_cast<SQLUINTEGER*>(InfoValuePtr) = DSQL_ISOLATION;
    }
    if (StringLengthPtr != nullptr) {
        *StringLengthPtr = static_cast<SQLSMALLINT>(sizeof(SQLUINTEGER));
    }
    return SQL_SUCCESS;
}

std::optional<SQLRETURN> DsqlCompatibilityPlugin::TrySetStmtAttr(
    SQLHSTMT       StatementHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     StringLength)
{
    // On Aurora DSQL a non-zero query timeout gets 01S02 and doesn't reach psqlODBC, which wouldn't apply it
    // (IgnoreTimeout=1): Aurora DSQL rejects SET statement_timeout.
    if (const SQLULEN timeout = IntegerValue(ValuePtr); Attribute == SQL_ATTR_QUERY_TIMEOUT && timeout != 0) {
        return QueryTimeoutChanged(static_cast<STMT*>(StatementHandle)->err, timeout);
    }
    return BasePlugin::TrySetStmtAttr(StatementHandle, Attribute, ValuePtr, StringLength);
}
