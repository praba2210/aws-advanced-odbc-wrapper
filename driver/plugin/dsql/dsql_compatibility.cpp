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

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "../../driver.h"
#include "../../error.h"
#include "../../util/dsql_connection_validator.h"
#include "../../util/logger_wrapper.h"

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
        // No named plans, so no DEALLOCATE when a prepared statement is freed or prepared again. psqlODBC wraps that
        // DEALLOCATE in SAVEPOINT (before 18.00.0003 at every rollback level, since then at every level but 0) and
        // ignores its error. Aurora DSQL rejects SAVEPOINT, which aborts the transaction, and the COMMIT after it
        // reports success without committing anything.
        ForcedOption{.name = "UseServerSidePrepare", .key = "USESERVERSIDEPREPARE", .short_key = "C8", .value = "0",
                     .reason = "freeing a prepared statement inside a transaction can silently roll the transaction "
                               "back on Aurora DSQL"},
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
        bool on;           // the forced setting; psqlODBC reads any non-zero value as on
        const char* reason;
    };

    constexpr std::array FORCED_ATTRIBUTES = {
        ForcedAttribute{.attribute = PGOPT_SERVER_SIDE_PREPARE, .name = "SQL_ATTR_PGOPT_SERVER_SIDE_PREPARE",
                        .on = false, .reason = "freeing a prepared statement inside a transaction can silently roll "
                                               "the transaction back on Aurora DSQL"},
        ForcedAttribute{.attribute = PGOPT_USE_DECLAREFETCH, .name = "SQL_ATTR_PGOPT_USE_DECLAREFETCH", .on = false,
                        .reason = "Aurora DSQL doesn't support cursors"},
        ForcedAttribute{.attribute = PGOPT_IGNORETIMEOUT, .name = "SQL_ATTR_PGOPT_IGNORETIMEOUT", .on = true,
                        .reason = "Aurora DSQL doesn't support statement_timeout"},
    };

    const ForcedAttribute* FindForcedAttribute(SQLINTEGER attribute)
    {
        const auto it = std::ranges::find(FORCED_ATTRIBUTES, attribute, &ForcedAttribute::attribute);
        return it == FORCED_ATTRIBUTES.end() ? nullptr : &*it;
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
}  // namespace

bool DsqlCompatibility::IsDsql(const DBC* dbc)
{
    return dbc != nullptr && DsqlConnectionValidator::IsDsqlTarget(dbc->conn_attr);
}

void DsqlCompatibility::ApplyBaseDriverOptions(std::map<std::string, std::string>& conn_attr)
{
    for (const ForcedOption& option : FORCED_OPTIONS) {
        Force(conn_attr, option);
    }
}

void DsqlCompatibility::PrepareConnect(DBC* dbc)
{
    if (!IsDsql(dbc)) {
        return;
    }
    ApplyBaseDriverOptions(dbc->conn_attr);

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
        default:
            return std::nullopt;
    }
}
