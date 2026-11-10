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

#include "odbcapi_rds_helper.h"

#include "util/auth_provider.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "error.h"
#include "plugin/aurora_initial_connection_strategy/aurora_initial_connection_strategy_plugin.h"
#include "plugin/base_plugin.h"
#include "plugin/blue_green/blue_green_plugin.h"
#include "plugin/custom_endpoint/custom_endpoint_plugin.h"
#include "plugin/default_plugin.h"
#include "plugin/dsql/dsql_auth_plugin.h"
#include "plugin/dsql/dsql_compatibility.h"
#include "plugin/dsql/dsql_error_hint_plugin.h"
#include "plugin/dsql/dsql_occ_retry_plugin.h"
#include "plugin/failover/failover_plugin.h"
#include "plugin/federated/adfs_auth_plugin.h"
#include "plugin/federated/aws_sso_auth_plugin.h"
#include "plugin/federated/okta_auth_plugin.h"
#include "plugin/iam/iam_auth_plugin.h"
#include "plugin/limitless/limitless_plugin.h"
#include "plugin/read_write_splitting/read_write_splitting_plugin.h"
#include "plugin/read_write_splitting/simple_read_write_splitting_plugin.h"
#include "plugin/secrets_manager/secrets_manager_plugin.h"
#include "util/attribute_validator.h"
#include "util/connection_string_helper.h"
#include "util/connection_string_keys.h"
#include "util/core_service.h"
#include "util/dsql_connection_lifetime.h"
#include "util/dsql_connection_validator.h"
#include "util/logger_wrapper.h"
#include "util/map_utils.h"
#include "util/odbc_dsn_helper.h"
#include "util/odbc_helper.h"
#include "util/plugin_service.h"
#include "util/rds_lib_loader.h"
#include "util/rds_strings.h"

#ifdef WIN32
    #include "gui/setup.h"
#endif

#if UNICODE && !defined(_WIN32)
    #include <dlfcn.h>
#endif

namespace {
// The return code of a call whose string the wrapper converted for the app (OdbcHelper::FetchDriverString): the base
// driver's, or SQL_SUCCESS_WITH_INFO with the wrapper's 01004 when only the conversion cut the string.
template <typename Handle>
SQLRETURN ReportConversionCut(Handle* handle, SQLRETURN ret, bool truncated, const char* value, size_t needed, const char* unit) {
    if (!truncated || ret != SQL_SUCCESS) {
        return ret;
    }
    const std::string message = std::string(value) + " was truncated: the buffer needs " + std::to_string(needed) + " " + unit;
    ClearError(handle);
    handle->err = std::make_unique<ErrInfo>(message.c_str(), WARN_STRING_DATA_RIGHT_TRUNCATED);
    return SQL_SUCCESS_WITH_INFO;
}
}  // namespace

ENV* RDS_GetEnvFromHandle(SQLSMALLINT HandleType, SQLHANDLE InputHandle) {
    if (!InputHandle) {
        return nullptr;
    }
    switch (HandleType) {
        case SQL_HANDLE_ENV:
            return static_cast<ENV*>(InputHandle);
        case SQL_HANDLE_DBC: {
            const DBC* dbc = static_cast<const DBC*>(InputHandle);
            return dbc ? dbc->env : nullptr;
        }
        case SQL_HANDLE_STMT: {
            const STMT* stmt = static_cast<const STMT*>(InputHandle);
            return (stmt && stmt->dbc) ? stmt->dbc->env : nullptr;
        }
        case SQL_HANDLE_DESC: {
            const DESC* desc = static_cast<const DESC*>(InputHandle);
            return (desc && desc->dbc) ? desc->dbc->env : nullptr;
        }
        default:
            return nullptr;
    }
}

#if UNICODE
// codechecker_suppress [readability-identifier-naming]
std::shared_ptr<OdbcHelper> RDS_GetOdbcHelper(const DBC* dbc, const ENV* env) {
    if (dbc && dbc->plugin_service) {
        return dbc->plugin_service->GetOdbcHelper();
    }
    if (env && !env->dbc_list.empty()) {
        const DBC* env_dbc = env->dbc_list.front();
        if (env_dbc && env_dbc->plugin_service) {
            return env_dbc->plugin_service->GetOdbcHelper();
        }
    }
    return nullptr;
}

// codechecker_suppress [readability-identifier-naming]
std::shared_ptr<OdbcHelper> RDS_GetOdbcHelper(
    SQLSMALLINT         HandleType,
    SQLHANDLE           InputHandle)
{
    switch (HandleType) {
        case SQL_HANDLE_ENV:
            return RDS_GetOdbcHelper(nullptr, static_cast<ENV*>(InputHandle));
        case SQL_HANDLE_DBC:
            return RDS_GetOdbcHelper(static_cast<DBC*>(InputHandle), nullptr);
        case SQL_HANDLE_STMT:
            return (static_cast<STMT*>(InputHandle))->dbc->plugin_service->GetOdbcHelper();
        case SQL_HANDLE_DESC:
            return (static_cast<DESC*>(InputHandle))->dbc->plugin_service->GetOdbcHelper();
        default:
            break;
    }

    return nullptr;
}
#endif

SQLRETURN RDS_ProcessLibRes(
    SQLSMALLINT         HandleType,
    SQLHANDLE           InputHandle,
    const RdsLibResult& LibResult)
{
    // The base driver ran this call: its diagnostics are current.
    if (HandleType == SQL_HANDLE_ENV && InputHandle) {
        static_cast<ENV*>(InputHandle)->base_diag_stale = false;
    } else if (HandleType == SQL_HANDLE_DBC && InputHandle) {
        static_cast<DBC*>(InputHandle)->base_diag_stale = false;
    }
    if (!LibResult.fn_load_success) {
        auto new_err = std::make_unique<ErrInfo>(
            "Underlying driver failed to load/execute: " + LibResult.fn_name,
            ERR_NO_UNDER_LYING_FUNCTION);
        LOG(ERROR) << new_err->error_msg;
        switch (HandleType) {
            case SQL_HANDLE_ENV:
                {
                    ENV *env = static_cast<ENV*>(InputHandle);
                    ClearError(env);
                    env->err = std::move(new_err);
                    break;
                }
            case SQL_HANDLE_DBC:
                {
                    DBC *dbc = static_cast<DBC*>(InputHandle);
                    ClearError(dbc);
                    dbc->err = std::move(new_err);
                    break;
                }
            case SQL_HANDLE_STMT:
                {
                    STMT *stmt = static_cast<STMT*>(InputHandle);
                    ClearError(stmt);
                    stmt->err = std::move(new_err);
                    break;
                }
            case SQL_HANDLE_DESC:
                {
                    DESC *desc = static_cast<DESC*>(InputHandle);
                    ClearError(desc);
                    desc->err = std::move(new_err);
                    break;
                }
            default:
                return SQL_INVALID_HANDLE;
        }
    }
    return LibResult.fn_result;
}

namespace {
    SQLRETURN AllocEnv(SQLHENV* EnvironmentHandlePointer, const bool count_in_core_service)
    {
        try {
            std::unique_ptr<ENV> env = std::make_unique<ENV>();
            env->logger_wrapper = std::make_shared<LoggerWrapper>();
            if (count_in_core_service) {
                // After the logger, so the core services can log while being created. Last step that can throw,
                // so a failure here leaves nothing to undo
                CoreService::Instance().OnEnvAllocated();
                env->counted_by_core_service = true;
            }
            *EnvironmentHandlePointer = env.release();
            return SQL_SUCCESS;
        } catch (const std::exception& ex) {
            // Must not escape into the Driver Manager. There is no handle yet to record a diagnostic on
            LOG(ERROR) << "Unable to allocate environment handle: " << ex.what();
            return SQL_ERROR;
        }
    }
}  // namespace

SQLRETURN RDS_AllocEnv(
    SQLHENV *      EnvironmentHandlePointer)
{
    return AllocEnv(EnvironmentHandlePointer, true);
}

SQLRETURN RDS_AllocInternalEnv(
    SQLHENV *      EnvironmentHandlePointer)
{
    return AllocEnv(EnvironmentHandlePointer, false);
}

SQLRETURN RDS_AllocDbc(
    SQLHENV        EnvironmentHandle,
    SQLHDBC *      ConnectionHandlePointer)
{
    if (!HasEnvAccess<ENV>(EnvironmentHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DBC *dbc;
    ENV *env = static_cast<ENV*>(EnvironmentHandle);

    dbc = new DBC();
    dbc->env = env;
    *ConnectionHandlePointer = dbc;

    {
        const std::lock_guard<std::recursive_mutex> lock_guard(env->lock);
        env->dbc_list.emplace_back(dbc);
    }

    return SQL_SUCCESS;
}

SQLRETURN RDS_AllocStmt(
    SQLHDBC        ConnectionHandle,
    SQLHSTMT *     StatementHandlePointer)
{
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt;
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    const ENV* env = dbc->env;

    if (!HasWrappedHandle(dbc)) {
        return SQL_INVALID_HANDLE;
    }

    stmt = new STMT();
    stmt->dbc = dbc;
    // Create underlying driver's statement handle
    SQLHSTMT wrapped_stmt = SQL_NULL_HSTMT;
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLAllocHandle>(env->driver_lib_loader, RdsFuncNames::AllocHandle,
        SQL_HANDLE_STMT, dbc->wrapped_dbc, &wrapped_stmt
    );
    dbc->base_diag_stale = false;  // the base driver's diagnostics for this call are on the connection
    RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
    // On failure, keep NULL so later calls fail with SQL_INVALID_HANDLE.
    if (SQL_SUCCEEDED(res.fn_result)) {
        OdbcHelper::SetWrappedStmt(stmt, wrapped_stmt);
    }
    *StatementHandlePointer = stmt;

    stmt->app_row_desc = new DESC();
    stmt->app_row_desc->dbc = dbc;
    stmt->app_row_desc->stmt = stmt;
    stmt->app_param_desc = new DESC();
    stmt->app_param_desc->dbc = dbc;
    stmt->app_param_desc->stmt = stmt;
    stmt->imp_row_desc = new DESC();
    stmt->imp_row_desc->dbc = dbc;
    stmt->imp_row_desc->stmt = stmt;
    stmt->imp_param_desc = new DESC();
    stmt->imp_param_desc->dbc = dbc;
    stmt->imp_param_desc->stmt = stmt;

    {
        // Under the connection's lock, as every other use of the list: an app may use one connection from several
        // threads, and SQLFreeHandle, a failover and a disconnect walk or change the list meanwhile.
        const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
        dbc->stmt_list.emplace_back(stmt);
    }

    return SQL_SUCCESS;
}

SQLRETURN RDS_AllocDesc(
    SQLHDBC        ConnectionHandle,
    SQLHANDLE *    DescriptorHandlePointer)
{
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DESC* desc;
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    const ENV* env = dbc->env;

    if (!HasWrappedHandle(dbc)) {
        return SQL_INVALID_HANDLE;
    }

    desc = new DESC();
    desc->dbc = dbc;
    // Create underlying driver's descriptor handle
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLAllocHandle>(env->driver_lib_loader, RdsFuncNames::AllocHandle,
        SQL_HANDLE_DESC, dbc->wrapped_dbc, &desc->wrapped_desc
    );
    dbc->base_diag_stale = false;  // the base driver's diagnostics for this call are on the connection
    RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
    if (!SQL_SUCCEEDED(res.fn_result)) {
        // Keep NULL so later calls fail with SQL_INVALID_HANDLE.
        desc->wrapped_desc = SQL_NULL_HDESC;
    }
    *DescriptorHandlePointer = desc;

    {
        // Under the connection's lock, as every other use of the list (see RDS_AllocStmt).
        const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
        dbc->desc_list.emplace_back(desc);
    }

    return SQL_SUCCESS;
}

SQLRETURN RDS_SQLSetEnvAttr(
    SQLHENV        EnvironmentHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     StringLength)
{
    if (!HasEnvAccess<ENV>(EnvironmentHandle)) {
        return SQL_INVALID_HANDLE;
    }
    ENV* env = static_cast<ENV*>(EnvironmentHandle);
    SQLRETURN ret = SQL_SUCCESS;

    const std::lock_guard<std::recursive_mutex> lock_guard(env->lock);

    // Track new value
    env->attr_map.insert_or_assign(Attribute, std::make_pair(ValuePtr, StringLength));

    // Check if underlying library is loaded
    //  Don't fail if it isn't loaded as
    //  this can be called prior to connecting
    if (env->driver_lib_loader && env->wrapped_env) {
        // Update existing connections environments
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetEnvAttr>(env->driver_lib_loader, RdsFuncNames::SetEnvAttr,
            env->wrapped_env, Attribute, ValuePtr, StringLength
        );
        ret = RDS_ProcessLibRes(SQL_HANDLE_ENV, env, res);
    }

    return ret;
}

SQLRETURN RDS_SQLGetEnvAttr(
    SQLHENV        EnvironmentHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     BufferLength,
    SQLINTEGER *   StringLengthPtr)
{
    if (!HasEnvAccess<ENV>(EnvironmentHandle)) {
        return SQL_INVALID_HANDLE;
    }
    ENV* env = static_cast<ENV*>(EnvironmentHandle);

    const std::lock_guard<std::recursive_mutex> lock_guard(env->lock);
    // The wrapper answers this call itself: it leaves no record, and the base driver's are an earlier call's.
    ClearError(env);
    env->base_diag_stale = env->wrapped_env != nullptr;

    if (env->attr_map.contains(Attribute)) {
        const std::pair<SQLPOINTER, SQLINTEGER> value_pair = env->attr_map.at(Attribute);
        const SQLINTEGER length = value_pair.second;
        // The standard environment attributes are all integers, whatever length they were set with. Another one is an
        // integer when its length says so (0, its size or an SQL_IS_* value), as SQLGetConnectAttr reads them, and a
        // string only for SQL_NTS or a byte count.
        bool standard_integer = false;
        switch (Attribute) {
            case SQL_ATTR_CONNECTION_POOLING:
            case SQL_ATTR_CP_MATCH:
            case SQL_ATTR_ODBC_VERSION:
            case SQL_ATTR_OUTPUT_NTS:
                standard_integer = true;
                break;
            default:
                break;
        }
        if (ValuePtr == nullptr) {
            // Nowhere to write it
        } else if (!standard_integer && length == SQL_IS_POINTER) {
            *(static_cast<SQLPOINTER*>(ValuePtr)) = value_pair.first;
        } else if (!standard_integer
            && (length == sizeof(SQLSMALLINT) || length == SQL_IS_SMALLINT || length == SQL_IS_USMALLINT)) {
            *(static_cast<SQLUSMALLINT*>(ValuePtr)) = static_cast<SQLUSMALLINT>(reinterpret_cast<uintptr_t>(value_pair.first));
        } else if (standard_integer || length == sizeof(SQLUINTEGER) || length == 0 || length == SQL_IS_UINTEGER
            || length == SQL_IS_INTEGER) {
            *(static_cast<SQLUINTEGER*>(ValuePtr)) = static_cast<SQLUINTEGER>(reinterpret_cast<uintptr_t>(value_pair.first));
        } else {
            snprintf(static_cast<char*>(ValuePtr), static_cast<size_t>(BufferLength), "%s", static_cast<char*>(value_pair.first));
            if (length >= BufferLength) {
                return SQL_SUCCESS_WITH_INFO;
            }
        }
        return SQL_SUCCESS;
    }
    // Not set: the ODBC default of the attributes that have one. SQL_ATTR_ODBC_VERSION has none, the app sets it before
    // it allocates a connection (HY010); HY092 for any other identifier.
    SQLUINTEGER default_value = 0;
    switch (Attribute) {
        // SQL_CP_OFF for the pooling and SQL_CP_STRICT_MATCH for the match, which are the same value.
        case SQL_ATTR_CONNECTION_POOLING:
        case SQL_ATTR_CP_MATCH:
            static_assert(SQL_CP_OFF == SQL_CP_STRICT_MATCH);
            default_value = SQL_CP_OFF;
            break;
        case SQL_ATTR_OUTPUT_NTS:
            default_value = SQL_TRUE;
            break;
        case SQL_ATTR_ODBC_VERSION:
            env->err = std::make_unique<ErrInfo>("SQLGetEnvAttr - SQL_ATTR_ODBC_VERSION isn't set on this environment", ERR_FUNCTION_SEQUENCE_ERROR);
            return SQL_ERROR;
        default:
            env->err = std::make_unique<ErrInfo>("SQLGetEnvAttr - Invalid attribute identifier", ERR_INVALID_ATTRIBUTE_OPTION_IDENTIFIER);
            return SQL_ERROR;
    }
    if (ValuePtr != nullptr) {
        *static_cast<SQLUINTEGER*>(ValuePtr) = default_value;
    }
    return SQL_SUCCESS;
}

SQLRETURN RDS_SQLEndTran(
    SQLSMALLINT    HandleType,
    SQLHANDLE      Handle,
    SQLSMALLINT    CompletionType)
{
    SQLRETURN ret = SQL_SUCCESS;
    ENV* env = nullptr;
    DBC* dbc = nullptr;
    RdsLibResult res;
    switch (HandleType) {
        case SQL_HANDLE_DBC:
            if (!HasEnvAccess<DBC>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                dbc = static_cast<DBC*>(Handle);
                env = dbc->env;

                const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
                ClearError(dbc);

                if (!HasWrappedHandle(dbc)) {
                    return SQL_INVALID_HANDLE;
                }
                res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLEndTran>(env->driver_lib_loader, RdsFuncNames::EndTran,
                    HandleType, dbc->wrapped_dbc, CompletionType
                );
                ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
                if (SQL_SUCCEEDED(ret) && dbc) {
                    dbc->transaction_status = TRANSACTION_CLOSED;
                }
            }
            break;
        case SQL_HANDLE_ENV:
            if (!HasEnvAccess<ENV>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                env = static_cast<ENV*>(Handle);

                const std::lock_guard<std::recursive_mutex> lock_guard(env->lock);
                ClearError(env);

                for (DBC* dbc : env->dbc_list) {
                    // TODO - May need enhanced error handling due to multiple DBCs
                    //   Should error out on the first?
                    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
                    ClearError(dbc);

                    if (!HasWrappedHandle(dbc)) {
                        return SQL_INVALID_HANDLE;
                    }
                    res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLEndTran>(env->driver_lib_loader, RdsFuncNames::EndTran,
                        SQL_HANDLE_DBC, dbc->wrapped_dbc, CompletionType
                    );
                    ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
                    if (SQL_SUCCEEDED(ret) && dbc) {
                        dbc->transaction_status = TRANSACTION_CLOSED;
                    }
                }

                res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLEndTran>(env->driver_lib_loader, RdsFuncNames::EndTran,
                    HandleType, env->wrapped_env, CompletionType
                );
                ret = RDS_ProcessLibRes(HandleType, env, res);
            }
            break;
        default:
            // TODO - Set error
            ret = SQL_ERROR;
            break;
    }
    return ret;
}

SQLRETURN RDS_FreeConnect(
    SQLHDBC        ConnectionHandle)
{
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    ENV* env = dbc->env;

    // Remove connection from environment
    {
        const std::lock_guard<std::recursive_mutex> lock_guard(env->lock);
        env->dbc_list.remove(dbc); // TODO - Make this into a function within ENV to make use of locks
    }

    if (dbc->plugin_head) {
        dbc->plugin_head->ReleaseResources();
    }

    if (dbc->plugin_service) {
        dbc->plugin_service->ReleaseHostListProvider();
    }

    // Cleanup tracked statements and descriptors, under the connection's lock as every other use of the lists. The
    // lock goes before the connection does.
    {
        const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
        const std::list<STMT*> stmt_list = dbc->stmt_list;
        for (STMT* stmt : stmt_list) {
            RDS_FreeStmt(stmt, SQL_DROP);
        }
        dbc->stmt_list.clear();
        // and descriptors
        const std::list<DESC*> desc_list = dbc->desc_list;
        for (DESC* desc : desc_list) {
            RDS_FreeDesc(desc);
        }
        dbc->desc_list.clear();
    }

    // Clean up wrapped DBC, through base_handles as every base disconnect: an SQLCancel from another thread can still
    // be in the base driver, and then the free runs when it returns, after the statements' (the DBC, deleted below,
    // leaves base_handles' teardowns to that call).
    if (dbc->wrapped_dbc) {
        const SQLHDBC wrapped_dbc = dbc->wrapped_dbc;
        dbc->wrapped_dbc = nullptr;
        dbc->base_handles.RunOrDefer([lib_loader = env->driver_lib_loader, wrapped_dbc] {
            const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeHandle>(lib_loader, RdsFuncNames::FreeHandle,
                SQL_HANDLE_DBC, wrapped_dbc
            );
            if (!res.fn_load_success || !SQL_SUCCEEDED(res.fn_result)) {
                LOG(WARNING) << "Unable to free the underlying connection handle";
            }
        });
    }

    ClearError(dbc);
    delete dbc;
    ConnectionHandle = nullptr;
    return SQL_SUCCESS;
}

SQLRETURN RDS_FreeDesc(
    SQLHDESC       DescriptorHandle)
{
    if (!HasEnvAccess<DESC>(DescriptorHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DESC* desc = static_cast<DESC*>(DescriptorHandle);
    if (desc->stmt != nullptr) {
        // A statement's implicit descriptor: SQLFreeHandle on its statement frees it.
        const std::lock_guard<std::recursive_mutex> lock_guard(desc->lock);
        ClearError(desc);
        desc->err = std::make_unique<ErrInfo>("SQLFreeHandle - Invalid use of an automatically allocated descriptor handle",
            ERR_INVALID_USE_OF_AN_AUTOMATICALLY_ALLOCATED_DESCRIPTOR_HANDLE);
        return SQL_ERROR;
    }
    DBC* dbc = desc->dbc;
    const ENV* env = dbc->env;

    // Remove descriptor from connection, and put the statements that use it back on their implicit descriptors (ODBC),
    // in the base driver too, before their base statements are left with a freed descriptor.
    {
        const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
        dbc->desc_list.remove(desc);
        for (STMT* stmt : dbc->stmt_list) {
            const std::lock_guard<std::recursive_mutex> stmt_lock_guard(stmt->lock);
            for (const auto& [attribute, explicit_desc] : {
                     std::pair{SQL_ATTR_APP_ROW_DESC, &stmt->explicit_row_desc},
                     std::pair{SQL_ATTR_APP_PARAM_DESC, &stmt->explicit_param_desc},
                 }) {
                if (*explicit_desc != desc) {
                    continue;
                }
                *explicit_desc = nullptr;
                if (stmt->wrapped_stmt) {
                    OdbcHelper::SetWrappedAppDesc(env->driver_lib_loader, stmt, attribute, nullptr);
                }
            }
        }
    }

    // Clean underlying Descriptors
    if (desc->wrapped_desc) {
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeHandle>(env->driver_lib_loader, RdsFuncNames::FreeHandle,
            SQL_HANDLE_DESC, desc->wrapped_desc
        );
        RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
        desc->wrapped_desc = nullptr;
    }

    // With it go its copies of the SQL_C_WCHAR bindings, after the base descriptor whose records point at them. The
    // statements that used it are back on their implicit descriptors, whose copies are as they were.
    ClearError(desc);
    delete desc;
    DescriptorHandle = nullptr;
    return SQL_SUCCESS;
}

SQLRETURN RDS_FreeEnv(
    SQLHENV        EnvironmentHandle)
{
    if (!HasEnvAccess<ENV>(EnvironmentHandle)) {
        return SQL_INVALID_HANDLE;
    }
    ENV* env = static_cast<ENV*>(EnvironmentHandle);

    // Clear from live to avoid double free from plugins
    while (!env->dbc_list.empty()) {
        RDS_FreeConnect(env->dbc_list.front());
    }

    if (env->driver_lib_loader) {
        // Clean underlying Env
        if (env->wrapped_env) {
            const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeHandle>(env->driver_lib_loader, RdsFuncNames::FreeHandle,
                SQL_HANDLE_ENV, env->wrapped_env
            );
            RDS_ProcessLibRes(SQL_HANDLE_ENV, env, res);
            env->wrapped_env = nullptr;
        }
        env->driver_lib_loader = nullptr;
    }

    ClearError(env);
    if (env->counted_by_core_service) {
        // After the child handles, but before deleting the ENV, which may drop the last logger reference
        CoreService::Instance().OnEnvFreed();
    }
    delete env;
    EnvironmentHandle = nullptr;
    return SQL_SUCCESS;
}

SQLRETURN RDS_FreeStmt(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   Option)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    switch (Option) {
        case SQL_CLOSE:
        case SQL_UNBIND:
        case SQL_RESET_PARAMS:
            {
                SQLRETURN ret = SQL_ERROR;
                if (stmt->wrapped_stmt) {
                    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLFreeStmt>(env->driver_lib_loader, RdsFuncNames::FreeStmt,
                        stmt->wrapped_stmt, Option
                    );
                    ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
                }

                // Let underlying driver cleanup before we remove any of our data: the copies of the ARD or APD in use,
                // whose records it emptied. All of them: a descriptor that is also another statement's APD or ARD has
                // one set of records. The other descriptors' records still point at theirs. Without a base statement,
                // the implicit descriptor's records are gone with it.
                if (Option == SQL_UNBIND || Option == SQL_RESET_PARAMS) {
                    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
                    if (Option == SQL_RESET_PARAMS) {
                        stmt->put_data_char_conversion = false;
                    }
                    DESC* desc = Option == SQL_UNBIND ? CurrentRowDesc(stmt) : CurrentParamDesc(stmt);
                    if (desc != nullptr && (SQL_SUCCEEDED(ret) || (!stmt->wrapped_stmt && desc->stmt == stmt))) {
                        const std::lock_guard<std::recursive_mutex> desc_lock_guard(desc->lock);
                        desc->bound_col_buffers.clear();
                        desc->bound_param_buffers.clear();
                    }
                }

                return ret;
            }
        case SQL_DROP:
            // Deprecated option, used fully cleanup handle
        default:
            {
                // Remove statement from connection
                {
                    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
                    dbc->stmt_list.remove(stmt);
                }

                // Clean underlying Statements
                OdbcHelper::FreeWrappedStmt(env->driver_lib_loader, stmt);

                // The implicit descriptors, and with them their copies of the SQL_C_WCHAR bindings, after the base
                // statement whose descriptors' records point at them. An explicit descriptor keeps its own.
                delete stmt->app_row_desc;
                stmt->app_row_desc = SQL_NULL_HANDLE;
                delete stmt->app_param_desc;
                stmt->app_param_desc = SQL_NULL_HANDLE;
                delete stmt->imp_row_desc;
                stmt->imp_row_desc = SQL_NULL_HANDLE;
                delete stmt->imp_param_desc;
                stmt->imp_param_desc = SQL_NULL_HANDLE;

                ClearError(stmt);
                delete stmt;
                StatementHandle = nullptr;
                return SQL_SUCCESS;
            }
    }
}

SQLRETURN RDS_SQLCancel(
    SQLHSTMT       StatementHandle)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    // Wakes the running call's waits that use CancelState::WaitFor, and stops it before it reaches the base driver.
    // While the wrapper itself runs the call, that's the whole cancel: the base statement isn't executing, so the base
    // driver's SQLCancel could do nothing useful (and psqlODBC's lost-connection check would rewrite the statement's
    // diagnostics while the call reads them). The base driver gets the cancel during its execute, or with no call
    // running, as before. A call past a state change it can't undo isn't stopped. Nor are failover's reconnect and a
    // Blue/Green suspend, which don't check the cancel: the call returns their result (08S02/08S01 for failover).
    if (const CancelState::Target target = stmt->cancel.Request();
        target != CancelState::Target::NO_CALL && target != CancelState::Target::BASE_DRIVER) {
        return SQL_SUCCESS;
    }

    // Not the statement's lock: a call running on another thread holds it until it returns. And no lock across the
    // base call, which can block (psqlODBC's PQcancel connects to the server, for as long as the OS takes to give up
    // on a host that stopped answering): the connection's base handles stay pinned until it returns, and a teardown
    // that comes meanwhile (failover, a read/write splitting switch, a disconnect) runs after it instead of waiting
    // for it. psqlODBC's SQLCancel is built to be called from another thread.
    RdsLibResult res;
    {
        const BaseHandleGuard::PinnedStatement pinned(dbc->base_handles, stmt->wrapped_stmt);
        if (!pinned.Handle()) {
            // Failover or a read/write splitting switch detached it: there's nothing for the base driver to cancel.
            // With no call running the cancel has no effect, as ODBC 3 says; the handle itself is valid.
            return SQL_SUCCESS;
        }
        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLCancel>(env->driver_lib_loader, RdsFuncNames::Cancel,
            pinned.Handle()
        );
    }
    if (!res.fn_load_success) {
        // The diagnostics belong to a call running on another thread, if there is one
        const std::unique_lock<std::recursive_mutex> idle(stmt->lock, std::try_to_lock);
        if (idle.owns_lock()) {
            RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
        }
        return SQL_ERROR;
    }
    return res.fn_result;
}

SQLRETURN RDS_StatementCanceled(
    SQLHSTMT       StatementHandle)
{
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    LOG(INFO) << "Statement canceled before it ran";
    ClearError(stmt);
    stmt->err = std::make_unique<ErrInfo>("Operation canceled: SQLCancel was called before the statement ran", ERR_OPERATION_CANCELED);
    return SQL_ERROR;
}

SQLRETURN RDS_GetConnectAttr(
    SQLHDBC        ConnectionHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     BufferLength,
    SQLINTEGER *   StringLengthPtr)
{
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    ClearError(dbc);

    // Aurora DSQL has one isolation level, whatever psqlODBC would report.
    if (DsqlCompatibility::TryGetConnectAttr(dbc, Attribute, ValuePtr)) {
        return SQL_SUCCESS;
    }

    // When MAX_CONNECTION_LIFETIME is set, an Aurora DSQL connection at that age reports itself dead, so a driver
    // manager's pool replaces it. No round trip: the answer comes from the connection's age.
    if (Attribute == SQL_ATTR_CONNECTION_DEAD && DsqlConnectionLifetime::IsRetired(dbc)) {
        if (ValuePtr != nullptr) {
            *static_cast<SQLUINTEGER*>(ValuePtr) = static_cast<SQLUINTEGER>(SQL_CD_TRUE);
        }
        if (StringLengthPtr != nullptr) {
            *StringLengthPtr = static_cast<SQLINTEGER>(sizeof(SQLUINTEGER));
        }
        return SQL_SUCCESS;
    }

    SQLRETURN ret = SQL_ERROR;

    // If already connected, query value from underlying DBC
    if (dbc->wrapped_dbc) {
#if UNICODE
        const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
        if (odbc_helper->NeedsConversion() && BufferLength >= 0 && OdbcHelper::IsStringConnectAttr(Attribute)) {
            // BufferLength and the length count bytes of the app's characters; the base driver's count its own.
            const DriverStringResult value = odbc_helper->FetchDriverString(
                [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                    SQLINTEGER value_length = 0;
                    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetConnectAttr>(env->driver_lib_loader, RdsFuncNames::GetConnectAttr,
                        dbc->wrapped_dbc, Attribute, buffer, static_cast<SQLINTEGER>(length), &value_length
                    );
                    *full_length = value_length > 0 ? static_cast<size_t>(value_length) : 0;
                    return res;
                },
                ValuePtr, static_cast<size_t>(BufferLength), true, std::numeric_limits<SQLINTEGER>::max());
            ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, value.res);
            if (SQL_SUCCEEDED(ret)) {
                if (StringLengthPtr) {
                    *StringLengthPtr = static_cast<SQLINTEGER>(value.length);
                }
                ret = ReportConversionCut(dbc, ret, value.truncated, "SQLGetConnectAttr - The value",
                    value.length + odbc_helper->GetTargetByteCount(false, 1), "bytes");
            }
        } else
#endif
        {
            const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetConnectAttr>(env->driver_lib_loader, RdsFuncNames::GetConnectAttr,
                dbc->wrapped_dbc, Attribute, ValuePtr, BufferLength, StringLengthPtr
            );
            ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
        }
    }
    // Otherwise get from the DBC's attribute map
    else if (dbc->attr_map.contains(Attribute)) {
        ret = SQL_SUCCESS;
        const std::pair<SQLPOINTER, SQLINTEGER> value_pair = dbc->attr_map.at(Attribute);
        // A string's stored length (bytes) can be the size of an integer: tell strings apart by the attribute. An
        // integer is written as the type its length gave: 0, its size or an SQL_IS_* value.
        if (!OdbcHelper::IsStringConnectAttr(Attribute)) {
            if (ValuePtr == nullptr) {
                // Nowhere to write it
            } else if (value_pair.second == SQL_IS_POINTER) {
                *(static_cast<SQLPOINTER*>(ValuePtr)) = value_pair.first;
            } else if (value_pair.second == sizeof(SQLSMALLINT) || value_pair.second == SQL_IS_SMALLINT
                || value_pair.second == SQL_IS_USMALLINT) {
                *(static_cast<SQLUSMALLINT*>(ValuePtr)) = static_cast<SQLSMALLINT>(reinterpret_cast<intptr_t>(value_pair.first));
            } else {
                // 0, sizeof(SQLUINTEGER), SQL_IS_UINTEGER or SQL_IS_INTEGER
                *(static_cast<SQLUINTEGER*>(ValuePtr)) = static_cast<SQLUINTEGER>(reinterpret_cast<uintptr_t>(value_pair.first));
            }
        } else {
            // What SQLSetConnectAttr stored: its copy of the app's string and its length in bytes, read in the app's
            // characters as known now. BufferLength and *StringLengthPtr count bytes too, and can't be negative (HY090).
            if (BufferLength < 0) {
                dbc->err = std::make_unique<ErrInfo>("SQLGetConnectAttr - BufferLength is less than 0", ERR_INVALID_STRING_OR_BUFFER_LENGTH);
                return SQL_ERROR;
            }
#ifdef UNICODE
            // The app's characters: 4 bytes each for a 4-byte SQLWCHAR app.
            const bool user_4_byte = env->use_4_bytes_user_app.load();
            const auto char_bytes = static_cast<SQLINTEGER>((user_4_byte ? 2 : 1) * sizeof(SQLTCHAR));
#else
            const auto char_bytes = static_cast<SQLINTEGER>(sizeof(SQLTCHAR));
#endif
            // The room in whole characters of the app, the terminator's included.
            const size_t buffer_chars = ValuePtr ? static_cast<size_t>(BufferLength / char_bytes) : 0;
#ifdef UNICODE
            const SQLINTEGER value_chars = value_pair.second < 0 ? SQL_NTS : value_pair.second / char_bytes;
            const std::string value = ConvertUserAppToUTF8(user_4_byte, static_cast<SQLTCHAR*>(value_pair.first), value_chars);
            const size_t full_chars = CopyUTF8ToAppChars(value, ValuePtr, buffer_chars, user_4_byte);
#else
            const auto* const value = static_cast<const char*>(value_pair.first);
            // An explicit length needn't be followed by a NUL, so never read past it.
            size_t full_chars = 0;
            if (value) {
                full_chars = value_pair.second < 0 ? strlen(value) : strnlen(value, static_cast<size_t>(value_pair.second));
            }
            if (buffer_chars > 0) {
                const size_t copied = std::min(full_chars, buffer_chars - 1);
                std::copy_n(value, copied, static_cast<char*>(ValuePtr));
                static_cast<char*>(ValuePtr)[copied] = '\0';
            }
#endif
            if (StringLengthPtr) {
                *StringLengthPtr = static_cast<SQLINTEGER>(full_chars * static_cast<size_t>(char_bytes));
            }
            // Cut unless the value and its terminator fit those characters: 01004.
            if (ValuePtr && full_chars >= buffer_chars) {
                const std::string message = "SQLGetConnectAttr - The value was truncated: the buffer needs "
                    + std::to_string((full_chars + 1) * static_cast<size_t>(char_bytes)) + " bytes";
                dbc->err = std::make_unique<ErrInfo>(message.c_str(), WARN_STRING_DATA_RIGHT_TRUNCATED);
                ret = SQL_SUCCESS_WITH_INFO;
            }
        }
    }
    return ret;
}

namespace {
// The largest SQLWCHAR, 4 bytes: a stored string value is padded to whole characters of it and then a terminator of it.
constexpr size_t STORED_CHAR_BYTES = 4;

// A copy of the string value an app passed, zero-padded to a multiple of STORED_CHAR_BYTES and followed by
// STORED_CHAR_BYTES zero bytes, so that a NUL character ends it inside the copy whether it's read as 2- or 4-byte
// characters. *stored_length gets SQL_NTS, or the app's length in bytes.
// With a length, the copy is the app's bytes as they are. They are read in the app's characters when the value is
// used, and for a direct caller with a 4-byte SQLWCHAR that size is known only from its first connect.
// For SQL_NTS (any negative length) the end is the first NUL character, so the copy has to take char_bytes, the size
// known now: reading a 2-byte string as 4-byte characters until a 4-byte NUL goes past its terminator, out of the
// app's buffer. A 4-byte string copied as 2-byte characters keeps its first character.
std::vector<SQLTCHAR> CopyAttributeString(const SQLTCHAR* in, SQLINTEGER length, size_t char_bytes,
                                          SQLINTEGER* stored_length) {
    size_t bytes = 0;
    if (length >= 0) {
        bytes = static_cast<size_t>(length);
    } else {
        const size_t step = char_bytes / sizeof(SQLTCHAR);
        size_t chars = 0;
        while (!std::all_of(in + (chars * step), in + ((chars + 1) * step), [](SQLTCHAR part) { return part == 0; })) {
            chars++;
        }
        bytes = chars * char_bytes;
    }
    const size_t padded = (((bytes + STORED_CHAR_BYTES - 1) / STORED_CHAR_BYTES) + 1) * STORED_CHAR_BYTES;
    std::vector<SQLTCHAR> copy(padded / sizeof(SQLTCHAR), 0);
    std::memcpy(copy.data(), in, bytes);
    *stored_length = length >= 0 ? static_cast<SQLINTEGER>(bytes) : SQL_NTS;
    return copy;
}
}  // namespace

SQLRETURN RDS_SQLSetConnectAttr(
    SQLHDBC        ConnectionHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     StringLength)
{
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    const ENV* env = dbc->env;

    SQLRETURN ret = SQL_SUCCESS;

    // iODBC's SQL_ATTR_APP_WCHAR_TYPE (iodbcext.h; the wrapper builds with unixODBC's headers). iODBC 3.52.14 and later
    // set it on the driver's connection in each SQLConnect and SQLDriverConnect, before calling the driver's, to the
    // app's SQLWCHAR type, on an ANSI driver too. It's iODBC's attribute, not the base driver's, so it is never kept
    // for the base driver: psqlODBC refuses it, and every connect would end with that warning.
    constexpr SQLINTEGER IODBC_ATTR_APP_WCHAR_TYPE = 1061;
    if (Attribute == IODBC_ATTR_APP_WCHAR_TYPE) {
        const std::lock_guard<std::recursive_mutex> wchar_type_lock_guard(dbc->lock);
        ClearError(dbc);
#if UNICODE
        // The Unicode driver takes the type, and iODBC passes the app's text as it is: the connect functions then
        // don't have to tell the width from the app's text. The app's UTF-8 isn't taken: iODBC converts its text then.
        constexpr SQLULEN IODBC_CP_UTF16 = 1;
        constexpr SQLULEN IODBC_CP_UCS4 = 3;
        const auto type = reinterpret_cast<SQLULEN>(ValuePtr);
        if (type != IODBC_CP_UTF16 && type != IODBC_CP_UCS4) {
            dbc->err = std::make_unique<ErrInfo>("SQLSetConnectAttr - Unsupported SQL_ATTR_APP_WCHAR_TYPE", ERR_INVALID_ATTRIBUTE_VALUE);
            return SQL_ERROR;
        }
        dbc->env->use_4_bytes_user_app.store(type == IODBC_CP_UCS4);
        dbc->app_wchar_type_known = true;
#endif
        // The ANSI driver gets no wide text: iODBC converts a wide app's text to ANSI from the app's type whatever the
        // driver answers (conv_text_m2d, CD_W2A), so it takes any value.
        return SQL_SUCCESS;
    }

    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    ClearError(dbc);

    // Attributes that would make psqlODBC undo a forced option or send a statement Aurora DSQL rejects are answered here.
    if (const std::optional<SQLRETURN> dsql_ret = DsqlCompatibility::TrySetConnectAttr(dbc, Attribute, ValuePtr)) {
        return *dsql_ret;
    }

    // If already connected, apply value to underlying DBC, otherwise track and apply on connect
    if (dbc->wrapped_dbc) {
#if UNICODE
        const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
        if (odbc_helper->NeedsConversion() && ValuePtr
            && OdbcHelper::IsStringConnectAttr(Attribute)
            && (StringLength == SQL_NTS || StringLength > 0)) {
            // StringLength counts bytes; ConvertInput takes characters of the app's SQLWCHAR.
            const SQLINTEGER value_chars = StringLength > 0
                ? StringLength / static_cast<SQLINTEGER>(odbc_helper->GetUse4BytesUserApp() ? sizeof(SQLTCHAR) * 2 : sizeof(SQLTCHAR))
                : StringLength;
            const auto value_converted = odbc_helper->ConvertInput(static_cast<SQLTCHAR*>(ValuePtr), value_chars);
            const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetConnectAttr>(env->driver_lib_loader, RdsFuncNames::SetConnectAttr,
                dbc->wrapped_dbc, Attribute, value_converted.tchar_ptr, value_converted.BaseLength(StringLength)
            );
            ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
        } else
#endif
        {
            const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetConnectAttr>(env->driver_lib_loader, RdsFuncNames::SetConnectAttr,
                dbc->wrapped_dbc, Attribute, ValuePtr, StringLength
            );
            ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
        }
    }
    // A string value is kept as a copy: the app's buffer needn't outlive this call, and the value is written back by
    // SQLGetConnectAttr before a connect and applied to the base driver at each connect, in the app's characters then.
    SQLPOINTER stored_value = ValuePtr;
    SQLINTEGER stored_length = StringLength;
    if (ValuePtr && OdbcHelper::IsStringConnectAttr(Attribute)) {
#ifdef UNICODE
        const size_t char_bytes = (env->use_4_bytes_user_app.load() ? 2 : 1) * sizeof(SQLTCHAR);
#else
        const size_t char_bytes = sizeof(SQLTCHAR);
#endif
        std::vector<SQLTCHAR>& copy = dbc->attr_strings[Attribute];
        copy = CopyAttributeString(static_cast<const SQLTCHAR*>(ValuePtr), StringLength, char_bytes, &stored_length);
        stored_value = copy.data();
    } else {
        dbc->attr_strings.erase(Attribute);
    }
    dbc->attr_map.insert_or_assign(Attribute, std::make_pair(stored_value, stored_length));

    if (SQL_ATTR_AUTOCOMMIT == Attribute) {
        const bool was_auto_commit = dbc->auto_commit;
        dbc->auto_commit = reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON) == ValuePtr;
        // Autocommit set on while already on changes nothing in psqlODBC, so a transaction opened with BEGIN stays open.
        if (!was_auto_commit || !dbc->auto_commit) {
            dbc->transaction_status = dbc->auto_commit ? TRANSACTION_CLOSED : TRANSACTION_OPEN;
        }
    }

    return ret;
}

// Support for Ansi & Unicode specifics
SQLRETURN RDS_SQLBrowseConnect(
    SQLHDBC        ConnectionHandle,
    SQLTCHAR *     InConnectionString,
    SQLSMALLINT    StringLength1,
    SQLTCHAR *     OutConnectionString,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLength2Ptr)
{
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);

    LOG(ERROR) << "Unsupported SQL API - SQLBrowseConnect";
    ClearError(dbc);
    dbc->err = std::make_unique<ErrInfo>("SQLBrowseConnect - API Unsupported", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
    RDS_NOT_IMPLEMENTED;
}

SQLRETURN RDS_SQLColAttribute(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   ColumnNumber,
    SQLUSMALLINT   FieldIdentifier,
    SQLPOINTER     CharacterAttributePtr,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr,
    SQLLEN *       NumericAttributePtr)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
#if UNICODE
    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();

    // Only a character field is in the base driver's characters; a numeric one goes to NumericAttributePtr.
    if (odbc_helper->NeedsConversion() && BufferLength >= 0 && OdbcHelper::IsStringColAttribute(FieldIdentifier)) {
        // BufferLength and the length count bytes of the app's characters; the base driver's count its own.
        const DriverStringResult attr = odbc_helper->FetchDriverString(
            [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                SQLSMALLINT attr_length = 0;
                const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLColAttribute>(env->driver_lib_loader, RdsFuncNames::ColAttribute,
                    stmt->wrapped_stmt, ColumnNumber, FieldIdentifier, buffer, static_cast<SQLSMALLINT>(length), &attr_length, NumericAttributePtr
                );
                *full_length = attr_length > 0 ? static_cast<size_t>(attr_length) : 0;
                return res;
            },
            CharacterAttributePtr, static_cast<size_t>(BufferLength), true, std::numeric_limits<SQLSMALLINT>::max());
        SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, attr.res);
        if (SQL_SUCCEEDED(ret)) {
            if (StringLengthPtr) {
                *StringLengthPtr = static_cast<SQLSMALLINT>(attr.length);
            }
            ret = ReportConversionCut(stmt, ret, attr.truncated, "SQLColAttribute - The value",
                attr.length + odbc_helper->GetTargetByteCount(false, 1), "bytes");
        }
        return ret;
    }
#endif
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLColAttribute>(env->driver_lib_loader, RdsFuncNames::ColAttribute,
        stmt->wrapped_stmt, ColumnNumber, FieldIdentifier, CharacterAttributePtr, BufferLength, StringLengthPtr, NumericAttributePtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLColumnPrivileges(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     SchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     TableName,
    SQLSMALLINT    NameLength3,
    SQLTCHAR *     ColumnName,
    SQLSMALLINT    NameLength4)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto catalog_converted = odbc_helper->ConvertInput(CatalogName, NameLength1);
    const auto schema_converted  = odbc_helper->ConvertInput(SchemaName,  NameLength2);
    const auto table_converted   = odbc_helper->ConvertInput(TableName,   NameLength3);
    const auto column_converted  = odbc_helper->ConvertInput(ColumnName,  NameLength4);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLColumnPrivileges>(env->driver_lib_loader, RdsFuncNames::ColumnPrivileges,
        stmt->wrapped_stmt,
            catalog_converted.tchar_ptr,
            catalog_converted.BaseLength(NameLength1),
            schema_converted.tchar_ptr,
            schema_converted.BaseLength(NameLength2),
            table_converted.tchar_ptr,
            table_converted.BaseLength(NameLength3),
            column_converted.tchar_ptr,
            column_converted.BaseLength(NameLength4)
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLColumns(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     SchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     TableName,
    SQLSMALLINT    NameLength3,
    SQLTCHAR *     ColumnName,
    SQLSMALLINT    NameLength4)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto catalog_converted = odbc_helper->ConvertInput(CatalogName, NameLength1);
    const auto schema_converted  = odbc_helper->ConvertInput(SchemaName,  NameLength2);
    const auto table_converted   = odbc_helper->ConvertInput(TableName,   NameLength3);
    const auto column_converted  = odbc_helper->ConvertInput(ColumnName,  NameLength4);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLColumns>(
        env->driver_lib_loader,
        RdsFuncNames::Columns,
        stmt->wrapped_stmt,
            catalog_converted.tchar_ptr,
            catalog_converted.BaseLength(NameLength1),
            schema_converted.tchar_ptr,
            schema_converted.BaseLength(NameLength2),
            table_converted.tchar_ptr,
            table_converted.BaseLength(NameLength3),
            column_converted.tchar_ptr,
            column_converted.BaseLength(NameLength4)
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

namespace {
#if UNICODE && !defined(_WIN32)
    constexpr uint32_t MAX_CODE_POINT = 0x10FFFF;
    constexpr uint32_t FIRST_SURROGATE = 0xD800;
    constexpr uint32_t LAST_SURROGATE = 0xDFFF;

    // Whether a library path is unixODBC's driver manager (libodbc.so.2, libodbc.2.dylib) or its cursor library
    // (libodbccr), not its installer library (libodbcinst), which the wrapper and psqlODBC link.
    bool IsUnixOdbcLibrary(const char* path) {
        if (path == nullptr) {
            return false;
        }
        std::string_view leaf(path);
        if (const size_t slash = leaf.rfind('/'); slash != std::string_view::npos) {
            leaf.remove_prefix(slash + 1);
        }
        constexpr std::array<std::string_view, 2> NAMES = {"libodbc", "libodbccr"};
        return std::ranges::any_of(NAMES, [leaf](std::string_view name) {
            if (!leaf.starts_with(name)) {
                return false;
            }
            const std::string_view rest = leaf.substr(name.size());
    #ifdef __APPLE__
            return rest.starts_with('.') && rest.ends_with(".dylib");
    #else
            return rest.starts_with(".so");
    #endif
        });
    }

    // Whether the connect functions should check for an app with a 4-byte SQLWCHAR: the driver manager didn't say how
    // wide the app's SQLWCHAR is (iODBC 3.52.14 and later do, with SQL_ATTR_APP_WCHAR_TYPE), and the connect didn't
    // come through unixODBC. unixODBC's SQLWCHAR is 2 bytes, and its SQLConnectW and SQLDriverConnectW read the DSN
    // and the connection string as 2-byte text before calling the driver, so a 4-byte app's text never gets here;
    // reading a 2-byte app's text of one character as 4-byte text reads past it. Under an older iODBC, or with no driver
    // manager, a wide app's SQLWCHAR can be wchar_t, 4 bytes.
    bool MayBeFourByteApp(const DBC* dbc) {
        return !dbc->app_wchar_type_known && !dbc->connect_through_unix_odbc;
    }

    // The connect functions' check for an app with a 4-byte SQLWCHAR that the environment doesn't know of: reads a
    // text that came in as a 2-byte app's as 4-byte text. It reads no further than a 2-byte text holds (in_length
    // units, or up to its NUL for SQL_NTS) until it has read at least one code point there and found each one valid;
    // only then does it read on, as 4-byte text of in_length code points. "" when the text can't be 4-byte text. A
    // 2-byte text's second unit makes its first code point invalid unless it is a NUL or below U+0011, so a 2-byte
    // text of two or more printable characters is never read past. A text of one character can't be told from the
    // start of a 4-byte one, so it is read on: only without unixODBC (MayBeFourByteApp).
    std::string ReadAsFourByteText(SQLTCHAR* in, SQLINTEGER in_length) {
        if (in == nullptr || in_length == 0) {
            return "";
        }
        const uint16_t* units = reinterpret_cast<const uint16_t*>(in);
        size_t two_byte_units = 0;
        if (in_length > 0) {
            two_byte_units = static_cast<size_t>(in_length);
        } else {
            while (units[two_byte_units] != 0) {
                ++two_byte_units;
            }
            ++two_byte_units;  // and its NUL
        }
        if (two_byte_units < 2) {
            return "";
        }
        for (size_t i = 0; i < two_byte_units / 2; ++i) {
            const uint32_t cp = static_cast<uint32_t>(units[i * 2])
                | (static_cast<uint32_t>(units[(i * 2) + 1]) << SQLTCHAR_HALF_BITS);
            if (cp == 0) {
                break;
            }
            if (cp > MAX_CODE_POINT || (cp >= FIRST_SURROGATE && cp <= LAST_SURROGATE)) {
                return "";
            }
        }
        return ConvertUserAppToUTF8(true, in, in_length);
    }
#endif
#ifndef UNICODE
    // An ANSI app's text: in_length bytes, up to a NUL, or to the NUL for SQL_NTS (or any length below 0). The text
    // needn't end in a NUL, so it is never read past in_length.
    std::string AnsiAppText(const SQLTCHAR* in, SQLINTEGER in_length) {
        if (in == nullptr) {
            return "";
        }
        const char* chars = reinterpret_cast<const char*>(in);
        return in_length >= 0 ? std::string(chars, strnlen(chars, static_cast<size_t>(in_length))) : std::string(chars);
    }
#endif
}  // namespace

#if UNICODE && !defined(_WIN32)
bool RDS_IsInUnixOdbc(const void* address) {
    Dl_info info{};
    return address != nullptr && dladdr(address, &info) != 0 && IsUnixOdbcLibrary(info.dli_fname);
}

void RDS_NoteConnectCaller(SQLHDBC ConnectionHandle, const void* caller) {
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return;
    }
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    const bool through_unix_odbc = RDS_IsInUnixOdbc(caller);
    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    dbc->connect_through_unix_odbc = through_unix_odbc;
}
#endif

SQLRETURN RDS_SQLConnect(
    SQLHDBC        ConnectionHandle,
    SQLTCHAR *     ServerName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     UserName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     Authentication,
    SQLSMALLINT    NameLength3)
{
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);

    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    ClearError(dbc);

    SQLRETURN ret = SQL_ERROR;

    // Connection is already established
    if (CONN_NOT_CONNECTED != dbc->conn_status) {
        dbc->err = std::make_unique<ErrInfo>("SQLConnect - The connection handle is already connected", ERR_CONNECTION_NAME_IN_USE);
        return SQL_ERROR;
    }

    // A reused handle starts from this call's arguments: keys an earlier connect parsed, or plugins added (a token,
    // a failover host, a default user), don't carry over. SQLSetConnectAttr values are kept in attr_map.
    dbc->conn_attr.clear();

#ifdef UNICODE
    bool use_4_bytes_user_app = dbc->env->use_4_bytes_user_app.load();
#endif
    std::string conn_str_utf8;
    std::string current_utf8;
    // Each name is read up to its NameLength characters (bytes in ANSI) and no further, so it is used whole.
    if (ServerName) {
        // Load input DSN followed by Base DSN retrieved from input DSN
#ifdef UNICODE
        current_utf8 = ConvertUserAppToUTF8(use_4_bytes_user_app, ServerName, NameLength1);
#else
        current_utf8 = AnsiAppText(ServerName, NameLength1);
#endif
        OdbcDsnHelper::LoadAll(current_utf8, dbc->conn_attr);
#if UNICODE && !defined(_WIN32)
        // Only when the driver manager didn't say how wide the app's SQLWCHAR is, and the connect didn't come through
        // unixODBC. Windows' is always 2 bytes.
        if (!use_4_bytes_user_app && !dbc->conn_attr.contains(KEY_DRIVER) && !dbc->conn_attr.contains(KEY_DSN)
            && MayBeFourByteApp(dbc)) {
            // An app with a 4-byte SQLWCHAR? The check reads no further than a 2-byte name holds until the text can't
            // be one, and the 2-byte name stays when it finds no DSN either.
            const std::string dsn_4_byte = ReadAsFourByteText(ServerName, NameLength1);
            if (!dsn_4_byte.empty()) {
                OdbcDsnHelper::LoadAll(dsn_4_byte, dbc->conn_attr);
            }
            if (dbc->conn_attr.contains(KEY_DRIVER) || dbc->conn_attr.contains(KEY_DSN)) {
                current_utf8 = dsn_4_byte;
                use_4_bytes_user_app = true;
                dbc->env->use_4_bytes_user_app.store(true);
            }
        }
#endif
        dbc->conn_attr.insert_or_assign(KEY_DSN, current_utf8);
        conn_str_utf8 += "DSN=" + current_utf8 + ";";
    } else {
        // Error, no DSN
        // TODO - Load default DSN?
        dbc->err = std::make_unique<ErrInfo>("SQLConnect - DSN not provided", ERR_CLIENT_UNABLE_TO_ESTABLISH_CONNECTION);
        return SQL_ERROR;
    }

    // Replace with input parameters
    if (UserName) {
#ifdef UNICODE
        current_utf8 = ConvertUserAppToUTF8(use_4_bytes_user_app, UserName, NameLength2);
#else
        current_utf8 = AnsiAppText(UserName, NameLength2);
#endif
        dbc->conn_attr.insert_or_assign(KEY_DB_USERNAME, current_utf8);
        conn_str_utf8 += "UID=" + current_utf8 + ";";
    }
    if (Authentication) {
#ifdef UNICODE
        current_utf8 = ConvertUserAppToUTF8(use_4_bytes_user_app, Authentication, NameLength3);
#else
        current_utf8 = AnsiAppText(Authentication, NameLength3);
#endif
        dbc->conn_attr.insert_or_assign(KEY_DB_PASSWORD, current_utf8);
        conn_str_utf8 += "PWD=" + current_utf8 + ";";
    }

    dbc->allow_interactive_auth = MapUtils::GetBooleanValue(dbc->conn_attr, KEY_SSO_ALLOW_INTERACTIVE, false);

    // Connect if initialization successful
    if (SQL_SUCCEEDED(RDS_InitializeConnection(dbc, conn_str_utf8))) {
        ret = dbc->plugin_head->Connect(ConnectionHandle, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    }

    return ret;
}

// TODO Maybe - Impl SQLDataSources
// Both PostgreSQL AND MySQL do NOT implement this
SQLRETURN RDS_SQLDataSources(
    SQLHENV        EnvironmentHandle,
    SQLUSMALLINT   Direction,
    SQLTCHAR *     ServerName,
    SQLSMALLINT    BufferLength1,
    SQLSMALLINT *  NameLength1Ptr,
    SQLTCHAR *     Description,
    SQLSMALLINT    BufferLength2,
    SQLSMALLINT *  NameLength2Ptr)
{
    if (!HasEnvAccess<ENV>(EnvironmentHandle)) {
        return SQL_INVALID_HANDLE;
    }
    ENV *env = static_cast<ENV*>(EnvironmentHandle);

    LOG(ERROR) << "Unsupported SQL API - SQLDataSources";
    ClearError(env);
    env->err = std::make_unique<ErrInfo>("SQLDataSources - API Unsupported", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
    RDS_NOT_IMPLEMENTED;
}

SQLRETURN RDS_SQLDescribeCol(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   ColumnNumber,
    SQLTCHAR *     ColumnName,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  NameLengthPtr,
    SQLSMALLINT *  DataTypePtr,
    SQLULEN *      ColumnSizePtr,
    SQLSMALLINT *  DecimalDigitsPtr,
    SQLSMALLINT *  NullablePtr)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    // BufferLength counts characters and can't be negative (HY090, as the driver manager checks).
    if (BufferLength < 0) {
        stmt->err = std::make_unique<ErrInfo>("SQLDescribeCol - BufferLength is less than 0", ERR_INVALID_STRING_OR_BUFFER_LENGTH);
        return SQL_ERROR;
    }
#if UNICODE
    {
        const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
        if (odbc_helper->NeedsConversion()) {
            // BufferLength and the length count the app's characters; the base driver's count its own.
            const DriverStringResult name = odbc_helper->FetchDriverString(
                [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                    SQLSMALLINT name_length = 0;
                    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLDescribeCol>(env->driver_lib_loader, RdsFuncNames::DescribeCol,
                        stmt->wrapped_stmt, ColumnNumber, buffer, static_cast<SQLSMALLINT>(length), &name_length, DataTypePtr, ColumnSizePtr, DecimalDigitsPtr, NullablePtr
                    );
                    *full_length = name_length > 0 ? static_cast<size_t>(name_length) : 0;
                    return res;
                },
                ColumnName, static_cast<size_t>(BufferLength), false, std::numeric_limits<SQLSMALLINT>::max());
            SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, name.res);
            if (SQL_SUCCEEDED(ret)) {
                if (NameLengthPtr) {
                    *NameLengthPtr = static_cast<SQLSMALLINT>(name.length);
                }
                ret = ReportConversionCut(stmt, ret, name.truncated, "SQLDescribeCol - The column name", name.length + 1, "characters");
            }
            return ret;
        }
    }
#endif
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLDescribeCol>(env->driver_lib_loader, RdsFuncNames::DescribeCol,
        stmt->wrapped_stmt, ColumnNumber, ColumnName, BufferLength, NameLengthPtr, DataTypePtr, ColumnSizePtr, DecimalDigitsPtr, NullablePtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLDriverConnect(
    SQLHDBC        ConnectionHandle,
    SQLHWND        WindowHandle,
    SQLTCHAR *     InConnectionString,
    SQLSMALLINT    StringLength1,
    SQLTCHAR *     OutConnectionString,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLength2Ptr,
    SQLUSMALLINT   DriverCompletion)
{
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);

    // BufferLength can't be negative (HY090). Checked before a dialog or a connect, so that a handle isn't left
    // connected under an error; unixODBC checks only StringLength1.
    if (BufferLength < 0) {
        const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
        ClearError(dbc);
        dbc->err = std::make_unique<ErrInfo>("SQLDriverConnect - BufferLength is less than 0", ERR_INVALID_STRING_OR_BUFFER_LENGTH);
        return SQL_ERROR;
    }

    std::string conn_str_utf8;
    std::string conn_out_str_utf8;
#ifdef UNICODE
    const bool use_4_bytes_user_app = dbc->env->use_4_bytes_user_app.load();
#endif

#if _WIN32
    const bool use_setup_dialog = (DriverCompletion != SQL_DRIVER_NOPROMPT) && WindowHandle;
#else
    const bool use_setup_dialog = false;
#endif

    if (use_setup_dialog) {
#if _WIN32
        bool complete_required = true;
        std::tuple<std::string, std::string, bool> dialog_result; // conn_str, out_conn_str, dialog_box_cancelled

        switch (DriverCompletion) {
            case SQL_DRIVER_PROMPT:
            case SQL_DRIVER_COMPLETE:
                complete_required = false;
            case SQL_DRIVER_COMPLETE_REQUIRED:
                dialog_result = StartDialogForSqlDriverConnect(WindowHandle, InConnectionString, OutConnectionString, complete_required);
                break;
            case SQL_DRIVER_NOPROMPT:
            default:
                break;
        }

        if (std::get<2>(dialog_result)) {
            // If a dialog box was opened but cancelled mid-operation, do not continue with the connection attempt.
            return SQL_NO_DATA_FOUND;
        }

        conn_str_utf8 = std::get<0>(dialog_result);
        conn_out_str_utf8 = std::get<1>(dialog_result);
#endif
    } else {
#ifdef UNICODE
        conn_str_utf8 = ConvertUserAppToUTF8(use_4_bytes_user_app, InConnectionString, StringLength1);
#else
        conn_str_utf8 = AnsiAppText(InConnectionString, StringLength1);
#endif
    }

    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    ClearError(dbc);

    SQLRETURN ret = SQL_ERROR;

    // Connection is already established
    if (CONN_NOT_CONNECTED != dbc->conn_status) {
        LOG(ERROR) << "Connection handle is already has an existing connection";
        dbc->err = std::make_unique<ErrInfo>("SQLDriverConnect - The connection handle is already connected", ERR_CONNECTION_NAME_IN_USE);
        return SQL_ERROR;
    }

    // A reused handle starts from this call's arguments: keys an earlier connect parsed, or plugins added (a token,
    // a failover host, a default user), don't carry over. SQLSetConnectAttr values are kept in attr_map.
    dbc->conn_attr.clear();

    // Parse connection string, load input DSN followed by Base DSN
    ConnectionStringHelper::ParseConnectionString(conn_str_utf8, dbc->conn_attr);

#if UNICODE && !defined(_WIN32)
    // Only when the driver manager didn't say how wide the app's SQLWCHAR is, and the connect didn't come through
    // unixODBC.
    if (!dbc->conn_attr.contains(KEY_DRIVER) && !dbc->conn_attr.contains(KEY_DSN) && MayBeFourByteApp(dbc)) {
        // An app with a 4-byte SQLWCHAR? The check reads no further than a 2-byte string holds until the text can't
        // be one, and the 2-byte string's keys stay when it finds no DSN or DRIVER either.
        const std::string conn_str_utf8_w = ReadAsFourByteText(InConnectionString, StringLength1);
        std::map<std::string, std::string> conn_attr_w;
        ConnectionStringHelper::ParseConnectionString(conn_str_utf8_w, conn_attr_w);

        if (conn_attr_w.contains(KEY_DRIVER) || conn_attr_w.contains(KEY_DSN)) {
            dbc->conn_attr = std::move(conn_attr_w);
            conn_str_utf8 = conn_str_utf8_w;
            dbc->env->use_4_bytes_user_app.store(true);
        }
    }
#endif

    // Load DSN information into map
    if (dbc->conn_attr.contains(KEY_DSN)) {
        OdbcDsnHelper::LoadAll(dbc->conn_attr.at(KEY_DSN), dbc->conn_attr);
    }

    // Construct Out Connection String if not built by dialog prompt
    // Build without loading base DSN information, connection string is to be used for wrapper
    if (conn_out_str_utf8.empty()) {
        conn_out_str_utf8 = ConnectionStringHelper::BuildFullConnectionString(dbc->conn_attr);
    }

    dbc->allow_interactive_auth = (DriverCompletion != SQL_DRIVER_NOPROMPT) || MapUtils::GetBooleanValue(dbc->conn_attr, KEY_SSO_ALLOW_INTERACTIVE, false);

    ret = RDS_InitializeConnection(dbc, conn_str_utf8);
    // Connect if initialization successful. The base driver's output string, with the password or token in PWD, goes
    // into DefaultPlugin's buffer, never the app's: the app gets only the wrapper's string below.
    if (SQL_SUCCEEDED(ret)) {
        // Pass SQL_DRIVER_NOPROMPT to base driver, otherwise base driver may show its own dialog box when it's not needed.
        ret = dbc->plugin_head->Connect(ConnectionHandle, WindowHandle, OutConnectionString, BufferLength, StringLength2Ptr, SQL_DRIVER_NOPROMPT);
    }

    if (SQL_SUCCEEDED(ret)) {
        dbc->plugin_service->SetCurrentHostInfo(dbc->plugin_service->GetHostListProvider()->GetConnectionInfo(dbc));
    }

    conn_out_str_utf8 = ConnectionStringHelper::RemoveInternalWrapperKeys(conn_out_str_utf8);

    // When DSN-only output is enabled, return just the DSN name to avoid exposing credentials in the output connection string.
    conn_out_str_utf8 = ConnectionStringHelper::BuildDsnOnlyConnectionString(dbc->conn_attr, conn_out_str_utf8);

    // Update OutConnectionString with parsed values: the wrapper's string and its terminator, and nothing past them.
    // BufferLength can claim more than the buffer holds (iODBC passes its 8 KB buffer's size in bytes as characters).
#ifdef UNICODE
    // An app with a 4-byte SQLWCHAR, which the parse above may have just detected, has 4 bytes per character.
    const bool user_4_byte = dbc->env->use_4_bytes_user_app.load();
    // The full length in the app's characters (UTF-16 units, or code points for a 4-byte SQLWCHAR), not in UTF-8 bytes.
    const SQLULEN out_length = CopyUTF8ToAppChars(conn_out_str_utf8, nullptr, 0, user_4_byte);
#else
    const SQLULEN out_length = conn_out_str_utf8.length();
#endif
    if (OutConnectionString && BufferLength > 0) {
        const auto buffer_chars = static_cast<size_t>(BufferLength);
#ifdef UNICODE
        // The longest prefix that leaves room for the terminator, not ending in the first half of a surrogate pair.
        CopyUTF8ToAppChars(conn_out_str_utf8, OutConnectionString, buffer_chars, user_4_byte);
#else
        const size_t copied = std::min<size_t>(out_length, buffer_chars - 1);
        std::copy_n(conn_out_str_utf8.begin(), copied, OutConnectionString);
        OutConnectionString[copied] = '\0';
#endif
    }
    if (StringLength2Ptr) {
        *StringLength2Ptr = static_cast<SQLSMALLINT>(out_length);
    }
    // The wrapper cut its own string: say so with 01004. A connect that failed or returned a warning keeps its
    // return code and its record (the wrapper keeps one record per handle).
    const SQLULEN out_capacity = BufferLength > 0 ? static_cast<SQLULEN>(BufferLength) : 0;
    if (OutConnectionString && out_length >= out_capacity && ret == SQL_SUCCESS) {
        const std::string message = "SQLDriverConnect - The output connection string was truncated: the buffer needs "
            + std::to_string(out_length + 1) + " characters";
        ClearError(dbc);
        dbc->err = std::make_unique<ErrInfo>(message.c_str(), WARN_STRING_DATA_RIGHT_TRUNCATED);
        ret = SQL_SUCCESS_WITH_INFO;
    }

    return ret;
}

// TODO Maybe - Impl SQLDrivers
// Not implemented in MySQL or PostgreSQL ODBC
SQLRETURN RDS_SQLDrivers(
    SQLHENV        EnvironmentHandle,
    SQLUSMALLINT   Direction,
    SQLTCHAR *     DriverDescription,
    SQLSMALLINT    BufferLength1,
    SQLSMALLINT *  DescriptionLengthPtr,
    SQLTCHAR *     DriverAttributes,
    SQLSMALLINT    BufferLength2,
    SQLSMALLINT *  AttributesLengthPtr)
{
    if (!HasEnvAccess<ENV>(EnvironmentHandle)) {
        return SQL_INVALID_HANDLE;
    }
    ENV* env = static_cast<ENV*>(EnvironmentHandle);

    LOG(ERROR) << "Unsupported SQL API - SQLDrivers";
    ClearError(env);
    env->err = std::make_unique<ErrInfo>("SQLDrivers - API Unsupported", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
    RDS_NOT_IMPLEMENTED;
}

SQLRETURN RDS_SQLError(
    SQLHENV        EnvironmentHandle,
    SQLHDBC        ConnectionHandle,
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     SQLState,
    SQLINTEGER *   NativeErrorPtr,
    SQLTCHAR *     MessageText,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  TextLengthPtr,
    bool           WrapperCall)
{
    SQLRETURN ret = SQL_INVALID_HANDLE;

    // Take in order of lowest to high
    //  Stmt > Dbc > Env
    // SQLError is deprecated, if called directly,
    //  return the first error only once
    //  UnixODBC will enter an infinite loop otherwise
    if (StatementHandle) {
        ret = RDS_SQLGetDiagRec(SQL_HANDLE_STMT, StatementHandle, NextErrorRecord<STMT>(StatementHandle),
            SQLState, NativeErrorPtr, MessageText, BufferLength, TextLengthPtr, WrapperCall
        );
    }
    else if (ConnectionHandle) {
        ret = RDS_SQLGetDiagRec(SQL_HANDLE_DBC, ConnectionHandle, NextErrorRecord<DBC>(ConnectionHandle),
            SQLState, NativeErrorPtr, MessageText, BufferLength, TextLengthPtr, WrapperCall
        );
    }
    else if (EnvironmentHandle) {
        ret = RDS_SQLGetDiagRec(SQL_HANDLE_ENV, EnvironmentHandle, NextErrorRecord<ENV>(EnvironmentHandle),
            SQLState, NativeErrorPtr, MessageText, BufferLength, TextLengthPtr, WrapperCall
        );
    }

    return ret;
}

SQLRETURN RDS_SQLExecDirect(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     StatementText,
    SQLINTEGER     TextLength)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    DBC* dbc = stmt->dbc;
    // Cancelable from here, also while the call waits for the connection another statement's call holds
    CancelState::Scope cancel_scope(stmt->cancel);

    // Lock DBC to prevent connection swap race condition
    const std::lock_guard<std::recursive_mutex> lock_guard_dbc(dbc->lock);
    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    // The statement's running call from here: a cancel now stops this call, not one waiting behind it. One that came
    // while it waited stops it before a plugin changes state for it.
    if (cancel_scope.Enter()) {
        return RDS_StatementCanceled(stmt);
    }

    // ODBC: HY009 for a null pointer; HY090 for a TextLength of 0, or below 0 but not SQL_NTS. The plugin chain takes
    // an empty text for SQLExecute, which would run whatever was prepared on the handle.
    if (StatementText == nullptr) {
        stmt->err = std::make_unique<ErrInfo>("SQLExecDirect - StatementText is a null pointer", ERR_INVALID_USE_OF_NULL_POINTER);
        return SQL_ERROR;
    }
    if (TextLength != SQL_NTS && TextLength <= 0) {
        stmt->err = std::make_unique<ErrInfo>("SQLExecDirect - Invalid string or buffer length", ERR_INVALID_STRING_OR_BUFFER_LENGTH);
        return SQL_ERROR;
    }

    // A direct execute replaces any prepared statement on the handle.
    stmt->prepared_text.clear();

#if UNICODE
    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const std::string utf8 = ConvertUserAppToUTF8(odbc_helper->GetUse4BytesUserApp(), StatementText, TextLength);
    // Plugin chain expects UTF16, with the text's length in UTF-16 units: a 4-byte app's TextLength counts code points
    std::vector<uint16_t> stmt_utf16 = ConvertUTF8ToUTF16(utf8);
    SQLTCHAR* stmt_text = reinterpret_cast<SQLTCHAR*>(stmt_utf16.data());
    const auto text_length = static_cast<SQLINTEGER>(stmt_utf16.size() - 1);
#else
    // The chain reads the text to its NUL, so it gets the TextLength bytes as a terminated copy.
    std::string stmt_ansi = AnsiAppText(StatementText, TextLength);
    SQLTCHAR* stmt_text = reinterpret_cast<SQLTCHAR*>(stmt_ansi.data());
    const auto text_length = static_cast<SQLINTEGER>(stmt_ansi.size());
#endif
    // Also empty: "" with SQL_NTS, or a text that starts with a NUL.
    if (text_length == 0) {
        stmt->err = std::make_unique<ErrInfo>("SQLExecDirect - Invalid string or buffer length", ERR_INVALID_STRING_OR_BUFFER_LENGTH);
        return SQL_ERROR;
    }

    if (dbc->plugin_head) {
#if UNICODE && !defined(_WIN32)
        // The bound character parameters, as for SQLExecute.
        RDS_ConvertBoundParamBuffersBeforeExecute(stmt);
#endif
        const SQLRETURN ret = dbc->plugin_head->Execute(StatementHandle, stmt_text, text_length);
        stmt->execute_needs_data = ret == SQL_NEED_DATA;
#if UNICODE && !defined(_WIN32)
        if (SQL_SUCCEEDED(ret)) {
            return RDS_ConvertBoundParamBuffersAfterExecute(stmt, ret, "SQLExecDirect");
        }
#endif
        return ret;
    }

    LOG(ERROR) << "Cannot execute without an open connection";
    stmt->err = std::make_unique<ErrInfo>("SQLExecDirect - Connection not open", ERR_CONNECTION_NOT_OPEN);
    return SQL_ERROR;
}

SQLRETURN RDS_SQLForeignKeys(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     PKCatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     PKSchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     PKTableName,
    SQLSMALLINT    NameLength3,
    SQLTCHAR *     FKCatalogName,
    SQLSMALLINT    NameLength4,
    SQLTCHAR *     FKSchemaName,
    SQLSMALLINT    NameLength5,
    SQLTCHAR *     FKTableName,
    SQLSMALLINT    NameLength6)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto pk_catalog_converted = odbc_helper->ConvertInput(PKCatalogName, NameLength1);
    const auto pk_schema_converted  = odbc_helper->ConvertInput(PKSchemaName,  NameLength2);
    const auto pk_table_converted   = odbc_helper->ConvertInput(PKTableName,   NameLength3);
    const auto fk_catalog_converted = odbc_helper->ConvertInput(FKCatalogName, NameLength4);
    const auto fk_schema_converted  = odbc_helper->ConvertInput(FKSchemaName,  NameLength5);
    const auto fk_table_converted   = odbc_helper->ConvertInput(FKTableName,   NameLength6);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLForeignKeys>(env->driver_lib_loader, RdsFuncNames::ForeignKeys,
        stmt->wrapped_stmt,
            pk_catalog_converted.tchar_ptr,
            pk_catalog_converted.BaseLength(NameLength1),
            pk_schema_converted.tchar_ptr,
            pk_schema_converted.BaseLength(NameLength2),
            pk_table_converted.tchar_ptr,
            pk_table_converted.BaseLength(NameLength3),
            fk_catalog_converted.tchar_ptr,
            fk_catalog_converted.BaseLength(NameLength4),
            fk_schema_converted.tchar_ptr,
            fk_schema_converted.BaseLength(NameLength5),
            fk_table_converted.tchar_ptr,
            fk_table_converted.BaseLength(NameLength6)
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLGetConnectAttr(
    SQLHDBC        ConnectionHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     BufferLength,
    SQLINTEGER *   StringLengthPtr)
{
    return RDS_GetConnectAttr(ConnectionHandle, Attribute, ValuePtr, BufferLength, StringLengthPtr);
}

SQLRETURN RDS_SQLGetConnectOption(
    SQLHDBC        ConnectionHandle,
    SQLUSMALLINT   Attribute,
    SQLPOINTER     ValuePtr)
{
    return RDS_GetConnectAttr(
        ConnectionHandle,
        Attribute,
        ValuePtr,
        ((Attribute == SQL_ATTR_CURRENT_CATALOG) ? SQL_MAX_OPTION_STRING_LENGTH : 0),
        nullptr
    );
}

SQLRETURN RDS_SQLGetCursorName(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CursorName,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  NameLengthPtr)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;
    SQLRETURN ret = SQL_SUCCESS;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    // BufferLength counts characters and can't be negative (HY090, as the driver manager checks). Nothing is written.
    if (BufferLength < 0) {
        stmt->err = std::make_unique<ErrInfo>("SQLGetCursorName - BufferLength is less than 0", ERR_INVALID_STRING_OR_BUFFER_LENGTH);
        return SQL_ERROR;
    }

    if (stmt->wrapped_stmt) {
#if UNICODE
        const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
        if (odbc_helper->NeedsConversion()) {
            // BufferLength and the length count the app's characters; the base driver's count its own.
            const DriverStringResult name = odbc_helper->FetchDriverString(
                [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                    SQLSMALLINT name_length = 0;
                    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetCursorName>(env->driver_lib_loader, RdsFuncNames::GetCursorName,
                        stmt->wrapped_stmt, buffer, static_cast<SQLSMALLINT>(length), &name_length
                    );
                    *full_length = name_length > 0 ? static_cast<size_t>(name_length) : 0;
                    return res;
                },
                CursorName, static_cast<size_t>(BufferLength), false, std::numeric_limits<SQLSMALLINT>::max());
            ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, name.res);
            if (SQL_SUCCEEDED(ret)) {
                if (NameLengthPtr) {
                    *NameLengthPtr = static_cast<SQLSMALLINT>(name.length);
                }
                ret = ReportConversionCut(stmt, ret, name.truncated, "SQLGetCursorName - The cursor name", name.length + 1, "characters");
            }
        } else {
#endif
            const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetCursorName>(env->driver_lib_loader, RdsFuncNames::GetCursorName,
                stmt->wrapped_stmt, CursorName, BufferLength, NameLengthPtr
            );
            ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
#if UNICODE
        }
#endif
    } else {
        if (stmt->cursor_name.empty()) {
            stmt->cursor_name = std::string("CUR_").append(std::to_string(stmt->dbc->unnamed_cursor_count++));
        }
        const std::string name = stmt->cursor_name;
#ifdef UNICODE
        // The length in the app's characters (UTF-16 units, or code points for a 4-byte SQLWCHAR); a name that
        // doesn't fit is cut to a terminated prefix of BufferLength characters.
        const size_t name_chars = CursorName ? static_cast<size_t>(BufferLength) : 0;
        const SQLULEN len = CopyUTF8ToAppChars(name, CursorName, name_chars, env->use_4_bytes_user_app.load());
#else
        const SQLULEN len = name.length();
        if (CursorName && BufferLength > 0) {
            snprintf(reinterpret_cast<char*>(CursorName), static_cast<size_t>(BufferLength) / sizeof(SQLTCHAR), "%s", name.c_str());
        }
#endif
        // The wrapper cut its own name: 01004.
        if (CursorName && len >= static_cast<SQLULEN>(BufferLength)) {
            const std::string message = "SQLGetCursorName - The cursor name was truncated: the buffer needs "
                + std::to_string(len + 1) + " characters";
            stmt->err = std::make_unique<ErrInfo>(message.c_str(), WARN_STRING_DATA_RIGHT_TRUNCATED);
            ret = SQL_SUCCESS_WITH_INFO;
        }
        if (NameLengthPtr) {
            // Characters, not bytes
            *NameLengthPtr = static_cast<SQLSMALLINT>(len);
        }
    }

    return ret;
}

SQLRETURN RDS_SQLGetDescField(
    SQLHDESC       DescriptorHandle,
    SQLSMALLINT    RecNumber,
    SQLSMALLINT    FieldIdentifier,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     BufferLength,
    SQLINTEGER *   StringLengthPtr)
{
    if (!HasEnvAccess<DESC>(DescriptorHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DESC* desc = static_cast<DESC*>(DescriptorHandle);
    const DBC* dbc = desc->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(desc->lock);
    ClearError(desc);

    if (!HasWrappedHandle(desc)) {
        return SQL_INVALID_HANDLE;
    }
#if UNICODE
    {
        const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
        if (odbc_helper->NeedsConversion() && BufferLength >= 0 && OdbcHelper::IsStringDescField(FieldIdentifier)) {
            // BufferLength and the length count bytes of the app's characters; the base driver's count its own.
            const DriverStringResult field = odbc_helper->FetchDriverString(
                [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                    SQLINTEGER field_length = 0;
                    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDescField>(env->driver_lib_loader, RdsFuncNames::GetDescField,
                        desc->wrapped_desc, RecNumber, FieldIdentifier, buffer, static_cast<SQLINTEGER>(length), &field_length
                    );
                    *full_length = field_length > 0 ? static_cast<size_t>(field_length) : 0;
                    return res;
                },
                ValuePtr, static_cast<size_t>(BufferLength), true, std::numeric_limits<SQLINTEGER>::max());
            SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, field.res);
            if (SQL_SUCCEEDED(ret)) {
                if (StringLengthPtr) {
                    *StringLengthPtr = static_cast<SQLINTEGER>(field.length);
                }
                ret = ReportConversionCut(desc, ret, field.truncated, "SQLGetDescField - The value",
                    field.length + odbc_helper->GetTargetByteCount(false, 1), "bytes");
            }
            return ret;
        }
    }
#endif
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDescField>(env->driver_lib_loader, RdsFuncNames::GetDescField,
        desc->wrapped_desc, RecNumber, FieldIdentifier, ValuePtr, BufferLength, StringLengthPtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
}

SQLRETURN RDS_SQLGetDescRec(
    SQLHDESC       DescriptorHandle,
    SQLSMALLINT    RecNumber,
    SQLTCHAR *     Name,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr,
    SQLSMALLINT *  TypePtr,
    SQLSMALLINT *  SubTypePtr,
    SQLLEN *       LengthPtr,
    SQLSMALLINT *  PrecisionPtr,
    SQLSMALLINT *  ScalePtr,
    SQLSMALLINT *  NullablePtr)
{
    if (!HasEnvAccess<DESC>(DescriptorHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DESC* desc = static_cast<DESC*>(DescriptorHandle);
    const DBC* dbc = desc->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(desc->lock);
    ClearError(desc);

    if (!HasWrappedHandle(desc)) {
        return SQL_INVALID_HANDLE;
    }
    // BufferLength counts characters and can't be negative (HY090).
    if (BufferLength < 0) {
        desc->err = std::make_unique<ErrInfo>("SQLGetDescRec - BufferLength is less than 0", ERR_INVALID_STRING_OR_BUFFER_LENGTH);
        return SQL_ERROR;
    }
#if UNICODE
    {
        const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
        if (odbc_helper->NeedsConversion()) {
            // BufferLength and the length count the app's characters; the base driver's count its own.
            const DriverStringResult name = odbc_helper->FetchDriverString(
                [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                    SQLSMALLINT name_length = 0;
                    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDescRec>(env->driver_lib_loader, RdsFuncNames::GetDescRec,
                        desc->wrapped_desc, RecNumber, buffer, static_cast<SQLSMALLINT>(length), &name_length, TypePtr, SubTypePtr, LengthPtr, PrecisionPtr, ScalePtr, NullablePtr
                    );
                    *full_length = name_length > 0 ? static_cast<size_t>(name_length) : 0;
                    return res;
                },
                Name, static_cast<size_t>(BufferLength), false, std::numeric_limits<SQLSMALLINT>::max());
            SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, name.res);
            if (SQL_SUCCEEDED(ret)) {
                if (StringLengthPtr) {
                    *StringLengthPtr = static_cast<SQLSMALLINT>(name.length);
                }
                ret = ReportConversionCut(desc, ret, name.truncated, "SQLGetDescRec - The name", name.length + 1, "characters");
            }
            return ret;
        }
    }
#endif
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDescRec>(env->driver_lib_loader, RdsFuncNames::GetDescRec,
        desc->wrapped_desc, RecNumber, Name, BufferLength, StringLengthPtr, TypePtr, SubTypePtr, LengthPtr, PrecisionPtr, ScalePtr, NullablePtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
}

SQLRETURN RDS_SQLGetDiagField(
    SQLSMALLINT    HandleType,
    SQLHANDLE      Handle,
    SQLSMALLINT    RecNumber,
    SQLSMALLINT    DiagIdentifier,
    SQLPOINTER     DiagInfoPtr,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr)
{
    DESC* desc = nullptr;
    STMT* stmt = nullptr;
    DBC* dbc = nullptr;
    ENV* env = nullptr;

    std::optional<ErrInfo> err;
    bool base_record = false;  // the base driver answered

    RdsLibResult res;
    SQLRETURN ret = SQL_SUCCESS;
    SQLULEN len = 0;

    switch (HandleType) {
        case SQL_HANDLE_ENV:
            if (!HasEnvAccess<ENV>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                env = static_cast<ENV*>(Handle);
                const std::lock_guard<std::recursive_mutex> lock_guard(env->lock);
                // The wrapper's own record first, as in RDS_SQLGetDiagRec: it replaces the base driver's.
                const bool wrapper_record = env->err && !env->err->sqlstate.empty();
                if (env->wrapped_env && !wrapper_record && !env->base_diag_stale) {
                    base_record = true;
#if UNICODE
                    const auto odbc_helper = RDS_GetOdbcHelper(nullptr, env);
                    if (odbc_helper && odbc_helper->NeedsConversion() && BufferLength >= 0
                        && OdbcHelper::IsStringDiagField(DiagIdentifier)) {
                        // BufferLength and the length count bytes of the app's characters; the base driver's count its own.
                        const DriverStringResult field = odbc_helper->FetchDriverString(
                            [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                                SQLSMALLINT field_length = 0;
                                const RdsLibResult result = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagField>(env->driver_lib_loader, RdsFuncNames::GetDiagField,
                                    HandleType, env->wrapped_env, RecNumber, DiagIdentifier, buffer, static_cast<SQLSMALLINT>(length), &field_length
                                );
                                *full_length = field_length > 0 ? static_cast<size_t>(field_length) : 0;
                                return result;
                            },
                            DiagInfoPtr, static_cast<size_t>(BufferLength), true, std::numeric_limits<SQLSMALLINT>::max());
                        res = field.res;
                        ret = RDS_ProcessLibRes(SQL_HANDLE_ENV, env, res);
                        if (SQL_SUCCEEDED(ret)) {
                            if (StringLengthPtr) {
                                *StringLengthPtr = static_cast<SQLSMALLINT>(field.length);
                            }
                            // Cut by the conversion only; SQLGetDiagField posts no record about itself.
                            if (field.truncated && ret == SQL_SUCCESS) {
                                ret = SQL_SUCCESS_WITH_INFO;
                            }
                        }
                    } else
#endif
                    {
                        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagField>(env->driver_lib_loader, RdsFuncNames::GetDiagField,
                            HandleType, env->wrapped_env, RecNumber, DiagIdentifier, DiagInfoPtr, BufferLength, StringLengthPtr
                        );
                        ret = RDS_ProcessLibRes(SQL_HANDLE_ENV, env, res);
                    }
                } else if (env->err) {
                    err.emplace(*env->err);
                }
            }
            break;
        case SQL_HANDLE_DBC:
            if (!HasEnvAccess<DBC>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                dbc = static_cast<DBC*>(Handle);
                env = dbc->env;
                const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
                // The wrapper's own record first, as in RDS_SQLGetDiagRec: it replaces the base driver's.
                const bool wrapper_record = dbc->err && !dbc->err->sqlstate.empty();
                if (dbc->wrapped_dbc && !wrapper_record && !dbc->base_diag_stale) {
                    base_record = true;
#if UNICODE
                    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
                    if (odbc_helper->NeedsConversion() && BufferLength >= 0
                        && OdbcHelper::IsStringDiagField(DiagIdentifier)) {
                        // BufferLength and the length count bytes of the app's characters; the base driver's count its own.
                        const DriverStringResult field = odbc_helper->FetchDriverString(
                            [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                                SQLSMALLINT field_length = 0;
                                const RdsLibResult result = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagField>(env->driver_lib_loader, RdsFuncNames::GetDiagField,
                                    HandleType, dbc->wrapped_dbc, RecNumber, DiagIdentifier, buffer, static_cast<SQLSMALLINT>(length), &field_length
                                );
                                *full_length = field_length > 0 ? static_cast<size_t>(field_length) : 0;
                                return result;
                            },
                            DiagInfoPtr, static_cast<size_t>(BufferLength), true, std::numeric_limits<SQLSMALLINT>::max());
                        res = field.res;
                        ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
                        if (SQL_SUCCEEDED(ret)) {
                            if (StringLengthPtr) {
                                *StringLengthPtr = static_cast<SQLSMALLINT>(field.length);
                            }
                            // Cut by the conversion only; SQLGetDiagField posts no record about itself.
                            if (field.truncated && ret == SQL_SUCCESS) {
                                ret = SQL_SUCCESS_WITH_INFO;
                            }
                        }
                    } else
#endif
                    {
                        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagField>(env->driver_lib_loader, RdsFuncNames::GetDiagField,
                            HandleType, dbc->wrapped_dbc, RecNumber, DiagIdentifier, DiagInfoPtr, BufferLength, StringLengthPtr
                        );
                        ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
                    }
                } else if (dbc->err) {
                    err.emplace(*dbc->err);
                }
            }
            break;
        case SQL_HANDLE_STMT:
            if (!HasEnvAccess<STMT>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                stmt = static_cast<STMT*>(Handle);
                dbc = stmt->dbc;
                env = dbc->env;
                const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
                // The wrapper's own record first, as in RDS_SQLGetDiagRec: it replaces the base driver's.
                const bool wrapper_record = stmt->err && !stmt->err->sqlstate.empty();
                if (stmt->wrapped_stmt && !wrapper_record) {
                    base_record = true;
#if UNICODE
                    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
                    if (odbc_helper->NeedsConversion() && BufferLength >= 0
                        && OdbcHelper::IsStringDiagField(DiagIdentifier)) {
                        // BufferLength and the length count bytes of the app's characters; the base driver's count its own.
                        const DriverStringResult field = odbc_helper->FetchDriverString(
                            [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                                SQLSMALLINT field_length = 0;
                                const RdsLibResult result = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagField>(env->driver_lib_loader, RdsFuncNames::GetDiagField,
                                    HandleType, stmt->wrapped_stmt, RecNumber, DiagIdentifier, buffer, static_cast<SQLSMALLINT>(length), &field_length
                                );
                                *full_length = field_length > 0 ? static_cast<size_t>(field_length) : 0;
                                return result;
                            },
                            DiagInfoPtr, static_cast<size_t>(BufferLength), true, std::numeric_limits<SQLSMALLINT>::max());
                        res = field.res;
                        ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
                        if (SQL_SUCCEEDED(ret)) {
                            if (StringLengthPtr) {
                                *StringLengthPtr = static_cast<SQLSMALLINT>(field.length);
                            }
                            // Cut by the conversion only; SQLGetDiagField posts no record about itself.
                            if (field.truncated && ret == SQL_SUCCESS) {
                                ret = SQL_SUCCESS_WITH_INFO;
                            }
                        }
                    } else
#endif
                    {
                        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagField>(env->driver_lib_loader, RdsFuncNames::GetDiagField,
                            HandleType, stmt->wrapped_stmt, RecNumber, DiagIdentifier, DiagInfoPtr, BufferLength, StringLengthPtr
                        );
                        ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
                    }
                } else if (stmt->err) {
                    err.emplace(*stmt->err);
                }
            }
            break;
        case SQL_HANDLE_DESC:
            if (!HasEnvAccess<DESC>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                desc = static_cast<DESC*>(Handle);
                dbc = desc->dbc;
                env = dbc->env;

                const std::lock_guard<std::recursive_mutex> lock_guard(desc->lock);

                // The wrapper's own record first, as in RDS_SQLGetDiagRec: it replaces the base driver's.
                const bool wrapper_record = desc->err && !desc->err->sqlstate.empty();
                if (desc->wrapped_desc && !wrapper_record) {
                    base_record = true;
#if UNICODE
                    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
                    if (odbc_helper->NeedsConversion() && BufferLength >= 0
                        && OdbcHelper::IsStringDiagField(DiagIdentifier)) {
                        // BufferLength and the length count bytes of the app's characters; the base driver's count its own.
                        const DriverStringResult field = odbc_helper->FetchDriverString(
                            [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                                SQLSMALLINT field_length = 0;
                                const RdsLibResult result = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagField>(env->driver_lib_loader, RdsFuncNames::GetDiagField,
                                    HandleType, desc->wrapped_desc, RecNumber, DiagIdentifier, buffer, static_cast<SQLSMALLINT>(length), &field_length
                                );
                                *full_length = field_length > 0 ? static_cast<size_t>(field_length) : 0;
                                return result;
                            },
                            DiagInfoPtr, static_cast<size_t>(BufferLength), true, std::numeric_limits<SQLSMALLINT>::max());
                        res = field.res;
                        ret = RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
                        if (SQL_SUCCEEDED(ret)) {
                            if (StringLengthPtr) {
                                *StringLengthPtr = static_cast<SQLSMALLINT>(field.length);
                            }
                            // Cut by the conversion only; SQLGetDiagField posts no record about itself.
                            if (field.truncated && ret == SQL_SUCCESS) {
                                ret = SQL_SUCCESS_WITH_INFO;
                            }
                        }
                    } else
#endif
                    {
                        res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagField>(env->driver_lib_loader, RdsFuncNames::GetDiagField,
                            HandleType, desc->wrapped_desc, RecNumber, DiagIdentifier, DiagInfoPtr, BufferLength, StringLengthPtr
                        );
                        ret = RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
                    }
                } else if (desc->err) {
                    err.emplace(*desc->err);
                }
            }
            break;
        default:
            return SQL_INVALID_HANDLE;
    }

    if (!err && !base_record) {
        // No record: none was posted, or the wrapper answered the last call itself. SQL_DIAG_NUMBER is 0, and a
        // record field is SQL_NO_DATA.
        if (DiagIdentifier == SQL_DIAG_NUMBER) {
            if (DiagInfoPtr) {
                *(static_cast<SQLINTEGER*>(DiagInfoPtr)) = 0;
            }
        } else if (RecNumber > 0) {
            ret = SQL_NO_DATA_FOUND;
        }
    }

    // The wrapper's own record, or a handle without an underlying driver handle
    if (err) {
        if (RecNumber > 1) {
            ret = SQL_NO_DATA_FOUND;
        } else {
            std::optional<std::string_view> char_value;
            ret = SQL_SUCCESS;
            switch (DiagIdentifier) {
                // Header Fields
                case SQL_DIAG_CURSOR_ROW_COUNT:
                case SQL_DIAG_DYNAMIC_FUNCTION:
                case SQL_DIAG_DYNAMIC_FUNCTION_CODE:
                case SQL_DIAG_ROW_COUNT:
                {
                    // Defined for statement handles only. A wrapper record comes from a call the wrapper
                    // answered itself, which executed nothing: no rows and an unknown statement.
                    if (HandleType != SQL_HANDLE_STMT) {
                        ret = SQL_ERROR;
                    } else if (DiagIdentifier == SQL_DIAG_DYNAMIC_FUNCTION) {
                        char_value = "";
                    } else if (DiagIdentifier == SQL_DIAG_DYNAMIC_FUNCTION_CODE) {
                        if (DiagInfoPtr) {
                            *(static_cast<SQLINTEGER*>(DiagInfoPtr)) = SQL_DIAG_UNKNOWN_STATEMENT;
                        }
                    } else if (DiagInfoPtr) {
                        *(static_cast<SQLLEN*>(DiagInfoPtr)) = 0;
                    }
                    break;
                }
                case SQL_DIAG_NUMBER:
                {
                    // No support for multi-error / list
                    //  at most 1 for our Wrapper Layer
                    *(static_cast<SQLINTEGER*>(DiagInfoPtr)) = 1;
                    break;
                }
                case SQL_DIAG_RETURNCODE:
                    // Driver manager's response
                    break;

                // Record Fields
                case SQL_DIAG_CLASS_ORIGIN:
                {
                    if (RecNumber <= 0) {
                        ret = SQL_ERROR;
                    } else {
                        if (err->sqlstate.starts_with("IM")) {
                            char_value = "ODBC 3.0";
                        } else {
                            char_value = "ISO 9075";
                        }
                    }
                    break;
                }
                case SQL_DIAG_COLUMN_NUMBER:
                {
                    if (RecNumber <= 0) {
                        ret = SQL_ERROR;
                    } else {
                        // An SQLINTEGER, unlike SQL_DIAG_ROW_NUMBER's SQLLEN: 8 bytes would overrun the field.
                        *(static_cast<SQLINTEGER*>(DiagInfoPtr)) = SQL_COLUMN_NUMBER_UNKNOWN;
                    }
                    break;
                }
                case SQL_DIAG_CONNECTION_NAME:
                {
                    if (RecNumber <= 0) {
                        ret = SQL_ERROR;
                    }
                    if (dbc != nullptr && dbc->conn_attr.contains(KEY_DSN)) {
                        char_value = dbc->conn_attr.at(KEY_DSN);
                    } else {
                        char_value = "";
                    }
                    break;
                }
                case SQL_DIAG_MESSAGE_TEXT:
                {
                    if (RecNumber <= 0) {
                        ret = SQL_ERROR;
                    }
                    char_value = err->error_msg;
                    break;
                }
                case SQL_DIAG_NATIVE:
                {
                    *(static_cast<SQLINTEGER*>(DiagInfoPtr)) = err->native_err;
                    break;
                }
                case SQL_DIAG_ROW_NUMBER:
                {
                    if (RecNumber <= 0) {
                        ret = SQL_ERROR;
                    } else {
                        *(static_cast<SQLLEN*>(DiagInfoPtr)) = SQL_ROW_NUMBER_UNKNOWN;
                    }
                    break;
                }
                case SQL_DIAG_SERVER_NAME:
                {
                    if (dbc != nullptr && dbc->conn_attr.contains(KEY_SERVER)) {
                        char_value = dbc->conn_attr.at(KEY_SERVER);
                    } else {
                        char_value = "";
                    }
                    break;
                }
                case SQL_DIAG_SQLSTATE:
                {
                    if (RecNumber <= 0) {
                        ret = SQL_ERROR;
                    } else {
                        char_value = err->sqlstate;
                    }
                    break;
                }
                case SQL_DIAG_SUBCLASS_ORIGIN:
                {
                    if (RecNumber <= 0) {
                        ret = SQL_ERROR;
                    }
                    if (err->is_odbc3_subclass) {
                        char_value = "ODBC 3.0";
                    } else {
                        char_value = "ISO 9075";
                    }
                    break;
                }
                default:
                    ret = SQL_ERROR;
            }

            // Pass info back to caller. BufferLength and *StringLengthPtr count bytes of the caller's characters.
            if (BufferLength < 0 && OdbcHelper::IsStringDiagField(DiagIdentifier)) {
                // A string field needs a BufferLength of 0 or more; nothing is written.
                ret = SQL_ERROR;
            } else if (char_value) {
#ifdef UNICODE
                // A W caller reads SQLWCHARs (4 bytes each for a 4-byte SQLWCHAR app), as RDS_SQLGetDiagRec writes.
                const bool user_4_byte = env != nullptr && env->use_4_bytes_user_app.load();
                const SQLULEN char_bytes = user_4_byte ? 2 * sizeof(SQLTCHAR) : sizeof(SQLTCHAR);
#else
                const SQLULEN char_bytes = sizeof(SQLTCHAR);
#endif
                // The room in whole characters of the app, the terminator's included: bytes past the last whole
                // character are unused.
                const size_t buf_chars = DiagInfoPtr && BufferLength > 0 ? static_cast<size_t>(BufferLength) / char_bytes : 0;
#ifdef UNICODE
                // The length in the app's characters, not in UTF-8 bytes; a value that doesn't fit is cut to a
                // terminated prefix.
                len = CopyUTF8ToAppChars(*char_value, DiagInfoPtr, buf_chars, user_4_byte);
#else
                len = char_value->size();
#endif
                const SQLULEN byte_len = len * char_bytes;
                if (DiagInfoPtr) {
#ifndef UNICODE
                    if (buf_chars > 0) {
                        const size_t copy_len = std::min(len, buf_chars - 1);
                        auto* const out_buf = static_cast<char*>(DiagInfoPtr);
                        std::copy_n(char_value->begin(), copy_len, out_buf);
                        out_buf[copy_len] = '\0';
                    }
#endif
                    // Cut unless the value and its terminator fit those characters.
                    if (len >= buf_chars) {
                        ret = SQL_SUCCESS_WITH_INFO;
                    }
                } else {
                    ret = SQL_SUCCESS_WITH_INFO;
                }
                if (StringLengthPtr) {
                    *StringLengthPtr = static_cast<SQLSMALLINT>(byte_len);
                }
            }
        }
    }

    return ret;
}

SQLRETURN RDS_SQLGetDiagRec(
    SQLSMALLINT    HandleType,
    SQLHANDLE      Handle,
    SQLSMALLINT    RecNumber,
    SQLTCHAR *     SQLState,
    SQLINTEGER *   NativeErrorPtr,
    SQLTCHAR *     MessageText,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  TextLengthPtr,
    bool           WrapperCall)
{
    DESC* desc = nullptr;
    STMT* stmt = nullptr;
    DBC* dbc = nullptr;
    ENV* env = nullptr;
    RdsLibResult res;
    SQLRETURN ret = SQL_ERROR;
    std::optional<ErrInfo> err;
    bool has_underlying_data = false;

    // BufferLength counts characters and can't be negative: an error, with nothing written and no record (SQLGetDiagRec
    // posts none about itself). unixODBC passes the request for a driver's record through without checking it.
    if (BufferLength < 0) {
        return RDS_GetEnvFromHandle(HandleType, Handle) ? SQL_ERROR : SQL_INVALID_HANDLE;
    }

#if UNICODE
    env = RDS_GetEnvFromHandle(HandleType, Handle);
    SQLTCHAR new_state_buffer[MAX_SQL_STATE_LEN * 2] = {0};
    std::vector<SQLTCHAR> new_msg_vector(static_cast<size_t>(BufferLength) * 2, '\0');
    SQLTCHAR* new_msg_buffer = new_msg_vector.data();

    const std::shared_ptr<OdbcHelper> odbc_helper = RDS_GetOdbcHelper(HandleType, Handle);
    const bool user_4_byte = env != nullptr && env->use_4_bytes_user_app.load();

    size_t app_state_bytes = MAX_SQL_STATE_LEN * sizeof(SQLTCHAR);
    size_t app_msg_bytes = BufferLength * sizeof(SQLTCHAR);
    if (user_4_byte) {
        app_state_bytes *= 2;
        app_msg_bytes *= 2;
    }

    // The base driver's record, its message in the app's characters. When they (2-byte ones for a wrapper call) and the
    // base driver's differ in size, the message's length counts the app's characters, from the whole message.
    const auto get_base_record = [&](SQLHANDLE wrapped_handle) {
        if (odbc_helper && (user_4_byte && !WrapperCall) != odbc_helper->GetUse4BytesBaseDriver()) {
            const DriverStringResult message = odbc_helper->FetchDriverString(
                [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                    SQLSMALLINT text_length = 0;
                    const RdsLibResult result = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagRec>(env->driver_lib_loader, RdsFuncNames::GetDiagRec,
                        HandleType, wrapped_handle, RecNumber, new_state_buffer, NativeErrorPtr, buffer, static_cast<SQLSMALLINT>(length), &text_length
                    );
                    *full_length = text_length > 0 ? static_cast<size_t>(text_length) : 0;
                    return result;
                },
                MessageText, static_cast<size_t>(BufferLength), false, std::numeric_limits<SQLSMALLINT>::max(), WrapperCall);
            RdsLibResult result = message.res;
            if (SQL_SUCCEEDED(result.fn_result)) {
                if (TextLengthPtr) {
                    *TextLengthPtr = static_cast<SQLSMALLINT>(message.length);
                }
                // Cut by the conversion only; SQLGetDiagRec posts no record about itself.
                if (message.truncated) {
                    result.fn_result = SQL_SUCCESS_WITH_INFO;
                }
            }
            return result;
        }
        const RdsLibResult result = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagRec>(env->driver_lib_loader, RdsFuncNames::GetDiagRec,
            HandleType, wrapped_handle, RecNumber, new_state_buffer, NativeErrorPtr, new_msg_buffer, BufferLength, TextLengthPtr
        );
        if (odbc_helper && MessageText) {
            odbc_helper->ConvertDriverOutputToTarget(WrapperCall, new_msg_buffer, MessageText, new_msg_vector.size() * sizeof(SQLTCHAR), app_msg_bytes);
        }
        return result;
    };
#endif

    // Use ERR from Wrapper if exist
    //  otherwise try underlying driver
    switch (HandleType) {
        case SQL_HANDLE_ENV:
            if (!HasEnvAccess<ENV>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                env = static_cast<ENV*>(Handle);

                const std::lock_guard<std::recursive_mutex> lock_guard(env->lock);

                if (env->err && !env->err->sqlstate.empty()) {
                    err.emplace(*env->err);
                } else if (env->wrapped_env && !env->base_diag_stale) {
                    has_underlying_data = true;
#if UNICODE
                    res = get_base_record(env->wrapped_env);
                    ret = RDS_ProcessLibRes(SQL_HANDLE_ENV, env, res);

                    if (odbc_helper && SQLState) {
                        odbc_helper->ConvertDriverOutputToTarget(WrapperCall, new_state_buffer, SQLState, sizeof(new_state_buffer), app_state_bytes);
                    }
#else
                    res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagRec>(env->driver_lib_loader, RdsFuncNames::GetDiagRec,
                        HandleType, env->wrapped_env, RecNumber, SQLState, NativeErrorPtr, MessageText, BufferLength, TextLengthPtr
                    );
                    ret = RDS_ProcessLibRes(SQL_HANDLE_ENV, env, res);
#endif
                }
            }
            break;
        case SQL_HANDLE_DBC:
            if (!HasEnvAccess<DBC>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                dbc = static_cast<DBC*>(Handle);
                env = dbc->env;

                const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);

                if (dbc->err && !dbc->err->sqlstate.empty()) {
                    err.emplace(*dbc->err);
                } else if (dbc->wrapped_dbc && !dbc->base_diag_stale) {
                    has_underlying_data = true;
#if UNICODE
                    res = get_base_record(dbc->wrapped_dbc);
                    ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
                    if (odbc_helper && SQLState) {
                        odbc_helper->ConvertDriverOutputToTarget(WrapperCall, new_state_buffer, SQLState, sizeof(new_state_buffer), app_state_bytes);
                    }
#else
                    res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagRec>(env->driver_lib_loader, RdsFuncNames::GetDiagRec,
                        HandleType, dbc->wrapped_dbc, RecNumber, SQLState, NativeErrorPtr, MessageText, BufferLength, TextLengthPtr
                    );
                    ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
#endif
                }
            }
            break;
        case SQL_HANDLE_STMT:
            if (!HasEnvAccess<STMT>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                stmt = static_cast<STMT*>(Handle);
                dbc = stmt->dbc;
                env = dbc->env;

                const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);

                if (stmt->err && !stmt->err->sqlstate.empty()) {
                    err.emplace(*stmt->err);
                } else if (stmt->wrapped_stmt) {
                    has_underlying_data = true;
#if UNICODE
                    res = get_base_record(stmt->wrapped_stmt);
                    ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);

                    if (odbc_helper && SQLState) {
                        odbc_helper->ConvertDriverOutputToTarget(WrapperCall, new_state_buffer, SQLState, sizeof(new_state_buffer), app_state_bytes);
                    }
#else
                    res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagRec>(env->driver_lib_loader, RdsFuncNames::GetDiagRec,
                        HandleType, stmt->wrapped_stmt, RecNumber, SQLState, NativeErrorPtr, MessageText, BufferLength, TextLengthPtr
                    );
                    ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
#endif
                }
            }
            break;
        case SQL_HANDLE_DESC:
            if (!HasEnvAccess<DESC>(Handle)) {
                return SQL_INVALID_HANDLE;
            }
            {
                desc = static_cast<DESC*>(Handle);
                dbc = desc->dbc;
                env = dbc->env;

                const std::lock_guard<std::recursive_mutex> lock_guard(desc->lock);

                if (desc->err && !desc->err->sqlstate.empty()) {
                    err.emplace(*desc->err);
                } else if (desc->wrapped_desc) {
                    has_underlying_data = true;
#if UNICODE
                    res = get_base_record(desc->wrapped_desc);
                    ret = RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);

                    if (odbc_helper && SQLState) {
                        odbc_helper->ConvertDriverOutputToTarget(WrapperCall, new_state_buffer, SQLState, sizeof(new_state_buffer), app_state_bytes);
                    }
#else
                    res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDiagRec>(env->driver_lib_loader, RdsFuncNames::GetDiagRec,
                        HandleType, desc->wrapped_desc, RecNumber, SQLState, NativeErrorPtr, MessageText, BufferLength, TextLengthPtr
                    );
                    ret = RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
#endif
                }
            }
            break;
        default:
            return SQL_INVALID_HANDLE;
    }

    if (err) {
        // No support for multi-error / list
        //  at most 1 for our Wrapper Layer
        if (RecNumber != 1) {
            return SQL_NO_DATA_FOUND;
        }

        ret = SQL_SUCCESS;
        if (SQLState) {
#ifdef UNICODE
            CopyUTF8ToUTF16Buffer(reinterpret_cast<uint16_t*>(SQLState), MAX_SQL_STATE_LEN, err->sqlstate);
            OdbcHelper::ConvertWrapperOutputToTarget(user_4_byte, WrapperCall, SQLState, MAX_SQL_STATE_LEN - 1, static_cast<size_t>(MAX_SQL_STATE_LEN) * 2 * sizeof(SQLTCHAR));
#else
            snprintf(reinterpret_cast<char*>(SQLState), MAX_SQL_STATE_LEN, "%s", err->sqlstate.c_str());
#endif
        }
#ifdef UNICODE
        // The length in the app's characters (UTF-16 units, or code points for a 4-byte SQLWCHAR), not in UTF-8
        // bytes; a message that doesn't fit is cut to a terminated prefix of BufferLength characters.
        const size_t msg_chars = MessageText ? static_cast<size_t>(BufferLength) : 0;
        const auto err_len = static_cast<SQLLEN>(
            CopyUTF8ToAppChars(err->error_msg, MessageText, msg_chars, user_4_byte && !WrapperCall));
#else
        const auto err_len = static_cast<SQLLEN>(err->error_msg.size());
        if (MessageText && (BufferLength > 0)) {
            snprintf(reinterpret_cast<char*>(MessageText), static_cast<size_t>(BufferLength) / sizeof(SQLTCHAR), "%s", err->error_msg.c_str());
        }
#endif
        if (TextLengthPtr) {
            *TextLengthPtr = static_cast<SQLSMALLINT>(err_len);
            if (BufferLength == 0) {
                ret = SQL_SUCCESS_WITH_INFO;
            }
        }
        if (MessageText && (BufferLength > 0) && err_len >= BufferLength) {
            ret = SQL_SUCCESS_WITH_INFO;
        }
        if (NativeErrorPtr) {
            *NativeErrorPtr = static_cast<SQLINTEGER>(err->native_err);
        }
    } else if (!has_underlying_data) {
        // No Data from wrapper or underlying driver
        //  Set states and clear buffers
        if (SQLState) {
#ifdef UNICODE
            CopyUTF8ToUTF16Buffer(reinterpret_cast<uint16_t*>(SQLState), MAX_SQL_STATE_LEN, NO_DATA_SQL_STATE);
            // Expand to 4-byte if user app expects it.
            OdbcHelper::ConvertWrapperOutputToTarget(user_4_byte, WrapperCall, SQLState, MAX_SQL_STATE_LEN - 1, static_cast<size_t>(MAX_SQL_STATE_LEN) * 2 * sizeof(SQLTCHAR));
#else
            snprintf(reinterpret_cast<char*>(SQLState), MAX_SQL_STATE_LEN, "%s", NO_DATA_SQL_STATE);
#endif
        }
        if (MessageText && (BufferLength > 0)) {
#ifdef UNICODE
            // A terminator in the app's characters: 4 bytes for a 4-byte SQLWCHAR app.
            CopyUTF8ToAppChars("", MessageText, static_cast<size_t>(BufferLength), user_4_byte && !WrapperCall);
#else
            MessageText[0] = '\0';
#endif
        }
        if (NativeErrorPtr) {
            *NativeErrorPtr = static_cast<SQLINTEGER>(NO_DATA_NATIVE_ERR);
        }
        if (TextLengthPtr) {
            *TextLengthPtr = static_cast<SQLSMALLINT>(NO_DATA_NATIVE_ERR);
        }
        ret = SQL_NO_DATA_FOUND;
    }

    return ret;
}

SQLRETURN RDS_SQLGetInfo(
    SQLHDBC        ConnectionHandle,
    SQLUSMALLINT   InfoType,
    SQLPOINTER     InfoValuePtr,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr)
{
    SQLRETURN ret = SQL_ERROR;
    SQLULEN len = sizeof(SQLSMALLINT);
    SQLULEN value = 0;
    const char* char_value = nullptr;
    char odbcver[ODBC_VER_SIZE];

    // Query underlying driver if connection is established
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);

    if (dbc == nullptr) {
        return SQL_INVALID_HANDLE;
    }

    // BufferLength can't be negative, for any info type (HY090): driver managers reject it before calling the driver,
    // and the base driver doesn't check it either. Nothing is written.
    if (BufferLength < 0) {
        const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
        ClearError(dbc);
        dbc->err = std::make_unique<ErrInfo>("SQLGetInfo - BufferLength is less than 0", ERR_INVALID_STRING_OR_BUFFER_LENGTH);
        return SQL_ERROR;
    }

    // Aurora DSQL has one isolation level; psqlODBC reports PostgreSQL's four.
    if (DsqlCompatibility::TryGetInfo(dbc, InfoType, InfoValuePtr, StringLengthPtr)) {
        return SQL_SUCCESS;
    }

    // Naming and version for Wrapper
    switch (InfoType) {
        case SQL_DRIVER_NAME:
            char_value = DRIVER_NAME;
            break;
        case SQL_DRIVER_VER:
            char_value = DRIVER_VERSION;
            break;
        default:
            break;
    }

    if (!char_value) {
        {
            const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
            if (dbc->wrapped_dbc) {
                const ENV* env = dbc->env;
                ClearError(dbc);
#if UNICODE
                const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
                // Only a string is in the base driver's characters; any other value goes straight to the app.
                if (odbc_helper->NeedsConversion() && BufferLength >= 0 && OdbcHelper::IsStringInfoType(InfoType)) {
                    // BufferLength and the length count bytes of the app's characters; the base driver's count its own.
                    const DriverStringResult info = odbc_helper->FetchDriverString(
                        [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                            SQLSMALLINT info_length = 0;
                            const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetInfo>(env->driver_lib_loader, RdsFuncNames::GetInfo,
                                dbc->wrapped_dbc, InfoType, buffer, static_cast<SQLSMALLINT>(length), &info_length
                            );
                            *full_length = info_length > 0 ? static_cast<size_t>(info_length) : 0;
                            return res;
                        },
                        InfoValuePtr, static_cast<size_t>(BufferLength), true, std::numeric_limits<SQLSMALLINT>::max());
                    SQLRETURN info_ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, info.res);
                    if (SQL_SUCCEEDED(info_ret)) {
                        if (StringLengthPtr) {
                            *StringLengthPtr = static_cast<SQLSMALLINT>(info.length);
                        }
                        info_ret = ReportConversionCut(dbc, info_ret, info.truncated, "SQLGetInfo - The value",
                            info.length + odbc_helper->GetTargetByteCount(false, 1), "bytes");
                    }
                    return info_ret;
                }
#endif
                const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetInfo>(env->driver_lib_loader, RdsFuncNames::GetInfo,
                    dbc->wrapped_dbc, InfoType, InfoValuePtr, BufferLength, StringLengthPtr
                );
                return RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
            }
        }

        // Get info for shell driver
        switch (InfoType) {
            case SQL_DRIVER_ODBC_VER:
                snprintf(odbcver, ODBC_VER_SIZE, "%02x.%02x", ODBCVER / ODBCVER_BITS, ODBCVER % ODBCVER_BITS);
                char_value = odbcver;
                break;
            case SQL_MAX_CONCURRENT_ACTIVITIES:
                value = 0; // No Limit
                break;
            case SQL_ASYNC_DBC_FUNCTIONS:
            case SQL_ASYNC_NOTIFICATION:
                value = 1; // "Supported"
                len = sizeof(SQLUINTEGER);  // Both are SQLUINTEGERs
                break;
            // TODO - Add other cases as needed
            default:
                const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
                LOG(ERROR) << "[" << InfoType << "] not implemented for AWS Advanced ODBC Wrapper's SQLGetInfo";
                ClearError(dbc);
                dbc->err = std::make_unique<ErrInfo>("SQLGetInfo - API Unsupported", ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
                RDS_NOT_IMPLEMENTED;
        }
    }
    ret = SQL_SUCCESS;
    // The wrapper answers this call itself: its record, a 01004 below or none, replaces any earlier one, and the base
    // driver's are an earlier call's.
    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    ClearError(dbc);
    dbc->base_diag_stale = dbc->wrapped_dbc != nullptr;

    // Pass info back to caller. BufferLength and *StringLengthPtr count bytes of the caller's characters.
    if (char_value) {
#ifdef UNICODE
        // A W caller reads SQLWCHARs, 4 bytes each for a 4-byte SQLWCHAR app, before a connect too.
        const bool user_4_byte = dbc->env != nullptr && dbc->env->use_4_bytes_user_app.load();
        const SQLULEN char_bytes = user_4_byte ? 2 * sizeof(SQLTCHAR) : sizeof(SQLTCHAR);
#else
        const SQLULEN char_bytes = sizeof(SQLTCHAR);
#endif
        // The room in whole characters of the app, the terminator's included: bytes past the last whole character
        // are unused.
        const size_t buf_chars = InfoValuePtr && BufferLength > 0 ? static_cast<size_t>(BufferLength) / char_bytes : 0;
#ifdef UNICODE
        // The length in the app's characters; a value that doesn't fit is cut to a terminated prefix.
        len = CopyUTF8ToAppChars(char_value, InfoValuePtr, buf_chars, user_4_byte);
#else
        len = strlen(char_value);
#endif
        const SQLULEN byte_len = len * char_bytes;
        if (InfoValuePtr) {
#ifndef UNICODE
            snprintf(static_cast<char*>(InfoValuePtr), buf_chars, "%s", char_value);
#endif
            // Cut unless the value and its terminator fit those characters: 01004.
            if (len >= buf_chars) {
                const std::string message = "SQLGetInfo - The value was truncated: the buffer needs "
                    + std::to_string(byte_len + char_bytes) + " bytes";
                dbc->err = std::make_unique<ErrInfo>(message.c_str(), WARN_STRING_DATA_RIGHT_TRUNCATED);
                ret = SQL_SUCCESS_WITH_INFO;
            }
        }
        if (StringLengthPtr) {
            *StringLengthPtr = static_cast<SQLSMALLINT>(byte_len);
        }
    } else {
        if (InfoValuePtr) {
            if (len == sizeof(SQLSMALLINT)) {
                *(static_cast<SQLUSMALLINT*>(InfoValuePtr)) = static_cast<SQLUSMALLINT>(value);
            }
            else if (len == sizeof(SQLINTEGER)) {
                *(static_cast<SQLUINTEGER*>(InfoValuePtr)) = static_cast<SQLUINTEGER>(value);
            }
        }
        if (StringLengthPtr) {
            *StringLengthPtr = static_cast<SQLSMALLINT>(len);
        }
    }

    return ret;
}

SQLRETURN RDS_SQLGetStmtAttr(
    SQLHSTMT       StatementHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     BufferLength,
    SQLINTEGER *   StringLengthPtr)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    // Guard before the SQL_ATTR_*_DESC cases below, which forward wrapped_stmt
    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    RdsLibResult res;
    switch (Attribute) {
        // An explicit ARD or APD the app set, without asking the base driver: its answer is the explicit one's base
        // descriptor, not the implicit one's.
        case SQL_ATTR_APP_ROW_DESC:
            if (stmt->explicit_row_desc) {
                *(static_cast<SQLPOINTER*>(ValuePtr)) = stmt->explicit_row_desc;
                return SQL_SUCCESS;
            }
            res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetStmtAttr>(env->driver_lib_loader, RdsFuncNames::GetStmtAttr,
                stmt->wrapped_stmt, Attribute, static_cast<SQLPOINTER>(&stmt->app_row_desc->wrapped_desc), BufferLength,
                StringLengthPtr
            );
            *(static_cast<SQLPOINTER*>(ValuePtr)) = stmt->app_row_desc;
            return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
        case SQL_ATTR_APP_PARAM_DESC:
            if (stmt->explicit_param_desc) {
                *(static_cast<SQLPOINTER*>(ValuePtr)) = stmt->explicit_param_desc;
                return SQL_SUCCESS;
            }
            res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetStmtAttr>(env->driver_lib_loader, RdsFuncNames::GetStmtAttr,
                stmt->wrapped_stmt, Attribute, static_cast<SQLPOINTER>(&stmt->app_param_desc->wrapped_desc), BufferLength,
                StringLengthPtr
            );
            *(static_cast<SQLPOINTER*>(ValuePtr)) = stmt->app_param_desc;
            return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
        case SQL_ATTR_IMP_ROW_DESC:
            res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetStmtAttr>(env->driver_lib_loader, RdsFuncNames::GetStmtAttr,
                stmt->wrapped_stmt, Attribute, static_cast<SQLPOINTER>(&stmt->imp_row_desc->wrapped_desc), BufferLength,
                StringLengthPtr
            );
            *(static_cast<SQLPOINTER*>(ValuePtr)) = stmt->imp_row_desc;
            return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
        case SQL_ATTR_IMP_PARAM_DESC:
            res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetStmtAttr>(env->driver_lib_loader, RdsFuncNames::GetStmtAttr,
                stmt->wrapped_stmt, Attribute, static_cast<SQLPOINTER>(&stmt->imp_param_desc->wrapped_desc), BufferLength,
                StringLengthPtr
            );
            *(static_cast<SQLPOINTER*>(ValuePtr)) = stmt->imp_param_desc;
            return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
        default:
            break;
    }

    res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetStmtAttr>(env->driver_lib_loader, RdsFuncNames::GetStmtAttr,
        stmt->wrapped_stmt, Attribute, ValuePtr, BufferLength, StringLengthPtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLGetTypeInfo(
    SQLHSTMT       StatementHandle,
    SQLSMALLINT    DataType)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetTypeInfo>(env->driver_lib_loader, RdsFuncNames::GetTypeInfo,
        stmt->wrapped_stmt, DataType
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLNativeSql(
    SQLHDBC        ConnectionHandle,
    SQLTCHAR *     InStatementText,
    SQLINTEGER     TextLength1,
    SQLTCHAR *     OutStatementText,
    SQLINTEGER     BufferLength,
    SQLINTEGER *   TextLength2Ptr)
{
    if (!HasEnvAccess<DBC>(ConnectionHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(dbc->lock);
    ClearError(dbc);

    if (!HasWrappedHandle(dbc)) {
        return SQL_INVALID_HANDLE;
    }
    // BufferLength counts characters and can't be negative (HY090, as the driver manager checks).
    if (BufferLength < 0) {
        dbc->err = std::make_unique<ErrInfo>("SQLNativeSql - BufferLength is less than 0", ERR_INVALID_STRING_OR_BUFFER_LENGTH);
        return SQL_ERROR;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto stmt_converted = odbc_helper->ConvertInput(InStatementText, TextLength1);

#if UNICODE
    {
        if (odbc_helper->NeedsConversion()) {
            // BufferLength and the length count the app's characters; the base driver's count its own.
            const DriverStringResult text = odbc_helper->FetchDriverString(
                [&](SQLTCHAR* buffer, size_t length, size_t* full_length) {
                    SQLINTEGER text_length = 0;
                    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLNativeSql>(env->driver_lib_loader, RdsFuncNames::NativeSql,
                        dbc->wrapped_dbc, stmt_converted.tchar_ptr, stmt_converted.BaseLength(TextLength1), buffer, static_cast<SQLINTEGER>(length), &text_length
                    );
                    *full_length = text_length > 0 ? static_cast<size_t>(text_length) : 0;
                    return res;
                },
                OutStatementText, static_cast<size_t>(BufferLength), false, std::numeric_limits<SQLINTEGER>::max());
            SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, text.res);
            if (SQL_SUCCEEDED(ret)) {
                if (TextLength2Ptr) {
                    *TextLength2Ptr = static_cast<SQLINTEGER>(text.length);
                }
                ret = ReportConversionCut(dbc, ret, text.truncated, "SQLNativeSql - The statement text", text.length + 1, "characters");
            }
            return ret;
        }
    }
#endif

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLNativeSql>(env->driver_lib_loader, RdsFuncNames::NativeSql,
        dbc->wrapped_dbc,
            stmt_converted.tchar_ptr,
            stmt_converted.BaseLength(TextLength1),
            OutStatementText,
            BufferLength,
            TextLength2Ptr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
}

SQLRETURN RDS_SQLPrepare(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     StatementText,
    SQLINTEGER     TextLength)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    // Kept for SQLExecute, which passes no text down the plugin chain: what the base driver prepared, TextLength
    // characters unless that is SQL_NTS, and never past a NUL.
    stmt->prepared_text.clear();
    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto stmt_converted = odbc_helper->ConvertInput(StatementText, TextLength);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLPrepare>(env->driver_lib_loader, RdsFuncNames::Prepare,
        stmt->wrapped_stmt,
            stmt_converted.tchar_ptr,
            stmt_converted.BaseLength(TextLength)
    );
    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
    if (SQL_SUCCEEDED(ret) && StatementText != nullptr) {
        // TextLength characters (code points for a 4-byte app) or to the NUL for SQL_NTS, and never past a NUL, as
        // psqlODBC reads it.
#if UNICODE
        stmt->prepared_text = ConvertUserAppToUTF8(odbc_helper->GetUse4BytesUserApp(), StatementText, TextLength);
#else
        stmt->prepared_text = AnsiAppText(StatementText, TextLength);
#endif
    }
    return ret;
}

SQLRETURN RDS_SQLPrimaryKeys(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     SchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     TableName,
    SQLSMALLINT    NameLength3)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);
    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto catalog_converted = odbc_helper->ConvertInput(CatalogName, NameLength1);
    const auto schema_converted  = odbc_helper->ConvertInput(SchemaName,  NameLength2);
    const auto table_converted   = odbc_helper->ConvertInput(TableName,   NameLength3);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLPrimaryKeys>(env->driver_lib_loader, RdsFuncNames::PrimaryKeys,
        stmt->wrapped_stmt,
            catalog_converted.tchar_ptr,
            catalog_converted.BaseLength(NameLength1),
            schema_converted.tchar_ptr,
            schema_converted.BaseLength(NameLength2),
            table_converted.tchar_ptr,
            table_converted.BaseLength(NameLength3)
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLProcedureColumns(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     SchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     ProcName,
    SQLSMALLINT    NameLength3,
    SQLTCHAR *     ColumnName,
    SQLSMALLINT    NameLength4)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto catalog_converted = odbc_helper->ConvertInput(CatalogName, NameLength1);
    const auto schema_converted  = odbc_helper->ConvertInput(SchemaName,  NameLength2);
    const auto proc_converted    = odbc_helper->ConvertInput(ProcName,    NameLength3);
    const auto column_converted  = odbc_helper->ConvertInput(ColumnName,  NameLength4);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLProcedureColumns>(env->driver_lib_loader, RdsFuncNames::ProcedureColumns,
        stmt->wrapped_stmt,
            catalog_converted.tchar_ptr,
            catalog_converted.BaseLength(NameLength1),
            schema_converted.tchar_ptr,
            schema_converted.BaseLength(NameLength2),
            proc_converted.tchar_ptr,
            proc_converted.BaseLength(NameLength3),
            column_converted.tchar_ptr,
            column_converted.BaseLength(NameLength4)
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLProcedures(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     SchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     ProcName,
    SQLSMALLINT    NameLength3)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto catalog_converted = odbc_helper->ConvertInput(CatalogName, NameLength1);
    const auto schema_converted  = odbc_helper->ConvertInput(SchemaName,  NameLength2);
    const auto proc_converted    = odbc_helper->ConvertInput(ProcName,    NameLength3);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLProcedures>(env->driver_lib_loader, RdsFuncNames::Procedures,
        stmt->wrapped_stmt,
            catalog_converted.tchar_ptr,
            catalog_converted.BaseLength(NameLength1),
            schema_converted.tchar_ptr,
            schema_converted.BaseLength(NameLength2),
            proc_converted.tchar_ptr,
            proc_converted.BaseLength(NameLength3)
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLSetConnectOption(
    SQLHDBC        ConnectionHandle,
    SQLUSMALLINT   Option,
    SQLULEN        Param)
{
    SQLINTEGER value_len = 0;
    if (Option == SQL_ATTR_CURRENT_CATALOG) {
        value_len = SQL_NTS;
    }
    return RDS_SQLSetConnectAttr(ConnectionHandle, Option, reinterpret_cast<SQLPOINTER>(Param), value_len);
}

SQLRETURN RDS_SQLSetCursorName(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CursorName,
    SQLSMALLINT    NameLength)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;
    SQLRETURN ret = SQL_SUCCESS;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (stmt->wrapped_stmt) {
        const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
        const auto cursor_converted = odbc_helper->ConvertInput(CursorName, NameLength);

        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetCursorName>(env->driver_lib_loader, RdsFuncNames::SetCursorName,
            stmt->wrapped_stmt,
                cursor_converted.tchar_ptr,
                cursor_converted.BaseLength(NameLength)
        );
        ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
    }

    // NameLength characters (bytes in ANSI) and no further: their UTF-8 can be longer than NameLength bytes.
#ifdef UNICODE
    stmt->cursor_name = ConvertUserAppToUTF8(
        dbc->plugin_service->GetOdbcHelper()->GetUse4BytesUserApp(),
        CursorName, NameLength);
#else
    stmt->cursor_name = AnsiAppText(CursorName, NameLength);
#endif
    return ret;
}

namespace {
// Points a base descriptor's record at a copy and its length, as SQLBindCol and SQLBindParameter do. The data pointer
// goes last: setting another field after it may unbind the record.
SQLRETURN PointRecordAtCopy(const ENV* env, DESC* desc, SQLUSMALLINT record, SQLPOINTER data, SQLLEN* length) {
    const auto set_field = [env, desc, record](SQLSMALLINT field, SQLPOINTER value) {
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetDescField>(env->driver_lib_loader, RdsFuncNames::SetDescField,
            desc->wrapped_desc, static_cast<SQLSMALLINT>(record), field, value, SQL_IS_POINTER
        );
        return RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
    };
    SQLRETURN ret = set_field(SQL_DESC_OCTET_LENGTH_PTR, length);
    if (SQL_SUCCEEDED(ret)) {
        ret = set_field(SQL_DESC_INDICATOR_PTR, length);
    }
    if (SQL_SUCCEEDED(ret)) {
        ret = set_field(SQL_DESC_DATA_PTR, data);
    }
    return ret;
}

// After the base driver's SQLCopyDesc, the target's records are the source's, so its SQL_C_WCHAR ones point at the
// source's copies, which go with the source. The target gets copies of them of its own (new buffers and lengths, no
// shared pointer), its records point at those, and its old copies, which no record points at any more, go. A
// data-at-execution parameter's record points at the app's buffer and stays so. Returns ret, or the error of a record
// it couldn't point at its copy.
SQLRETURN CopyBoundBuffers(const ENV* env, const DESC* source, DESC* target, SQLRETURN ret) {
    std::vector<BoundColBuffer> columns;
    for (const BoundColBuffer& buffer : source->bound_col_buffers) {
        BoundColBuffer copy = buffer;
        copy.local_str_len = std::make_shared<SQLLEN>(*buffer.local_str_len);
        const SQLRETURN point_ret = PointRecordAtCopy(env, target, copy.column_number, copy.local_buf.data(),
                                                      copy.local_str_len.get());
        if (!SQL_SUCCEEDED(point_ret)) {
            ret = point_ret;
        }
        // Moving it keeps the buffers the base driver has.
        columns.push_back(std::move(copy));
    }
    std::vector<BoundParamBuffer> params;
    for (const BoundParamBuffer& buffer : source->bound_param_buffers) {
        BoundParamBuffer copy = buffer;
        copy.local_str_len = std::make_shared<SQLLEN>(*buffer.local_str_len);
        if (!copy.local_buf.empty()) {
            const SQLRETURN point_ret = PointRecordAtCopy(env, target, copy.param_number, copy.local_buf.data(),
                                                          copy.local_str_len.get());
            if (!SQL_SUCCEEDED(point_ret)) {
                ret = point_ret;
            }
        }
        params.push_back(std::move(copy));
    }
    target->bound_col_buffers = std::move(columns);
    target->bound_param_buffers = std::move(params);
    return ret;
}
}  // namespace

SQLRETURN RDS_SQLCopyDesc(
    SQLHDESC       SourceDescHandle,
    SQLHDESC       TargetDescHandle)
{
    if (!HasEnvAccess<DESC>(SourceDescHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DESC* src_desc = static_cast<DESC*>(SourceDescHandle);
    if (!HasWrappedHandle(src_desc)) {
        return SQL_INVALID_HANDLE;
    }

    // The target must be an already-allocated application descriptor.
    DESC* dst_desc = static_cast<DESC*>(TargetDescHandle);
    if (!HasWrappedHandle(dst_desc)) {
        return SQL_INVALID_HANDLE;
    }
    const DBC* src_dbc = src_desc->dbc;
    const ENV* src_env = src_dbc->env;

    // Both at once: SQLCopyDesc(A, B) and SQLCopyDesc(B, A) on two threads, each taking them in turn, would deadlock.
    // A copy onto itself takes the one recursive mutex twice.
    const std::scoped_lock lock_guard(src_desc->lock, dst_desc->lock);
    // The call's diagnostics go to the target.
    ClearError(dst_desc);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLCopyDesc>(src_env->driver_lib_loader, RdsFuncNames::CopyDesc,
        src_desc->wrapped_desc, dst_desc->wrapped_desc
    );
    // Only the target's fields change. It keeps its owner and its connection: a statement's implicit descriptor
    // stays the statement's and out of dbc->desc_list, and an explicit one stays on the connection it was allocated on.
    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_DESC, dst_desc, res);
    if (!SQL_SUCCEEDED(ret) || src_desc == dst_desc) {
        return ret;
    }
    // And its copies of the SQL_C_WCHAR bindings, as fresh copies of the source's.
    return CopyBoundBuffers(dst_desc->dbc->env, src_desc, dst_desc, ret);
}

#if UNICODE && !defined(_WIN32)
namespace {
std::string OneRowCopiesMessage(const char* function) {
    return std::string(function) + " - Arrays, row-wise binding and bind offsets aren't supported with the SQL_C_WCHAR"
        " bindings the wrapper converts between the application's and the driver's SQLWCHAR";
}

// The bindings a statement attribute or ODBC 2 option with this value has the base driver use for more than one row or
// parameter set, row-wise or at a bind offset: the ARD's, the APD's, or none.
enum class MultiRowBindings : std::uint8_t { NONE, ROWS, PARAMS };
MultiRowBindings MultiRowBindingsOf(SQLINTEGER attribute, SQLULEN value) {
    switch (attribute) {
        case SQL_ATTR_ROW_ARRAY_SIZE:
        case SQL_ROWSET_SIZE:
            return value > 1 ? MultiRowBindings::ROWS : MultiRowBindings::NONE;
        // Row-wise (not SQL_BIND_BY_COLUMN), or an offset pointer
        case SQL_ATTR_ROW_BIND_TYPE:
        case SQL_ATTR_ROW_BIND_OFFSET_PTR:
            static_assert(SQL_BIND_BY_COLUMN == 0);
            return value != 0 ? MultiRowBindings::ROWS : MultiRowBindings::NONE;
        case SQL_ATTR_PARAMSET_SIZE:
            return value > 1 ? MultiRowBindings::PARAMS : MultiRowBindings::NONE;
        case SQL_ATTR_PARAM_BIND_TYPE:
        case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
            static_assert(SQL_PARAM_BIND_BY_COLUMN == 0);
            return value != 0 ? MultiRowBindings::PARAMS : MultiRowBindings::NONE;
        default:
            return MultiRowBindings::NONE;
    }
}

std::string WideRecordMessage(const char* function) {
    return std::string(function) + " - A SQL_C_WCHAR record of an application descriptor isn't set through the descriptor"
        " while the wrapper converts SQL_C_WCHAR between the application's and the driver's SQLWCHAR: bind it with"
        " SQLBindCol or SQLBindParameter";
}

// A statement's ARD or APD, or a descriptor the app allocated (which can only be one of those).
bool IsAppDesc(const DESC* desc) {
    return desc->stmt == nullptr || desc == desc->stmt->app_row_desc || desc == desc->stmt->app_param_desc;
}

// Whether the descriptor's record has a copy of a SQL_C_WCHAR binding (SQLBindCol's or SQLBindParameter's), which the
// base driver's record points at.
bool HasRecordCopy(DESC* desc, SQLSMALLINT record) {
    const std::lock_guard<std::recursive_mutex> lock_guard(desc->lock);
    return std::any_of(desc->bound_col_buffers.begin(), desc->bound_col_buffers.end(),
                       [record](const BoundColBuffer& b) { return b.column_number == record; })
        || std::any_of(desc->bound_param_buffers.begin(), desc->bound_param_buffers.end(),
                       [record](const BoundParamBuffer& b) { return b.param_number == record; });
}

// Whether the base driver has the descriptor's record as SQL_C_WCHAR (an SQLCopyDesc from an IRD, say, makes one).
bool IsBaseWideRecord(const ENV* env, const DESC* desc, SQLSMALLINT record) {
    SQLSMALLINT type = 0;
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLGetDescField>(env->driver_lib_loader, RdsFuncNames::GetDescField,
        desc->wrapped_desc, record, SQL_DESC_CONCISE_TYPE, &type, SQL_IS_SMALLINT, nullptr
    );
    return res.fn_load_success && SQL_SUCCEEDED(res.fn_result) && type == SQL_C_WCHAR;
}

// SQLSetDescField on a record of an application descriptor (ARD or APD), while the wrapper copies SQL_C_WCHAR bindings:
// - A record with a copy keeps the fields its SQLBindCol or SQLBindParameter set (type, buffer, length, indicator):
//   the base driver's record points at the copy, and the copy back writes the app's buffer.
// - When the app's and the base driver's SQLWCHAR differ, a record made SQL_C_WCHAR, or a SQL_C_WCHAR one given a
//   buffer, length or indicator, would have the base driver read and write its own width in the app's buffer.
bool RefusesWideDescField(const DBC* dbc, DESC* desc, SQLSMALLINT record, SQLSMALLINT field, SQLPOINTER value) {
    switch (field) {
        case SQL_DESC_TYPE:
        case SQL_DESC_CONCISE_TYPE:
        case SQL_DESC_DATA_PTR:
        case SQL_DESC_OCTET_LENGTH:
        case SQL_DESC_OCTET_LENGTH_PTR:
        case SQL_DESC_INDICATOR_PTR:
            break;
        default:
            return false;
    }
    if (record < 1 || !IsAppDesc(desc)) {
        return false;
    }
    if (HasRecordCopy(desc, record)) {
        return true;
    }
    if (!dbc->plugin_service->GetOdbcHelper()->NeedsConversion()) {
        return false;
    }
    if (field == SQL_DESC_TYPE || field == SQL_DESC_CONCISE_TYPE) {
        return static_cast<SQLSMALLINT>(reinterpret_cast<SQLLEN>(value)) == SQL_C_WCHAR;
    }
    return field != SQL_DESC_OCTET_LENGTH && IsBaseWideRecord(dbc->env, desc, record);
}

// The same for SQLSetDescRec, which sets all of them.
bool RefusesWideDescRec(const DBC* dbc, DESC* desc, SQLSMALLINT record, SQLSMALLINT type) {
    if (record < 1 || !IsAppDesc(desc)) {
        return false;
    }
    return HasRecordCopy(desc, record) || (dbc->plugin_service->GetOdbcHelper()->NeedsConversion() && type == SQL_C_WCHAR);
}

// The same for a descriptor's header fields.
bool IsMultiRowDescField(SQLSMALLINT field, SQLPOINTER value) {
    switch (field) {
        case SQL_DESC_ARRAY_SIZE:
            return reinterpret_cast<SQLULEN>(value) > 1;
        case SQL_DESC_BIND_TYPE:
            return reinterpret_cast<SQLULEN>(value) != SQL_BIND_BY_COLUMN;
        case SQL_DESC_BIND_OFFSET_PTR:
            return value != nullptr;
        default:
            return false;
    }
}
}  // namespace

SQLRETURN RDS_RefuseMultiRowWithCopies(STMT* stmt, SQLINTEGER attribute, SQLULEN value, const char* function) {
    const MultiRowBindings bindings = MultiRowBindingsOf(attribute, value);
    if (bindings == MultiRowBindings::NONE) {
        return SQL_SUCCESS;
    }
    if (!HasOneRowCopies(bindings == MultiRowBindings::ROWS ? CurrentRowDesc(stmt) : CurrentParamDesc(stmt))) {
        return SQL_SUCCESS;
    }
    return RDS_OneRowCopiesError(stmt, function);
}

SQLRETURN RDS_OneRowCopiesError(STMT* stmt, const char* function) {
    ClearError(stmt);
    stmt->err = std::make_unique<ErrInfo>(OneRowCopiesMessage(function), ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
    return SQL_ERROR;
}
#endif

SQLRETURN RDS_SQLSetDescRec(
    SQLHDESC      DescriptorHandle,
    SQLSMALLINT   RecNumber,
    SQLSMALLINT   Type,
    SQLSMALLINT   SubType,
    SQLLEN        Length,
    SQLSMALLINT   Precision,
    SQLSMALLINT   Scale,
    SQLPOINTER    DataPtr,
    SQLLEN *      StringLengthPtr,
    SQLLEN *      IndicatorPtr)
{
    if (!HasEnvAccess<DESC>(DescriptorHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DESC* desc = static_cast<DESC*>(DescriptorHandle);
    const DBC* dbc = desc->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(desc->lock);
    ClearError(desc);

    if (!HasWrappedHandle(desc)) {
        return SQL_INVALID_HANDLE;
    }
#if UNICODE && !defined(_WIN32)
    if (RefusesWideDescRec(dbc, desc, RecNumber, Type)) {
        desc->err = std::make_unique<ErrInfo>(WideRecordMessage("SQLSetDescRec"), ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
        return SQL_ERROR;
    }
#endif
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetDescRec>(env->driver_lib_loader, RdsFuncNames::SetDescRec,
        desc->wrapped_desc, RecNumber, Type, SubType, Length, Precision, Scale, DataPtr, StringLengthPtr, IndicatorPtr
    );
    return RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
}

SQLRETURN RDS_SQLSetDescField(
    SQLHDESC       DescriptorHandle,
    SQLSMALLINT    RecNumber,
    SQLSMALLINT    FieldIdentifier,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     BufferLength)
{
    if (!HasEnvAccess<DESC>(DescriptorHandle)) {
        return SQL_INVALID_HANDLE;
    }
    DESC* desc = static_cast<DESC*>(DescriptorHandle);
    const DBC* dbc = desc->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(desc->lock);
    ClearError(desc);

    if (!HasWrappedHandle(desc)) {
        return SQL_INVALID_HANDLE;
    }
#if UNICODE && !defined(_WIN32)
    // A descriptor with one-row copies of SQL_C_WCHAR bindings, as for the statement attributes.
    if (IsMultiRowDescField(FieldIdentifier, ValuePtr) && HasOneRowCopies(desc)) {
        desc->err = std::make_unique<ErrInfo>(OneRowCopiesMessage("SQLSetDescField"), ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
        return SQL_ERROR;
    }
    if (RefusesWideDescField(dbc, desc, RecNumber, FieldIdentifier, ValuePtr)) {
        desc->err = std::make_unique<ErrInfo>(WideRecordMessage("SQLSetDescField"), ERR_OPTIONAL_FEATURE_NOT_IMPLEMENTED);
        return SQL_ERROR;
    }
#endif
#if UNICODE
    {
        const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
        if (odbc_helper->NeedsConversion() && ValuePtr
            && OdbcHelper::IsStringDescField(FieldIdentifier)
            && (BufferLength == SQL_NTS || BufferLength > 0)) {
            // BufferLength counts bytes; ConvertInput takes characters of the app's SQLWCHAR.
            const SQLINTEGER value_chars = BufferLength > 0
                ? BufferLength / static_cast<SQLINTEGER>(odbc_helper->GetUse4BytesUserApp() ? sizeof(SQLTCHAR) * 2 : sizeof(SQLTCHAR))
                : BufferLength;
            const auto value_converted = odbc_helper->ConvertInput(static_cast<SQLTCHAR*>(ValuePtr), value_chars);
            const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetDescField>(env->driver_lib_loader, RdsFuncNames::SetDescField,
                desc->wrapped_desc, RecNumber, FieldIdentifier, value_converted.tchar_ptr, value_converted.BaseLength(BufferLength)
            );
            return RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
        }
    }
#endif
    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetDescField>(env->driver_lib_loader, RdsFuncNames::SetDescField,
        desc->wrapped_desc, RecNumber, FieldIdentifier, ValuePtr, BufferLength
    );
    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_DESC, desc, res);
#if UNICODE && !defined(_WIN32)
    // A lower count unbinds the records above it in the base driver (psqlODBC resets them), so their copies go: kept,
    // each fetch would write a stale value into the app's old buffer, and each execute would bind the parameter again.
    if (FieldIdentifier == SQL_DESC_COUNT && SQL_SUCCEEDED(ret)) {
        DropRecordCopiesAbove(desc, static_cast<SQLSMALLINT>(reinterpret_cast<SQLLEN>(ValuePtr)));
    }
#endif
    return ret;
}

namespace {
// SQLSetStmtAttr(SQL_ATTR_APP_ROW_DESC / SQL_ATTR_APP_PARAM_DESC). The app's handle is a wrapper DESC (a driver manager
// passes the driver's handle): the base driver gets its own. NULL or the statement's own implicit descriptor puts the
// implicit one back.
SQLRETURN SetAppDesc(STMT* stmt, SQLINTEGER attribute, DESC* desc) {
    const DESC* implicit_desc = attribute == SQL_ATTR_APP_ROW_DESC ? stmt->app_row_desc : stmt->app_param_desc;
    if (desc == implicit_desc) {
        desc = nullptr;
    }
    if (desc && desc->stmt) {
        stmt->err = std::make_unique<ErrInfo>("SQLSetStmtAttr - Invalid use of an automatically allocated descriptor handle",
            ERR_INVALID_USE_OF_AN_AUTOMATICALLY_ALLOCATED_DESCRIPTOR_HANDLE);
        return SQL_ERROR;
    }
    if (desc && desc->dbc != stmt->dbc) {
        stmt->err = std::make_unique<ErrInfo>("SQLSetStmtAttr - The descriptor isn't on the statement's connection",
            ERR_INVALID_ATTRIBUTE_VALUE);
        return SQL_ERROR;
    }
    DESC*& explicit_desc = attribute == SQL_ATTR_APP_ROW_DESC ? stmt->explicit_row_desc : stmt->explicit_param_desc;
    if (!stmt->wrapped_stmt) {
        explicit_desc = desc;
        return SQL_SUCCESS;
    }
    const RdsLibResult res = OdbcHelper::SetWrappedAppDesc(stmt->dbc->env->driver_lib_loader, stmt, attribute, desc);
    if (desc && !desc->wrapped_desc) {
        stmt->err = std::make_unique<ErrInfo>("SQLSetStmtAttr - Unable to allocate the descriptor's underlying handle",
            ERR_UNDERLYING_HANDLE_NULL);
        return SQL_ERROR;
    }
    const SQLRETURN ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
    if (SQL_SUCCEEDED(ret)) {
        explicit_desc = desc;
    }
    return ret;
}
}  // namespace

SQLRETURN RDS_SQLSetStmtAttr(
    SQLHSTMT       StatementHandle,
    SQLINTEGER     Attribute,
    SQLPOINTER     ValuePtr,
    SQLINTEGER     StringLength)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;
    SQLRETURN ret = SQL_SUCCESS;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    switch (Attribute) {
        // Read-only attributes
        case SQL_ATTR_IMP_PARAM_DESC:
        case SQL_ATTR_IMP_ROW_DESC:
        case SQL_ATTR_ROW_NUMBER:
            stmt->err = std::make_unique<ErrInfo>("Attribute is read-only for Statement Handles", ERR_INVALID_ATTRIBUTE_VALUE);
            return SQL_ERROR;
        case SQL_ATTR_APP_ROW_DESC:
        case SQL_ATTR_APP_PARAM_DESC:
            return SetAppDesc(stmt, Attribute, static_cast<DESC*>(ValuePtr));
        default:
            break;
    }
#if UNICODE && !defined(_WIN32)
    if (RDS_RefuseMultiRowWithCopies(stmt, Attribute, reinterpret_cast<SQLULEN>(ValuePtr), "SQLSetStmtAttr") != SQL_SUCCESS) {
        return SQL_ERROR;
    }
#endif

    // On Aurora DSQL a non-zero query timeout gets 01S02 and doesn't reach psqlODBC, which wouldn't apply it
    // (IgnoreTimeout=1): Aurora DSQL rejects SET statement_timeout.
    if (const std::optional<SQLRETURN> dsql_ret = DsqlCompatibility::TrySetStmtAttr(stmt, Attribute, ValuePtr)) {
        return *dsql_ret;
    }

    if (stmt->wrapped_stmt) {
        const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetStmtAttr>(env->driver_lib_loader, RdsFuncNames::SetStmtAttr,
            stmt->wrapped_stmt, Attribute, ValuePtr, StringLength
        );
        ret = RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
    }
    stmt->attr_map.insert_or_assign(Attribute, std::make_pair(ValuePtr, StringLength));

    return ret;
}

SQLRETURN RDS_SQLSpecialColumns(
    SQLHSTMT       StatementHandle,
    SQLUSMALLINT   IdentifierType,
    SQLTCHAR *     CatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     SchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     TableName,
    SQLSMALLINT    NameLength3,
    SQLUSMALLINT   Scope,
    SQLUSMALLINT   Nullable)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto catalog_converted = odbc_helper->ConvertInput(CatalogName, NameLength1);
    const auto schema_converted  = odbc_helper->ConvertInput(SchemaName,  NameLength2);
    const auto table_converted   = odbc_helper->ConvertInput(TableName,   NameLength3);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSpecialColumns>(env->driver_lib_loader, RdsFuncNames::SpecialColumns,
        stmt->wrapped_stmt,
            IdentifierType,
            catalog_converted.tchar_ptr,
            catalog_converted.BaseLength(NameLength1),
            schema_converted.tchar_ptr,
            schema_converted.BaseLength(NameLength2),
            table_converted.tchar_ptr,
            table_converted.BaseLength(NameLength3),
            Scope,
            Nullable
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLStatistics(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     SchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     TableName,
    SQLSMALLINT    NameLength3,
    SQLUSMALLINT   Unique,
    SQLUSMALLINT   Reserved)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto catalog_converted = odbc_helper->ConvertInput(CatalogName, NameLength1);
    const auto schema_converted  = odbc_helper->ConvertInput(SchemaName,  NameLength2);
    const auto table_converted   = odbc_helper->ConvertInput(TableName,   NameLength3);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLStatistics>(env->driver_lib_loader, RdsFuncNames::Statistics,
        stmt->wrapped_stmt,
            catalog_converted.tchar_ptr,
            catalog_converted.BaseLength(NameLength1),
            schema_converted.tchar_ptr,
            schema_converted.BaseLength(NameLength2),
            table_converted.tchar_ptr,
            table_converted.BaseLength(NameLength3),
            Unique,
            Reserved
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLTablePrivileges(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     SchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     TableName,
    SQLSMALLINT    NameLength3)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto catalog_converted = odbc_helper->ConvertInput(CatalogName, NameLength1);
    const auto schema_converted  = odbc_helper->ConvertInput(SchemaName,  NameLength2);
    const auto table_converted   = odbc_helper->ConvertInput(TableName,   NameLength3);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLTablePrivileges>(env->driver_lib_loader, RdsFuncNames::TablePrivileges,
        stmt->wrapped_stmt,
            catalog_converted.tchar_ptr,
            catalog_converted.BaseLength(NameLength1),
            schema_converted.tchar_ptr,
            schema_converted.BaseLength(NameLength2),
            table_converted.tchar_ptr,
            table_converted.BaseLength(NameLength3)
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_SQLTables(
    SQLHSTMT       StatementHandle,
    SQLTCHAR *     CatalogName,
    SQLSMALLINT    NameLength1,
    SQLTCHAR *     SchemaName,
    SQLSMALLINT    NameLength2,
    SQLTCHAR *     TableName,
    SQLSMALLINT    NameLength3,
    SQLTCHAR *     TableType,
    SQLSMALLINT    NameLength4)
{
    if (!HasEnvAccess<STMT>(StatementHandle)) {
        return SQL_INVALID_HANDLE;
    }
    STMT* stmt = static_cast<STMT*>(StatementHandle);
    const DBC* dbc = stmt->dbc;
    const ENV* env = dbc->env;

    const std::lock_guard<std::recursive_mutex> lock_guard(stmt->lock);
    ClearError(stmt);

    if (!HasWrappedHandle(stmt)) {
        return SQL_INVALID_HANDLE;
    }

    const auto odbc_helper = dbc->plugin_service->GetOdbcHelper();
    const auto catalog_converted  = odbc_helper->ConvertInput(CatalogName, NameLength1);
    const auto schema_converted   = odbc_helper->ConvertInput(SchemaName,  NameLength2);
    const auto table_converted    = odbc_helper->ConvertInput(TableName,   NameLength3);
    const auto table_type_converted = odbc_helper->ConvertInput(TableType,  NameLength4);

    const RdsLibResult res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLTables>(env->driver_lib_loader, RdsFuncNames::Tables,
        stmt->wrapped_stmt,
            catalog_converted.tchar_ptr,
            catalog_converted.BaseLength(NameLength1),
            schema_converted.tchar_ptr,
            schema_converted.BaseLength(NameLength2),
            table_converted.tchar_ptr,
            table_converted.BaseLength(NameLength3),
            table_type_converted.tchar_ptr,
            table_type_converted.BaseLength(NameLength4)
    );
    return RDS_ProcessLibRes(SQL_HANDLE_STMT, stmt, res);
}

SQLRETURN RDS_InitializeConnection(DBC* dbc, const std::string& conn_str)
{
    ENV* env = dbc->env;

    // Remove input Driver
    // We don't want the underlying connection to look back to the wrapper
    // Also allows the Base DSN parse driver into map
    dbc->conn_attr.erase(KEY_DRIVER);

    // Set the DSN use the Base if one is found
    if (dbc->conn_attr.contains(KEY_BASE_DSN)) {
        dbc->conn_attr.insert_or_assign(KEY_DSN, dbc->conn_attr.at(KEY_BASE_DSN));
        // Load Base DSN info, should contain driver to use
        OdbcDsnHelper::LoadAll(dbc->conn_attr.at(KEY_BASE_DSN), dbc->conn_attr);
    }

    // If driver is not loaded from Base DSN, try input base driver
    if (!dbc->conn_attr.contains(KEY_DRIVER) && dbc->conn_attr.contains(KEY_BASE_DRIVER)) {
        dbc->conn_attr.insert_or_assign(KEY_DRIVER, dbc->conn_attr.at(KEY_BASE_DRIVER));
    }

    // An Aurora DSQL cluster ID in SERVER or IAM_HOST becomes the cluster endpoint before anything reads the host.
    if (!DsqlAuthPlugin::ExpandClusterId(dbc)) {
        return SQL_ERROR;
    }

    const std::unordered_set<std::string> invalid_params = AttributeValidator::ValidateMap(dbc->conn_attr);
    if (!invalid_params.empty()) {
        std::string invalid_message("Invalid value specified for connection string attribute:\n\t");
        for (const std::string& msg : invalid_params) {
            invalid_message += msg;
            invalid_message += "\n\t";
        }
        LOG(ERROR) << invalid_message;
        ClearError(dbc);
        dbc->err = std::make_unique<ErrInfo>(invalid_message, WARN_INVALID_CONNECTION_STRING_ATTRIBUTE);
        return SQL_ERROR;
    }

    // Settings that can't work with Aurora DSQL fail before the base driver is loaded or the plugin service exists,
    // so a rejected handle keeps no state.
    if (!DsqlConnectionValidator::CheckSettings(dbc)) {
        return SQL_ERROR;
    }

    RdsLibResult res;
    SQLRETURN ret = SQL_SUCCESS;

    {
        const std::lock_guard env_guard(env->lock);
        if (dbc->conn_attr.contains(KEY_DRIVER)) {
            // TODO - Need to ensure the paths (slashes) are correct per OS
            const std::string driver_value = dbc->conn_attr.at(KEY_DRIVER);

            std::string driver_path = driver_value;
            // If driver does not contain slashes which are used to indicate paths on OS
            // try to load driver from odbcinst / installed drivers list by name
            if (driver_value.find('/') == std::string::npos && driver_value.find('\\') == std::string::npos) {
                driver_path = OdbcDsnHelper::ResolveDriverName(driver_value);
                if (driver_path.empty()) {
                    const std::string load_err_message = "Cannot load underlying driver '" + driver_value
                        + "': no ODBC driver registered under this name (odbcinst.ini) and the value is not a path";
                    LOG(ERROR) << load_err_message;
                    ClearError(dbc);
                    dbc->err = std::make_unique<ErrInfo>(load_err_message, ERR_SPECIFIED_DRIVER_COULD_NOT_BE_LOADED);
                    return SQL_ERROR;
                }
            }

            // Load Module to Env if empty
            if (!env->driver_lib_loader) {
                const auto driver_lib_loader = std::make_shared<RdsLibLoader>(driver_path);
                if (!driver_lib_loader->IsLoaded()) {
                    std::string load_err_message = "Cannot load underlying driver '" + driver_value + "'";
                    if (driver_path != driver_value) {
                        load_err_message += " (resolved to '" + driver_path + "')";
                    }
                    load_err_message += ": " + driver_lib_loader->GetLoadError();
                    LOG(ERROR) << load_err_message;
                    ClearError(dbc);
                    dbc->err = std::make_unique<ErrInfo>(load_err_message, ERR_SPECIFIED_DRIVER_COULD_NOT_BE_LOADED);
                    return SQL_ERROR;
                }
                env->driver_lib_loader = driver_lib_loader;
            } else if (driver_path != env->driver_lib_loader->GetDriverPath()) {
                LOG(ERROR) << "Environment underlying driver differs from new connect. Create a new environment for different underlying drivers";
                ClearError(dbc);
                dbc->err = std::make_unique<ErrInfo>("Environment underlying driver differs from new connect. Create a new environment for different underlying drivers", ERR_DIFF_ENV_UNDERLYING_DRIVER);
                return SQL_ERROR;
            }
        } else {
            LOG(ERROR) << "No underlying driver found. Provide proper path to [BASE_DRIVER] or [DRIVER] within the [BASE_DSN]";
            ClearError(dbc);
            dbc->err = std::make_unique<ErrInfo>("No underlying driver found. Provide proper path to [BASE_DRIVER] or [DRIVER] within the [BASE_DSN]", ERR_NO_UNDER_LYING_DRIVER);
            return SQL_ERROR;
        }

        // Initialize Wrapped ENV
        // Create Wrapped HENV for Wrapped HDBC if not already allocated
        if (!env->wrapped_env) {
            res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLAllocHandle>(env->driver_lib_loader, RdsFuncNames::AllocHandle,
                SQL_HANDLE_ENV, nullptr, &env->wrapped_env
            );
            ret = RDS_ProcessLibRes(SQL_HANDLE_DBC, dbc, res);
            if (!SQL_SUCCEEDED(ret) || !env->wrapped_env) {
                LOG(ERROR) << "Unable to allocate underlying ENV";
                ClearError(dbc);
                dbc->err = std::make_unique<ErrInfo>("Unable to allocate underlying ENV", ERR_SQLALLOCHANDLE_ON_SQL_HANDLE_ENV_FAILED);
                return SQL_ERROR;
            }
            // Apply Tracked Environment Attributes
            for (auto const& [key, val] : env->attr_map) {
                res = RdsLibLoader::CallFunctionChecked<RDS_FP_SQLSetEnvAttr>(env->driver_lib_loader, RdsFuncNames::SetEnvAttr,
                    env->wrapped_env, key, val.first, val.second
                );
                ret = RDS_ProcessLibRes(SQL_HANDLE_ENV, env, res) == SQL_SUCCESS
                    ? ret : static_cast<SQLRETURN>(SQL_SUCCESS_WITH_INFO);
            }
        }
    }

    // Initialize Plugins
    try {
        if (!dbc->plugin_service) {
            // Create Plugin Service
            PluginService::InitClusterId(dbc->conn_attr);
            dbc->plugin_service = std::make_shared<PluginService>(env->driver_lib_loader, env, dbc->conn_attr,
                conn_str.empty()
                ? ConnectionStringHelper::BuildFullConnectionString(dbc->conn_attr)
                : conn_str
            );
        }
        // Plugin Builder
        if (!dbc->plugin_head) {
            std::shared_ptr<BasePlugin> plugin_head = std::make_shared<DefaultPlugin>(dbc);

            // Auth Plugins
            if (dbc->conn_attr.contains(KEY_AUTH_TYPE)) {
                const AuthType type = AuthProvider::AuthTypeFromString(dbc->conn_attr.at(KEY_AUTH_TYPE));
                switch (type) {
                        case AuthType::IAM:
                            plugin_head = std::make_shared<IamAuthPlugin>(dbc, plugin_head);
                            break;
                        case AuthType::SECRETS_MANAGER:
                            plugin_head = std::make_shared<SecretsManagerPlugin>(dbc, plugin_head);
                            break;
                        case AuthType::ADFS:
                            plugin_head = std::make_shared<AdfsAuthPlugin>(dbc, plugin_head);
                            break;
                        case AuthType::OKTA:
                            plugin_head = std::make_shared<OktaAuthPlugin>(dbc, plugin_head);
                            break;
                        case AuthType::AWS_SSO:
                            plugin_head = std::make_shared<AwsSsoAuthPlugin>(dbc, plugin_head);
                            break;
                        case AuthType::DSQL:
                            plugin_head = std::make_shared<DsqlAuthPlugin>(dbc, plugin_head);
                            break;
                        case AuthType::DATABASE:
                        case AuthType::INVALID:
                        default:
                            break;
                }
            }

            // Limitless
            if (dbc->conn_attr.contains(KEY_ENABLE_LIMITLESS)
                && dbc->conn_attr.at(KEY_ENABLE_LIMITLESS) == VALUE_BOOL_TRUE)
            {
                plugin_head = std::make_shared<LimitlessPlugin>(dbc, plugin_head);
            }

            // Failover
            if (dbc->conn_attr.contains(KEY_ENABLE_FAILOVER)
                && dbc->conn_attr.at(KEY_ENABLE_FAILOVER) == VALUE_BOOL_TRUE)
            {
                plugin_head = std::make_shared<FailoverPlugin>(dbc, plugin_head);
            }

            // Aurora Initial Connection Strategy
            if (dbc->conn_attr.contains(KEY_ENABLE_AURORA_INITIAL_CONNECTION_STRATEGY)
                && dbc->conn_attr.at(KEY_ENABLE_AURORA_INITIAL_CONNECTION_STRATEGY) == VALUE_BOOL_TRUE) {
                plugin_head = std::make_shared<AuroraInitialConnectionStrategyPlugin>(dbc, plugin_head);
            }

            // Read Write Splitting
            bool srw_enabled = false;
            if (dbc->conn_attr.contains(KEY_ENABLE_SRW_SPLIT)
                && dbc->conn_attr.at(KEY_ENABLE_SRW_SPLIT) == VALUE_BOOL_TRUE)
            {
                plugin_head = std::make_shared<SimpleReadWriteSplittingPlugin>(dbc, plugin_head);
                srw_enabled = true;
            }

            if (dbc->conn_attr.contains(KEY_ENABLE_RW_SPLIT)
                && dbc->conn_attr.at(KEY_ENABLE_RW_SPLIT) == VALUE_BOOL_TRUE)
            {
                if (srw_enabled) {
                    throw std::runtime_error("Only one of the Read Write Splitting and Simple Read Write Splitting plugins should be enabled at a time.");
                }
                plugin_head = std::make_shared<ReadWriteSplittingPlugin>(dbc, plugin_head);
            }

            // Custom Endpoint
            if (dbc->conn_attr.contains(KEY_ENABLE_CUSTOM_ENDPOINT)
                && dbc->conn_attr.at(KEY_ENABLE_CUSTOM_ENDPOINT) == VALUE_BOOL_TRUE)
            {
                plugin_head = std::make_shared<CustomEndpointPlugin>(dbc, plugin_head);
            }

            // Blue Green
            if (dbc->conn_attr.contains(KEY_ENABLE_BLUE_GREEN)
                && dbc->conn_attr.at(KEY_ENABLE_BLUE_GREEN) == VALUE_BOOL_TRUE)
            {
                plugin_head = std::make_shared<BlueGreenPlugin>(dbc, plugin_head);
            }

            // Aurora DSQL OCC retry. Inside the DSQL error hint plugin; a retry runs the chain below it again.
            if (dbc->conn_attr.contains(KEY_ENABLE_OCC_RETRY)
                && dbc->conn_attr.at(KEY_ENABLE_OCC_RETRY) == VALUE_BOOL_TRUE)
            {
                plugin_head = std::make_shared<DsqlOccRetryPlugin>(dbc, plugin_head);
            }

            // Aurora DSQL error hints. Outermost, after any OCC retry plugin, so they see only the result the app gets.
            plugin_head = DsqlErrorHintPlugin::AddToChain(dbc, plugin_head);

            // Finalize and track in DBC
            if (!plugin_head) {
                throw std::runtime_error("Plugin chain construction produced an empty chain.");
            }
            dbc->plugin_head = plugin_head.get();
            dbc->plugin_service->SetPluginChain(plugin_head);
        }

        dbc->plugin_service->InitHostListProvider();
    } catch (const std::exception& ex) {
        const std::string err_msg = std::string("Error initializing plugins: ") + ex.what();
        LOG(ERROR) << err_msg;
        ClearError(dbc);
        dbc->err = std::make_unique<ErrInfo>(err_msg, WARN_INVALID_CONNECTION_STRING_ATTRIBUTE);
        return SQL_ERROR;
    }

    return ret;
}
