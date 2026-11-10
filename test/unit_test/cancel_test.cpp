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

// SQLCancel from another thread while a statement runs on the statement handle, over a fake base driver, with real
// threads. Every cross-thread wait is bounded, so a regression fails instead of hanging.

#include "../../driver/util/windows_headers.h"

#include <sql.h>
#include <sqlext.h>

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../../driver/dialect/dialect.h"
#include "../../driver/driver.h"
#include "../../driver/host_info.h"
#include "../../driver/host_list_providers/topology_util.h"
#include "../../driver/host_selector/host_selector.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/base_plugin.h"
#include "../../driver/plugin/default_plugin.h"
#include "../../driver/plugin/failover/failover_plugin.h"
#include "../../driver/plugin/read_write_splitting/read_write_splitting_plugin.h"
#include "../../driver/util/base_handle_guard.h"
#include "../../driver/util/cancel_state.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/driver_module.h"
#include "../../driver/util/odbc_helper.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"

namespace {
    // The fakes' bound on a blocked call, and the most a test waits for a thread.
    constexpr std::chrono::seconds SAFETY_TIMEOUT(5);
    // What a cancel that doesn't wait for the running call takes, with room for a slow machine.
    constexpr std::chrono::seconds PROMPT(1);

    // The base driver. Its handles are ids it never reuses, so a call on a freed statement is seen (and nothing is
    // dereferenced). Each statement belongs to the connection it was allocated on, and a base disconnect makes them
    // unusable: psqlODBC before #190 freed them, and since then it detaches them, so that a call on one dereferences a
    // null connection.
    // The next blocked_execs executes wait until SQLCancel is called on their handle, ReleaseTheExec(), or
    // SAFETY_TIMEOUT. A canceled one returns SQL_ERROR, as psqlODBC does for a server-side cancel.
    // With block_cancel, SQLCancel stays inside until ReleaseTheCancel(), without ending the execute: a PQcancel that
    // can't connect. A statement freed or disconnected, or its connection freed, while a cancel is inside on it counts
    // as a dead-handle call. So does every call after the environment's free: psqlODBC frees every connection and
    // statement of an environment with it.
    struct FakeBaseDriver {
        std::mutex mutex;
        std::condition_variable cv;
        std::map<std::uintptr_t, std::uintptr_t> live;  // statement -> its connection
        std::map<std::uintptr_t, int> stmt_frees;       // statement -> SQLFreeHandle calls
        std::map<std::uintptr_t, int> disconnects;      // connection -> SQLDisconnect calls
        std::map<std::uintptr_t, int> dbc_frees;        // connection -> SQLFreeHandle calls
        std::map<std::uintptr_t, int> env_frees;        // environment -> SQLFreeHandle calls
        bool freed_env = false;
        std::vector<std::string> calls;                 // the frees, the disconnects and the cancels' ends, in order
        std::multiset<std::uintptr_t> cancel_connections;  // the connections of the statements a cancel is inside on
        bool module_pinned_at_cancel = false;           // DriverModule::IsPinned() as the last SQLCancel came in
        std::set<std::uintptr_t> lost;                  // statements whose execute lost the connection (08S01)
        std::uintptr_t next_handle = 0x1000;
        bool has_cancel = true;
        int failing_execs = 0;  // the next ones lose the connection (08S01)
        int exec_yields = 0;    // a non-blocked execute yields this often inside, so a cancel can land in it
        int blocked_execs = 0;
        SQLHSTMT executing = SQL_NULL_HSTMT;
        bool exec_canceled = false;
        bool exec_released = false;
        SQLRETURN released_exec_result = SQL_SUCCESS;
        bool block_cancel = false;
        bool cancel_released = false;
        int cancels_inside = 0;
        SQLRETURN cancel_result = SQL_SUCCESS;
        std::function<void()> on_driver_connect;
        int execs = 0;
        int cancels = 0;
        int cancels_during_exec = 0;
        int dead_handle_calls = 0;
        int driver_connects = 0;
    };
    std::unique_ptr<FakeBaseDriver> base_driver;

    // A non-blocking cancel yields this often between its two liveness checks, so a free that doesn't wait for it
    // lands in between.
    constexpr int CANCEL_YIELDS = 20;

    std::uintptr_t Id(SQLHANDLE handle) {
        return reinterpret_cast<std::uintptr_t>(handle);
    }

    // Call with base_driver->mutex held.
    bool IsLive(SQLHANDLE handle) {
        return base_driver->live.contains(Id(handle));
    }

    SQLRETURN FakeAllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE* output) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        const std::uintptr_t handle = base_driver->next_handle++;
        if (type == SQL_HANDLE_STMT) {
            base_driver->live[handle] = Id(input);
        }
        *output = reinterpret_cast<SQLHANDLE>(handle);
        return SQL_SUCCESS;
    }

    // Call with base_driver->mutex held.
    void LogCall(std::string call) {
        if (base_driver->freed_env) {
            ++base_driver->dead_handle_calls;
        }
        base_driver->calls.push_back(std::move(call));
    }

    std::string FreeHandleCall(SQLSMALLINT type) {
        switch (type) {
            case SQL_HANDLE_ENV:
                return "FreeHandle(ENV)";
            case SQL_HANDLE_DBC:
                return "FreeHandle(DBC)";
            case SQL_HANDLE_STMT:
                return "FreeHandle(STMT)";
            default:
                return "FreeHandle(DESC)";
        }
    }

    SQLRETURN FakeFreeHandle(SQLSMALLINT type, SQLHANDLE handle) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        LogCall(FreeHandleCall(type));
        if (type == SQL_HANDLE_STMT) {
            ++base_driver->stmt_frees[Id(handle)];
            if (base_driver->live.erase(Id(handle)) == 0) {
                ++base_driver->dead_handle_calls;
            }
        } else if (type == SQL_HANDLE_DBC) {
            ++base_driver->dbc_frees[Id(handle)];
            if (base_driver->cancel_connections.contains(Id(handle))) {
                ++base_driver->dead_handle_calls;
            }
        } else if (type == SQL_HANDLE_ENV) {
            ++base_driver->env_frees[Id(handle)];
            base_driver->freed_env = true;
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeDisconnect(SQLHDBC dbc) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        LogCall("Disconnect");
        ++base_driver->disconnects[Id(dbc)];
        std::erase_if(base_driver->live, [dbc](const auto& stmt) { return stmt.second == Id(dbc); });
        return SQL_SUCCESS;
    }

    SQLRETURN FakeDriverConnect(SQLHDBC /*dbc*/, SQLHWND /*window*/, SQLTCHAR* /*in*/, SQLSMALLINT /*in_length*/,
                                SQLTCHAR* /*out*/, SQLSMALLINT /*buffer_length*/, SQLSMALLINT* /*out_length*/,
                                SQLUSMALLINT /*completion*/) {
        std::function<void()> on_connect;
        {
            const std::lock_guard<std::mutex> lock(base_driver->mutex);
            ++base_driver->driver_connects;
            on_connect = base_driver->on_driver_connect;
        }
        if (on_connect) {
            on_connect();
        }
        return SQL_SUCCESS;
    }

    // The connection is never dead.
    SQLRETURN FakeGetConnectAttr(SQLHDBC /*dbc*/, SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER /*length*/,
                                 SQLINTEGER* /*out_length*/) {
        if (attribute == SQL_ATTR_CONNECTION_DEAD && value != nullptr) {
            *static_cast<SQLUINTEGER*>(value) = SQL_CD_FALSE;
        }
        return SQL_SUCCESS;
    }

    // 08S01 for a statement whose execute lost the connection.
    SQLRETURN FakeGetDiagRec(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT record, SQLTCHAR* state,
                             SQLINTEGER* native_error, SQLTCHAR* message, SQLSMALLINT buffer_length,
                             SQLSMALLINT* text_length) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        if (type != SQL_HANDLE_STMT || record != 1 || !base_driver->lost.contains(Id(handle))) {
            return SQL_NO_DATA;
        }
        const std::string lost_state = "08S01";
        if (state != nullptr) {
            std::copy(lost_state.begin(), lost_state.end(), state);
            state[lost_state.size()] = 0;
        }
        if (native_error != nullptr) {
            *native_error = 0;
        }
        if (message != nullptr && buffer_length > 0) {
            message[0] = 0;
        }
        if (text_length != nullptr) {
            *text_length = 0;
        }
        return SQL_SUCCESS;
    }

    SQLRETURN RunStatement(SQLHSTMT stmt) {
        std::unique_lock<std::mutex> lock(base_driver->mutex);
        if (!IsLive(stmt)) {
            ++base_driver->dead_handle_calls;
            return SQL_INVALID_HANDLE;
        }
        ++base_driver->execs;
        if (base_driver->blocked_execs == 0) {
            const bool fail = base_driver->failing_execs > 0;
            if (fail) {
                --base_driver->failing_execs;
                base_driver->lost.insert(Id(stmt));
            }
            if (base_driver->exec_yields > 0) {
                base_driver->executing = stmt;
                const int yields = base_driver->exec_yields;
                lock.unlock();
                for (int i = 0; i < yields; ++i) {
                    std::this_thread::yield();
                }
                lock.lock();
                base_driver->executing = SQL_NULL_HSTMT;
            }
            return fail ? SQL_ERROR : SQL_SUCCESS;
        }
        --base_driver->blocked_execs;
        base_driver->executing = stmt;
        base_driver->exec_canceled = false;
        base_driver->exec_released = false;
        base_driver->cv.notify_all();
        base_driver->cv.wait_for(lock, SAFETY_TIMEOUT,
            [] { return base_driver->exec_canceled || base_driver->exec_released; });
        base_driver->executing = SQL_NULL_HSTMT;
        if (base_driver->exec_canceled) {
            return SQL_ERROR;
        }
        if (base_driver->exec_released && base_driver->released_exec_result == SQL_ERROR) {
            base_driver->lost.insert(Id(stmt));
            return SQL_ERROR;
        }
        return SQL_SUCCESS;
    }

    SQLRETURN FakeExecDirect(SQLHSTMT stmt, SQLTCHAR* /*text*/, SQLINTEGER /*length*/) {
        return RunStatement(stmt);
    }

    SQLRETURN FakeExecute(SQLHSTMT stmt) {
        return RunStatement(stmt);
    }

    SQLRETURN FakeCancel(SQLHSTMT stmt) {
        std::unique_lock<std::mutex> lock(base_driver->mutex);
        if (!IsLive(stmt)) {
            ++base_driver->dead_handle_calls;
            return SQL_INVALID_HANDLE;
        }
        base_driver->module_pinned_at_cancel = DriverModule::IsPinned();
        ++base_driver->cancels;
        ++base_driver->cancels_inside;
        const std::uintptr_t connection = base_driver->live.at(Id(stmt));
        base_driver->cancel_connections.insert(connection);
        if (base_driver->executing == stmt) {
            ++base_driver->cancels_during_exec;
            if (!base_driver->block_cancel) {
                base_driver->exec_canceled = true;
            }
        }
        base_driver->cv.notify_all();
        if (base_driver->block_cancel) {
            base_driver->cv.wait_for(lock, SAFETY_TIMEOUT, [] { return base_driver->cancel_released; });
        } else {
            lock.unlock();
            for (int i = 0; i < CANCEL_YIELDS; ++i) {
                std::this_thread::yield();
            }
            lock.lock();
        }
        if (!IsLive(stmt)) {
            ++base_driver->dead_handle_calls;
        }
        LogCall("Cancel");
        --base_driver->cancels_inside;
        base_driver->cancel_connections.erase(base_driver->cancel_connections.find(connection));
        base_driver->cv.notify_all();
        return base_driver->cancel_result;
    }

    // Waits until the base driver runs a statement, with a bound.
    bool WaitUntilTheBaseExecutes() {
        std::unique_lock<std::mutex> lock(base_driver->mutex);
        return base_driver->cv.wait_for(lock, SAFETY_TIMEOUT, [] { return base_driver->executing != SQL_NULL_HSTMT; });
    }

    // Waits until a base SQLCancel is inside the base driver, with a bound.
    bool WaitUntilTheCancelIsInside() {
        std::unique_lock<std::mutex> lock(base_driver->mutex);
        return base_driver->cv.wait_for(lock, SAFETY_TIMEOUT, [] { return base_driver->cancels_inside > 0; });
    }

    // Ends the blocked execute with result; SQL_ERROR means the connection was lost (08S01).
    void ReleaseTheExec(SQLRETURN result) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        base_driver->released_exec_result = result;
        base_driver->exec_released = true;
        base_driver->cv.notify_all();
    }

    void ReleaseTheCancel() {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        base_driver->cancel_released = true;
        base_driver->cv.notify_all();
    }

    bool IsLiveStatement(SQLHSTMT stmt) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        return IsLive(stmt);
    }

    int StatementFrees(SQLHSTMT stmt) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        return base_driver->stmt_frees[Id(stmt)];
    }

    int Disconnects(SQLHDBC dbc) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        return base_driver->disconnects[Id(dbc)];
    }

    int ConnectionFrees(SQLHDBC dbc) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        return base_driver->dbc_frees[Id(dbc)];
    }

    int EnvironmentFrees(SQLHENV env) {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        return base_driver->env_frees[Id(env)];
    }

    std::vector<std::string> Calls() {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        return base_driver->calls;
    }

    int DeadHandleCalls() {
        const std::lock_guard<std::mutex> lock(base_driver->mutex);
        return base_driver->dead_handle_calls;
    }

    // Calls check until it returns true, or false after SAFETY_TIMEOUT: for a state another thread reaches without
    // telling.
    template <typename Check>
    bool SpinUntil(Check check) {
        const auto deadline = std::chrono::steady_clock::now() + SAFETY_TIMEOUT;
        while (!check()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

    template <typename Function>
    RdsPlatform::FuncHandle AsFuncHandle(Function* function) {
        return reinterpret_cast<RdsPlatform::FuncHandle>(function);
    }

    class FakeBaseDriverLoader : public RdsLibLoader {
    public:
        RdsPlatform::FuncHandle GetFunction(const std::string& name) override {
            if (name == RdsFuncNames::AllocHandle) {
                return AsFuncHandle(&FakeAllocHandle);
            }
            if (name == RdsFuncNames::FreeHandle) {
                return AsFuncHandle(&FakeFreeHandle);
            }
            if (name == RdsFuncNames::Disconnect) {
                return AsFuncHandle(&FakeDisconnect);
            }
            if (name == RdsFuncNames::ExecDirect) {
                return AsFuncHandle(&FakeExecDirect);
            }
            if (name == RdsFuncNames::Execute) {
                return AsFuncHandle(&FakeExecute);
            }
            if (name == RdsFuncNames::DriverConnect) {
                return AsFuncHandle(&FakeDriverConnect);
            }
            if (name == RdsFuncNames::GetConnectAttr) {
                return AsFuncHandle(&FakeGetConnectAttr);
            }
            if (name == RdsFuncNames::GetDiagRec) {
                return AsFuncHandle(&FakeGetDiagRec);
            }
            if (name == RdsFuncNames::Cancel && base_driver->has_cancel) {
                return AsFuncHandle(&FakeCancel);
            }
            return nullptr;
        }
    };

    // Waits before the next plugin, as a retry backoff does: until the call is canceled or the delay runs out.
    class WaitingPlugin : public BasePlugin {
    public:
        WaitingPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin, std::chrono::milliseconds delay)
            : BasePlugin(dbc, std::move(next_plugin)), delay_(delay) {}

        SQLRETURN Execute(SQLHSTMT StatementHandle, SQLTCHAR* StatementText, SQLINTEGER TextLength) override {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                ++waits_;
            }
            cv_.notify_all();
            if (static_cast<STMT*>(StatementHandle)->cancel.WaitFor(delay_)) {
                ++wakes_;
            }
            return ExecuteNext(StatementHandle, StatementText, TextLength);
        }

        // Waits, with a bound, until the plugin's wait number `count` has started.
        bool WaitUntilWaiting(int count = 1) {
            std::unique_lock<std::mutex> lock(mutex_);
            return cv_.wait_for(lock, SAFETY_TIMEOUT, [this, count] { return waits_ >= count; });
        }

        // The waits a cancel cut short.
        int Wakes() const { return wakes_.load(); }

        // For the next call (none may run).
        void SetDelay(std::chrono::milliseconds delay) { delay_ = delay; }

    private:
        std::chrono::milliseconds delay_;
        std::mutex mutex_;
        std::condition_variable cv_;
        int waits_ = 0;
        std::atomic<int> wakes_ = 0;
    };

    // Network errors are 08xxx; a SET ... READ ONLY / READ WRITE statement switches the read/write splitting plugin.
    class TestDialect : public Dialect {
    public:
        bool IsSqlStateNetworkError(const char* sql_state) override {
            return sql_state != nullptr && std::string(sql_state).starts_with("08");
        }

        std::optional<bool> DoesStatementSetReadOnly(std::string statement) override {
            if (statement.find("READ ONLY") != std::string::npos) {
                return true;
            }
            if (statement.find("READ WRITE") != std::string::npos) {
                return false;
            }
            return {};
        }
    };

    // The first host of the role asked for.
    class TestHostSelector : public HostSelector {
    public:
        HostInfo GetHost(std::vector<HostInfo> hosts, bool is_writer,
                         std::unordered_map<std::string, std::string> /*properties*/) override {
            for (const HostInfo& host : hosts) {
                if (host.IsHostWriter() == is_writer) {
                    return host;
                }
            }
            throw std::runtime_error("no host of that role");
        }
    };

    class TestTopologyUtil : public TopologyUtil {
    public:
        std::vector<HostInfo> GetHosts(SQLHDBC /*hdbc*/, const HostInfo& /*initial_host*/,
                                       const HostInfo& /*host_template*/) override {
            return {};
        }
    };

    constexpr int TEST_PORT = 5432;
    const HostInfo WRITER_HOST{"writer-host", TEST_PORT, UP, WRITER};
    const HostInfo READER_HOST{"reader-host", TEST_PORT, UP, READER};

    // The plugin service of a connected handle: its OdbcHelper, and a cluster of a writer and a reader for the
    // failover and read/write splitting plugins. Every connection it describes skips the dialect check.
    class TestPluginService : public PluginService {
    public:
        explicit TestPluginService(std::shared_ptr<OdbcHelper> odbc_helper)
            : odbc_helper_(std::move(odbc_helper)) {}

        std::shared_ptr<OdbcHelper> GetOdbcHelper() override { return odbc_helper_; }
        std::shared_ptr<Dialect> GetDialect() override { return dialect_; }
        std::shared_ptr<HostSelector> GetHostSelector() override { return host_selector_; }
        std::shared_ptr<TopologyUtil> GetTopologyUtil() override { return topology_util_; }
        std::vector<HostInfo> GetHosts() override { return hosts_; }
        std::vector<HostInfo> GetFilteredHosts() override { return hosts_; }
        std::map<std::string, std::string> GetOriginalConnAttr() override {
            return {{KEY_MONITORING_CONN_UUID, "cancel-test"}};
        }
        void RefreshHosts() override {}
        void ForceRefreshHosts(bool /*verify_writer*/, std::chrono::milliseconds /*timeout_ms*/) override {}
        HostInfo GetCurrentHostInfo() override { return current_host_; }
        void SetCurrentHostInfo(const HostInfo& info) override { current_host_ = info; }

    private:
        std::shared_ptr<OdbcHelper> odbc_helper_;
        std::shared_ptr<Dialect> dialect_ = std::make_shared<TestDialect>();
        std::shared_ptr<HostSelector> host_selector_ = std::make_shared<TestHostSelector>();
        std::shared_ptr<TopologyUtil> topology_util_ = std::make_shared<TestTopologyUtil>();
        std::vector<HostInfo> hosts_{WRITER_HOST, READER_HOST};
        HostInfo current_host_ = WRITER_HOST;
    };

    // Counts the calls that reach the plugin chain.
    class CountingPlugin : public BasePlugin {
    public:
        CountingPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin) : BasePlugin(dbc, std::move(next_plugin)) {}

        SQLRETURN Execute(SQLHSTMT StatementHandle, SQLTCHAR* StatementText, SQLINTEGER TextLength) override {
            ++calls_;
            return ExecuteNext(StatementHandle, StatementText, TextLength);
        }

        int Calls() const { return calls_.load(); }

    private:
        std::atomic<int> calls_ = 0;
    };

    // The read/write splitting plugin, with its connection as the one the statement uses.
    class TestReadWriteSplittingPlugin : public ReadWriteSplittingPlugin {
    public:
        using ReadWriteSplittingPlugin::ReadWriteSplittingPlugin;
        void SetCurrentConnection(SQLHDBC conn) { this->current_connection_ = conn; }
    };

    // A statement text in the app's characters.
    std::vector<SQLTCHAR> AppText(const std::string& text) {
        std::vector<SQLTCHAR> app_text(text.begin(), text.end());
        app_text.push_back(0);
        return app_text;
    }
}  // namespace

// The cancel flag exists only while a call runs, and a cancel wakes the call's wait. A call runs from its Enter().
TEST(CancelStateTest, RequestWithoutACallDoesNothing) {
    CancelState state;
    EXPECT_EQ(CancelState::Target::NO_CALL, state.Request());
    EXPECT_FALSE(state.IsCanceled());
    CancelState::Scope call(state);
    EXPECT_FALSE(call.Enter());
    EXPECT_FALSE(state.IsCanceled());
}

TEST(CancelStateTest, RequestCancelsOnlyTheRunningCall) {
    CancelState state;
    {
        CancelState::Scope call(state);
        call.Enter();
        EXPECT_EQ(CancelState::Target::WRAPPER, state.Request());
        EXPECT_TRUE(state.IsCanceled());
    }
    EXPECT_FALSE(state.IsCanceled());
    CancelState::Scope next_call(state);
    EXPECT_FALSE(next_call.Enter());
    EXPECT_FALSE(state.IsCanceled());
}

// Two threads can share a statement: their calls wait for each other. A cancel stops the call running when SQLCancel
// was called, not one waiting behind it or one issued after SQLCancel returned.
TEST(CancelStateTest, RequestDoesNotReachACallWaitingBehindTheRunningOne) {
    CancelState state;
    std::optional<CancelState::Scope> running(std::in_place, state);
    running->Enter();
    std::optional<CancelState::Scope> waiting(std::in_place, state);
    EXPECT_EQ(CancelState::Target::WRAPPER, state.Request());
    running.reset();
    EXPECT_FALSE(waiting->Enter());
    EXPECT_FALSE(state.IsCanceled());
}

TEST(CancelStateTest, RequestDoesNotReachACallIssuedAfterIt) {
    CancelState state;
    std::optional<CancelState::Scope> running(std::in_place, state);
    running->Enter();
    EXPECT_EQ(CancelState::Target::WRAPPER, state.Request());
    std::optional<CancelState::Scope> later(std::in_place, state);
    running.reset();
    EXPECT_FALSE(later->Enter());
    EXPECT_FALSE(state.IsCanceled());
}

// With no call running, the cancel is for the call waiting to run (another statement's call holds the connection),
// even if a call issued after SQLCancel gets the statement first.
TEST(CancelStateTest, RequestWithOnlyAWaitingCallCancelsItWhenItRuns) {
    CancelState state;
    std::optional<CancelState::Scope> waiting(std::in_place, state);
    EXPECT_EQ(CancelState::Target::WRAPPER, state.Request());
    std::optional<CancelState::Scope> later(std::in_place, state);
    EXPECT_FALSE(later->Enter());
    later.reset();
    EXPECT_TRUE(waiting->Enter());
    EXPECT_TRUE(state.IsCanceled());
    waiting.reset();
    CancelState::Scope next_call(state);
    EXPECT_FALSE(next_call.Enter());
}

// Only a call inside the base driver's execute is the base driver's to cancel, and a canceled call doesn't enter it.
TEST(CancelStateTest, RequestSaysWhetherTheCallIsInsideTheBaseDriver) {
    CancelState state;
    CancelState::Scope call(state);
    call.Enter();
    ASSERT_TRUE(state.EnterBaseDriver());
    EXPECT_EQ(CancelState::Target::BASE_DRIVER, state.Request());
    state.LeaveBaseDriver();
    EXPECT_EQ(CancelState::Target::WRAPPER, state.Request());
    EXPECT_FALSE(state.EnterBaseDriver());
}

// Past MarkUncancelable(), the call runs: a cancel, also one that came before, doesn't stop it.
TEST(CancelStateTest, MarkUncancelableLetsTheCallRun) {
    CancelState state;
    CancelState::Scope call(state);
    call.Enter();
    EXPECT_EQ(CancelState::Target::WRAPPER, state.Request());
    state.MarkUncancelable();
    EXPECT_FALSE(state.IsCanceled());
    EXPECT_EQ(CancelState::Target::UNCANCELABLE, state.Request());
    EXPECT_TRUE(state.EnterBaseDriver());
    EXPECT_EQ(CancelState::Target::UNCANCELABLE, state.Request());
}

TEST(CancelStateTest, WaitForRunsOutWithoutACancel) {
    CancelState state;
    CancelState::Scope call(state);
    call.Enter();
    EXPECT_FALSE(state.WaitFor(std::chrono::milliseconds(10)));
}

TEST(CancelStateTest, WaitForReturnsAtOnceWhenTheCallIsCanceled) {
    CancelState state;
    CancelState::Scope call(state);
    call.Enter();
    EXPECT_EQ(CancelState::Target::WRAPPER, state.Request());
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(state.WaitFor(SAFETY_TIMEOUT));
    EXPECT_LT(std::chrono::steady_clock::now() - start, PROMPT);
}

TEST(CancelStateTest, WaitForWakesOnACancelFromAnotherThread) {
    CancelState state;
    CancelState::Scope call(state);
    call.Enter();
    std::future<CancelState::Target> cancel = std::async(std::launch::async, [&state] { return state.Request(); });
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(state.WaitFor(SAFETY_TIMEOUT));
    EXPECT_LT(std::chrono::steady_clock::now() - start, PROMPT);
    EXPECT_EQ(CancelState::Target::WRAPPER, cancel.get());
}

// A teardown runs at once with no base driver call pinned, else after the last one. Teardowns run one at a time in
// the order they came: one that comes while handed-over ones run is handed over too, so a statement's free never runs
// after, or alongside, its connection's later disconnect.
TEST(BaseHandleGuardTest, TeardownsRunInOrderAfterTheLastPin) {
    BaseHandleGuard guard;
    int base_stmt = 0;
    WrappedStmt wrapped_stmt;
    guard.Store(wrapped_stmt, &base_stmt);
    std::vector<std::string> order;
    guard.RunOrDefer([&order] { order.emplace_back("unpinned"); });
    EXPECT_EQ(std::vector<std::string>{"unpinned"}, order);
    order.clear();
    {
        const BaseHandleGuard::PinnedStatement pinned(guard, wrapped_stmt);
        ASSERT_EQ(wrapped_stmt.Get(), pinned.Handle());
        guard.RunOrDefer([&guard, &order] {
            order.emplace_back("free");
            guard.RunOrDefer([&order] { order.emplace_back("disconnect"); });
            order.emplace_back("free done");
        });
        EXPECT_TRUE(order.empty());
    }
    EXPECT_EQ((std::vector<std::string>{"free", "free done", "disconnect"}), order);
}

TEST(BaseHandleGuardTest, NoBaseHandleIsNotPinned) {
    BaseHandleGuard guard;
    const WrappedStmt wrapped_stmt{};
    bool ran = false;
    {
        const BaseHandleGuard::PinnedStatement pinned(guard, wrapped_stmt);
        EXPECT_EQ(nullptr, pinned.Handle());
        guard.RunOrDefer([&ran] { ran = true; });
        EXPECT_TRUE(ran);
    }
}

// What the guard keeps alive outlives the guard while a call is pinned, and goes with the last pin, after the teardowns
// handed to it: as the base environment of a connection the app freed during an SQLCancel.
TEST(BaseHandleGuardTest, KeepAliveOutlivesTheGuardUntilTheLastPin) {
    auto guard = std::make_unique<BaseHandleGuard>();
    int base_stmt = 0;
    WrappedStmt wrapped_stmt;
    guard->Store(wrapped_stmt, &base_stmt);
    auto kept = std::make_shared<int>(0);
    const std::weak_ptr<int> weak = kept;
    guard->KeepAlive(std::move(kept));
    bool alive_in_the_teardown = false;
    {
        const BaseHandleGuard::PinnedStatement pinned(*guard, wrapped_stmt);
        guard->RunOrDefer([&weak, &alive_in_the_teardown] { alive_in_the_teardown = !weak.expired(); });
        guard = nullptr;
        EXPECT_FALSE(weak.expired());
    }
    EXPECT_TRUE(alive_in_the_teardown);
    EXPECT_TRUE(weak.expired());
}

// A connected statement over the fake base driver, with only the default plugin.
class CancelTest : public testing::Test {
protected:
    ENV env_;
    DBC dbc_{};
    STMT* stmt_ = nullptr;
    int base_env_ = 0;  // the base handles; only their addresses are used
    int base_dbc_ = 0;
    std::shared_ptr<OdbcHelper> odbc_helper_;
    std::shared_ptr<BasePlugin> plugin_chain_;
    std::vector<std::shared_ptr<BasePlugin>> chains_;  // those of the connections NewConnectedStatement makes
    std::vector<SQLTCHAR> text_ = AppText("SELECT 1");

    void SetUp() override {
        base_driver = std::make_unique<FakeBaseDriver>();
        env_.driver_lib_loader = std::make_shared<FakeBaseDriverLoader>();
        env_.wrapped_env = &base_env_;
        odbc_helper_ = std::make_shared<OdbcHelper>(env_.driver_lib_loader, &env_);
        dbc_.env = &env_;
        dbc_.wrapped_dbc = &base_dbc_;
        dbc_.conn_status = CONN_CONNECTED;
        dbc_.plugin_service = std::make_shared<TestPluginService>(odbc_helper_);
        UsePluginChain(std::make_shared<DefaultPlugin>(&dbc_));
        SQLHSTMT handle = SQL_NULL_HSTMT;
        ASSERT_EQ(SQL_SUCCESS, RDS_AllocStmt(&dbc_, &handle));
        stmt_ = static_cast<STMT*>(handle);
        ASSERT_NE(nullptr, stmt_->wrapped_stmt);
    }

    void TearDown() override {
        if (stmt_ != nullptr) {
            RDS_FreeStmt(stmt_, SQL_DROP);
        }
        dbc_.plugin_head = nullptr;
        plugin_chain_ = nullptr;
        chains_.clear();
        dbc_.plugin_service = nullptr;
        base_driver = nullptr;
    }

    void UsePluginChain(std::shared_ptr<BasePlugin> plugin_chain) {
        plugin_chain_ = std::move(plugin_chain);
        dbc_.plugin_head = plugin_chain_.get();
    }

    // An app's environment over the base environment base_env_, which SQLFreeHandle frees (the fixture's env_ stays).
    // Not RDS_AllocEnv, which starts the driver's file logging.
    ENV* NewEnvironment() {
        auto* env = new ENV();
        env->driver_lib_loader = env_.driver_lib_loader;
        env->wrapped_env = &base_env_;
        return env;
    }

    // A statement on a new app connection of env, connected to base_dbc, with only the default plugin.
    STMT* NewConnectedStatement(ENV* env, int& base_dbc) {
        SQLHDBC handle = SQL_NULL_HDBC;
        EXPECT_EQ(SQL_SUCCESS, RDS_AllocDbc(env, &handle));
        auto* dbc = static_cast<DBC*>(handle);
        dbc->wrapped_dbc = &base_dbc;
        dbc->conn_status = CONN_CONNECTED;
        dbc->plugin_service = std::make_shared<TestPluginService>(odbc_helper_);
        chains_.push_back(std::make_shared<DefaultPlugin>(dbc));
        dbc->plugin_head = chains_.back().get();
        return NewStatement(dbc);
    }

    // Another statement on dbc, with its base statement.
    static STMT* NewStatement(DBC* dbc) {
        SQLHSTMT handle = SQL_NULL_HSTMT;
        EXPECT_EQ(SQL_SUCCESS, RDS_AllocStmt(dbc, &handle));
        auto* stmt = static_cast<STMT*>(handle);
        EXPECT_NE(nullptr, stmt->wrapped_stmt);
        return stmt;
    }

    // An SQLCancel on stmt from another thread, which stays in the base driver until ReleaseTheCancel().
    static std::future<SQLRETURN> BlockedCancel(STMT* stmt) {
        base_driver->block_cancel = true;
        return std::async(std::launch::async, [stmt] { return SQLCancel(stmt); });
    }

    // A WaitingPlugin in front of the default plugin.
    std::shared_ptr<WaitingPlugin> WaitBeforeTheBaseDriver(std::chrono::milliseconds delay) {
        auto waiting = std::make_shared<WaitingPlugin>(&dbc_, std::make_shared<DefaultPlugin>(&dbc_), delay);
        UsePluginChain(waiting);
        return waiting;
    }

    std::future<SQLRETURN> ExecDirectOnAnotherThread() {
        return std::async(std::launch::async, [this] { return RDS_SQLExecDirect(stmt_, text_.data(), SQL_NTS); });
    }

    std::future<SQLRETURN> ExecuteOnAnotherThread() {
        return std::async(std::launch::async, [this] { return SQLExecute(stmt_); });
    }

    std::future<SQLRETURN> CancelOnAnotherThread() {
        return std::async(std::launch::async, [this] { return SQLCancel(stmt_); });
    }
};

// The cancel no longer waits for the running call: the base driver gets it while its execute runs, and the call
// returns the base driver's answer. The wrapper adds no record.
TEST_F(CancelTest, Cancel_ReachesTheBaseWhileExecDirectRuns) {
    base_driver->blocked_execs = 1;
    std::future<SQLRETURN> exec = ExecDirectOnAnotherThread();
    ASSERT_TRUE(WaitUntilTheBaseExecutes());

    std::future<SQLRETURN> cancel = CancelOnAnotherThread();
    ASSERT_EQ(std::future_status::ready, cancel.wait_for(PROMPT));
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ(SQL_ERROR, exec.get());
    EXPECT_EQ(1, base_driver->cancels_during_exec);
    EXPECT_EQ(nullptr, stmt_->err);
}

TEST_F(CancelTest, Cancel_ReachesTheBaseWhileExecuteRuns) {
    base_driver->blocked_execs = 1;
    std::future<SQLRETURN> exec = ExecuteOnAnotherThread();
    ASSERT_TRUE(WaitUntilTheBaseExecutes());

    std::future<SQLRETURN> cancel = CancelOnAnotherThread();
    ASSERT_EQ(std::future_status::ready, cancel.wait_for(PROMPT));
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ(SQL_ERROR, exec.get());
    EXPECT_EQ(1, base_driver->cancels_during_exec);
}

TEST_F(CancelTest, CancelHandle_ReachesTheBaseWhileExecDirectRuns) {
    base_driver->blocked_execs = 1;
    std::future<SQLRETURN> exec = ExecDirectOnAnotherThread();
    ASSERT_TRUE(WaitUntilTheBaseExecutes());

    std::future<SQLRETURN> cancel = std::async(std::launch::async,
        [this] { return SQLCancelHandle(SQL_HANDLE_STMT, stmt_); });
    ASSERT_EQ(std::future_status::ready, cancel.wait_for(PROMPT));
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ(SQL_ERROR, exec.get());
    EXPECT_EQ(1, base_driver->cancels_during_exec);
}

// A base driver without SQLCancel: the record goes on the statement only when no other thread is inside a call on it,
// since that call owns the statement's diagnostics, and the cancel doesn't wait for it.
TEST_F(CancelTest, Cancel_BaseWithoutSqlCancelPostsARecordOnlyWhenIdle) {
    base_driver->has_cancel = false;
    EXPECT_EQ(SQL_ERROR, SQLCancel(stmt_));
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("LD002", stmt_->err->sqlstate);

    // Another thread inside a call on the statement: it holds the statement's lock and has its own record.
    std::promise<void> held;
    std::future<void> is_held = held.get_future();
    std::promise<void> release;
    std::future<void> released = release.get_future();
    std::future<void> holder = std::async(std::launch::async, [this, &held, &released] {
        const std::lock_guard<std::recursive_mutex> lock(stmt_->lock);
        ClearError(stmt_);
        stmt_->err = std::make_unique<ErrInfo>("the running call's record", ERR_GENERAL_ERROR);
        held.set_value();
        released.wait();
    });
    is_held.wait();
    std::future<SQLRETURN> cancel = CancelOnAnotherThread();
    const bool prompt = cancel.wait_for(PROMPT) == std::future_status::ready;
    release.set_value();
    holder.get();

    EXPECT_TRUE(prompt);
    EXPECT_EQ(SQL_ERROR, cancel.get());
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("the running call's record", stmt_->err->error_msg);
}

// After failover or a read/write splitting switch, a valid statement has no base handle until its next execute. A
// cancel with nothing running has no effect, as ODBC 3 says; SQL_INVALID_HANDLE would call the handle unusable.
TEST_F(CancelTest, Cancel_IdleWithoutABaseHandleReturnsSuccess) {
    FakeFreeHandle(SQL_HANDLE_STMT, stmt_->wrapped_stmt);
    OdbcHelper::SetWrappedStmt(stmt_, SQL_NULL_HSTMT);
    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    EXPECT_EQ(SQL_SUCCESS, SQLCancelHandle(SQL_HANDLE_STMT, stmt_));
    EXPECT_EQ(nullptr, stmt_->err);
    EXPECT_EQ(0, base_driver->cancels);
}

TEST_F(CancelTest, CancelHandle_DbcAndDescAreStillNotImplemented) {
    EXPECT_EQ(SQL_ERROR, SQLCancelHandle(SQL_HANDLE_DBC, &dbc_));
    ASSERT_NE(nullptr, dbc_.err);
    EXPECT_EQ("HYC00", dbc_.err->sqlstate);
    EXPECT_EQ(SQL_ERROR, SQLCancelHandle(SQL_HANDLE_DESC, stmt_->app_row_desc));
    ASSERT_NE(nullptr, stmt_->app_row_desc->err);
    EXPECT_EQ("HYC00", stmt_->app_row_desc->err->sqlstate);
}

// SQLExecDirect and SQLExecute are cancelable while they run, and only then.
TEST_F(CancelTest, ExecDirect_IsCancelableOnlyWhileItRuns) {
    EXPECT_EQ(CancelState::Target::NO_CALL, stmt_->cancel.Request());
    base_driver->blocked_execs = 1;
    std::future<SQLRETURN> exec = ExecDirectOnAnotherThread();
    ASSERT_TRUE(WaitUntilTheBaseExecutes());
    const CancelState::Target target = stmt_->cancel.Request();
    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    EXPECT_EQ(SQL_ERROR, exec.get());

    EXPECT_EQ(CancelState::Target::BASE_DRIVER, target);
    EXPECT_FALSE(stmt_->cancel.IsCanceled());
    EXPECT_EQ(CancelState::Target::NO_CALL, stmt_->cancel.Request());
}

TEST_F(CancelTest, Execute_IsCancelableOnlyWhileItRuns) {
    EXPECT_EQ(CancelState::Target::NO_CALL, stmt_->cancel.Request());
    base_driver->blocked_execs = 1;
    std::future<SQLRETURN> exec = ExecuteOnAnotherThread();
    ASSERT_TRUE(WaitUntilTheBaseExecutes());
    const CancelState::Target target = stmt_->cancel.Request();
    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    EXPECT_EQ(SQL_ERROR, exec.get());

    EXPECT_EQ(CancelState::Target::BASE_DRIVER, target);
    EXPECT_FALSE(stmt_->cancel.IsCanceled());
    EXPECT_EQ(CancelState::Target::NO_CALL, stmt_->cancel.Request());
}

// Also while the call waits for the connection, which another statement's call can hold for a long time.
TEST_F(CancelTest, ExecDirect_IsCancelableWhileItWaitsForTheConnection) {
    std::future<SQLRETURN> exec;
    bool cancelable = false;
    {
        const std::lock_guard<std::recursive_mutex> other_call(dbc_.lock);
        exec = ExecDirectOnAnotherThread();
        cancelable = SpinUntil([this] { return stmt_->cancel.Request() != CancelState::Target::NO_CALL; });
    }
    exec.wait();
    EXPECT_TRUE(cancelable);
}

// The cancel wakes a wait in the running call at once, as an OCC retry backoff's.
TEST_F(CancelTest, Cancel_WakesAWaitInTheRunningCall) {
    const auto waiting = WaitBeforeTheBaseDriver(SAFETY_TIMEOUT);
    std::future<SQLRETURN> exec = ExecDirectOnAnotherThread();
    ASSERT_TRUE(waiting->WaitUntilWaiting());

    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    ASSERT_EQ(std::future_status::ready, exec.wait_for(PROMPT));
    exec.get();
    EXPECT_EQ(1, waiting->Wakes());
    // The wrapper runs the call here, not the base driver: nothing to forward, and the base statement is idle
    EXPECT_EQ(0, base_driver->cancels);
}

TEST_F(CancelTest, CancelHandle_WakesAWaitInTheRunningCall) {
    const auto waiting = WaitBeforeTheBaseDriver(SAFETY_TIMEOUT);
    std::future<SQLRETURN> exec = ExecuteOnAnotherThread();
    ASSERT_TRUE(waiting->WaitUntilWaiting());

    EXPECT_EQ(SQL_SUCCESS, SQLCancelHandle(SQL_HANDLE_STMT, stmt_));
    ASSERT_EQ(std::future_status::ready, exec.wait_for(PROMPT));
    exec.get();
    EXPECT_EQ(1, waiting->Wakes());
}

// SQLCancel returns the base driver's code, also when the wrapper stopped a wait.
TEST_F(CancelTest, Cancel_ReturnsTheBaseDriversCode) {
    base_driver->cancel_result = SQL_ERROR;
    base_driver->blocked_execs = 1;
    std::future<SQLRETURN> exec = ExecDirectOnAnotherThread();
    ASSERT_TRUE(WaitUntilTheBaseExecutes());

    EXPECT_EQ(SQL_ERROR, SQLCancel(stmt_));
    ASSERT_EQ(std::future_status::ready, exec.wait_for(PROMPT));
    exec.get();
    EXPECT_EQ(1, base_driver->cancels_during_exec);
}

// While the wrapper itself runs the call (a plugin, failover after the base call failed), the base statement isn't
// executing: a base SQLCancel could do nothing useful, and psqlODBC's lost-connection check then rewrites the
// statement's diagnostics while the call reads them. Only the flag is set.
TEST_F(CancelTest, Cancel_WhileTheWrapperRunsTheCallDoesNotReachTheBaseDriver) {
    base_driver->cancel_result = SQL_ERROR;
    const auto waiting = WaitBeforeTheBaseDriver(SAFETY_TIMEOUT);
    std::future<SQLRETURN> exec = ExecuteOnAnotherThread();
    ASSERT_TRUE(waiting->WaitUntilWaiting());

    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    EXPECT_EQ(SQL_SUCCESS, SQLCancelHandle(SQL_HANDLE_STMT, stmt_));
    ASSERT_EQ(std::future_status::ready, exec.wait_for(PROMPT));
    exec.get();
    EXPECT_EQ(0, base_driver->cancels);
}

// Mid-call, failover can have detached the base handle; the wrapper's cancel is then the whole cancel.
TEST_F(CancelTest, Cancel_DuringACallWithoutABaseHandleReturnsSuccess) {
    FakeFreeHandle(SQL_HANDLE_STMT, stmt_->wrapped_stmt);
    OdbcHelper::SetWrappedStmt(stmt_, SQL_NULL_HSTMT);
    const auto waiting = WaitBeforeTheBaseDriver(SAFETY_TIMEOUT);
    std::future<SQLRETURN> exec = ExecDirectOnAnotherThread();
    ASSERT_TRUE(waiting->WaitUntilWaiting());

    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    ASSERT_EQ(std::future_status::ready, exec.wait_for(PROMPT));
    exec.get();
    EXPECT_EQ(1, waiting->Wakes());
    EXPECT_EQ(0, base_driver->cancels);
}

// With no call running, the cancel goes to the base driver as before (psqlODBC then does nothing) and leaves nothing
// for the next call.
TEST_F(CancelTest, Cancel_IdleReachesTheBaseAndLeavesTheNextCallAlone) {
    const auto waiting = WaitBeforeTheBaseDriver(std::chrono::milliseconds(10));
    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    EXPECT_EQ(1, base_driver->cancels);
    EXPECT_EQ(nullptr, stmt_->err);

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, text_.data(), SQL_NTS));
    EXPECT_EQ(0, waiting->Wakes());
    EXPECT_EQ(1, base_driver->execs);
}

TEST_F(CancelTest, Cancel_DuringACallDoesNotReachTheNextCall) {
    const auto waiting = WaitBeforeTheBaseDriver(SAFETY_TIMEOUT);
    std::future<SQLRETURN> exec = ExecDirectOnAnotherThread();
    ASSERT_TRUE(waiting->WaitUntilWaiting());
    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    ASSERT_EQ(std::future_status::ready, exec.wait_for(PROMPT));
    exec.get();

    waiting->SetDelay(std::chrono::milliseconds(10));
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, text_.data(), SQL_NTS));
    EXPECT_EQ(nullptr, stmt_->err);
    EXPECT_EQ(1, waiting->Wakes());
}

// A call canceled before it reached the base driver doesn't run the statement: it returns HY008.
TEST_F(CancelTest, Cancel_WhileAPluginWaits_ExecDirectReturnsHY008WithoutRunning) {
    const auto waiting = WaitBeforeTheBaseDriver(SAFETY_TIMEOUT);
    std::future<SQLRETURN> exec = ExecDirectOnAnotherThread();
    ASSERT_TRUE(waiting->WaitUntilWaiting());

    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    ASSERT_EQ(std::future_status::ready, exec.wait_for(PROMPT));
    EXPECT_EQ(SQL_ERROR, exec.get());
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("HY008", stmt_->err->sqlstate);
    EXPECT_EQ(0, base_driver->execs);
}

TEST_F(CancelTest, Cancel_WhileAPluginWaits_ExecuteReturnsHY008WithoutRunning) {
    const auto waiting = WaitBeforeTheBaseDriver(SAFETY_TIMEOUT);
    std::future<SQLRETURN> exec = ExecuteOnAnotherThread();
    ASSERT_TRUE(waiting->WaitUntilWaiting());

    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    ASSERT_EQ(std::future_status::ready, exec.wait_for(PROMPT));
    EXPECT_EQ(SQL_ERROR, exec.get());
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("HY008", stmt_->err->sqlstate);
    EXPECT_EQ(0, base_driver->execs);
}

TEST_F(CancelTest, Cancel_WhileTheCallWaitsForTheConnection_ReturnsHY008WithoutRunning) {
    std::future<SQLRETURN> exec;
    bool cancelable = false;
    {
        const std::lock_guard<std::recursive_mutex> other_call(dbc_.lock);
        exec = ExecDirectOnAnotherThread();
        // Known to be waiting once its call is cancelable; SQLCancel's own result doesn't say
        cancelable = SpinUntil([this] { return stmt_->cancel.Request() != CancelState::Target::NO_CALL; });
        EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    }
    ASSERT_TRUE(cancelable);
    EXPECT_EQ(SQL_ERROR, exec.get());
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("HY008", stmt_->err->sqlstate);
    EXPECT_EQ(0, base_driver->execs);
}

// The default plugin checks just before the base driver's SQLExecDirect or SQLExecute.
TEST_F(CancelTest, DefaultPlugin_CanceledCallReturnsHY008BeforeTheBaseDriver) {
    CancelState::Scope call(stmt_->cancel);
    call.Enter();
    ASSERT_EQ(CancelState::Target::WRAPPER, stmt_->cancel.Request());

    EXPECT_EQ(SQL_ERROR, dbc_.plugin_head->Execute(stmt_, text_.data(), SQL_NTS));
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("HY008", stmt_->err->sqlstate);
    EXPECT_EQ("Operation canceled: SQLCancel was called before the statement ran", stmt_->err->error_msg);
    ClearError(stmt_);
    EXPECT_EQ(SQL_ERROR, dbc_.plugin_head->Execute(stmt_));
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("HY008", stmt_->err->sqlstate);
    EXPECT_EQ(0, base_driver->execs);
}

// A call canceled while it waited for the connection returns before any plugin runs, so no plugin changes state for it.
TEST_F(CancelTest, ExecDirect_CanceledWhileWaitingForTheConnectionReachesNoPlugin) {
    const auto counting = std::make_shared<CountingPlugin>(&dbc_, std::make_shared<DefaultPlugin>(&dbc_));
    UsePluginChain(counting);
    std::future<SQLRETURN> exec;
    bool canceled = false;
    {
        const std::lock_guard<std::recursive_mutex> other_call(dbc_.lock);
        exec = ExecDirectOnAnotherThread();
        canceled = SpinUntil([this] { return stmt_->cancel.Request() != CancelState::Target::NO_CALL; });
    }
    ASSERT_TRUE(canceled);
    EXPECT_EQ(SQL_ERROR, exec.get());
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("HY008", stmt_->err->sqlstate);
    EXPECT_EQ(0, counting->Calls());
}

TEST_F(CancelTest, Execute_CanceledWhileWaitingForTheConnectionReachesNoPlugin) {
    const auto counting = std::make_shared<CountingPlugin>(&dbc_, std::make_shared<DefaultPlugin>(&dbc_));
    UsePluginChain(counting);
    std::future<SQLRETURN> exec;
    bool canceled = false;
    {
        const std::lock_guard<std::recursive_mutex> other_call(dbc_.lock);
        exec = ExecuteOnAnotherThread();
        canceled = SpinUntil([this] { return stmt_->cancel.Request() != CancelState::Target::NO_CALL; });
    }
    ASSERT_TRUE(canceled);
    EXPECT_EQ(SQL_ERROR, exec.get());
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("HY008", stmt_->err->sqlstate);
    EXPECT_EQ(0, counting->Calls());
}

// The base driver's SQLCancel can block: psqlODBC's PQcancel connects to the server, for as long as the OS takes to give
// up on a host that stopped answering. A disconnect doesn't wait for it; the statement's base handle and the base
// connection are released once, when the cancel returns.
TEST_F(CancelTest, Cancel_BlockedInTheBaseDriverDoesNotHoldUpADisconnect) {
    const SQLHSTMT base_stmt = stmt_->wrapped_stmt;
    base_driver->block_cancel = true;
    std::future<SQLRETURN> cancel = CancelOnAnotherThread();
    const bool inside = WaitUntilTheCancelIsInside();

    std::future<void> disconnect = std::async(std::launch::async, [this] { odbc_helper_->Disconnect(&dbc_); });
    const bool prompt = disconnect.wait_for(PROMPT) == std::future_status::ready;
    const int frees_during_the_cancel = StatementFrees(base_stmt);
    const int disconnects_during_the_cancel = Disconnects(&base_dbc_);
    ReleaseTheCancel();
    disconnect.get();

    EXPECT_TRUE(inside);
    EXPECT_TRUE(prompt);
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ(0, frees_during_the_cancel);
    EXPECT_EQ(0, disconnects_during_the_cancel);
    EXPECT_EQ(1, StatementFrees(base_stmt));
    EXPECT_EQ(1, Disconnects(&base_dbc_));
    EXPECT_EQ(0, base_driver->dead_handle_calls);
    EXPECT_EQ(nullptr, stmt_->wrapped_stmt);
}

// The app frees the statement and disconnects while the base driver's SQLCancel is still inside, as psqlODBC's PQcancel
// is with a host that stopped answering. Neither waits for it, and the base driver gets neither meanwhile: psqlODBC
// before 18.00.0002 frees the statements in its SQLDisconnect, so the statement's free after it, or the cancel, would
// use a freed statement. When the cancel returns, the statement is freed, then the base connection disconnected and
// freed. It's out of the handle by then: a connect meanwhile gets a base connection of its own, which stays.
TEST_F(CancelTest, Cancel_BlockedInTheBaseDriverDoesNotHoldUpTheAppsDisconnect) {
    const SQLHSTMT base_stmt = stmt_->wrapped_stmt;
    base_driver->block_cancel = true;
    std::future<SQLRETURN> cancel = CancelOnAnotherThread();
    const bool inside = WaitUntilTheCancelIsInside();

    std::future<SQLRETURN> disconnect = std::async(std::launch::async, [this] {
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt_));
        return SQLDisconnect(&dbc_);
    });
    const bool prompt = disconnect.wait_for(PROMPT) == std::future_status::ready;
    stmt_ = nullptr;
    const int frees_during_the_cancel = StatementFrees(base_stmt);
    const int disconnects_during_the_cancel = Disconnects(&base_dbc_);
    const int connection_frees_during_the_cancel = ConnectionFrees(&base_dbc_);
    EXPECT_EQ(SQL_SUCCESS, disconnect.get());
    EXPECT_EQ(CONN_NOT_CONNECTED, dbc_.conn_status);
    EXPECT_EQ(nullptr, dbc_.wrapped_dbc);
    // A connect meanwhile: the default plugin allocates a new base connection. (A monitoring connection's attribute
    // keeps it from updating the dialect, which this plugin service has none of.)
    dbc_.conn_attr.insert_or_assign(KEY_MONITORING_CONN_UUID, "cancel-test");
    EXPECT_EQ(SQL_SUCCESS, dbc_.plugin_head->Connect(&dbc_, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    const SQLHDBC new_base_dbc = dbc_.wrapped_dbc;
    ReleaseTheCancel();

    EXPECT_TRUE(inside);
    EXPECT_TRUE(prompt);
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ(0, frees_during_the_cancel);
    EXPECT_EQ(0, disconnects_during_the_cancel);
    EXPECT_EQ(0, connection_frees_during_the_cancel);
    EXPECT_EQ(1, StatementFrees(base_stmt));
    EXPECT_EQ(1, Disconnects(&base_dbc_));
    EXPECT_EQ(1, ConnectionFrees(&base_dbc_));
    ASSERT_NE(nullptr, new_base_dbc);
    EXPECT_NE(static_cast<SQLHDBC>(&base_dbc_), new_base_dbc);
    EXPECT_EQ(0, Disconnects(new_base_dbc));
    EXPECT_EQ(0, ConnectionFrees(new_base_dbc));
    EXPECT_EQ(0, DeadHandleCalls());
}

// Without a cancel inside, SQLDisconnect gives the base driver's answer at once and keeps the base connection for the
// next connect, as before.
TEST_F(CancelTest, Disconnect_WithoutACancelRunsAtOnce) {
    const SQLHSTMT base_stmt = stmt_->wrapped_stmt;
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt_));
    stmt_ = nullptr;
    EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(&dbc_));
    EXPECT_EQ(1, StatementFrees(base_stmt));
    EXPECT_EQ(1, Disconnects(&base_dbc_));
    EXPECT_EQ(0, ConnectionFrees(&base_dbc_));
    EXPECT_EQ(static_cast<SQLHDBC>(&base_dbc_), dbc_.wrapped_dbc);
    EXPECT_EQ(CONN_NOT_CONNECTED, dbc_.conn_status);
}

// The app frees the connection while the base driver's SQLCancel is still inside, after a disconnect or without one.
// The DBC goes at once; the cancel's pin keeps what it needs, and when the cancel returns the base statement is freed,
// then the base connection (disconnected first after an SQLDisconnect), each once.
TEST_F(CancelTest, Cancel_BlockedInTheBaseDriverOutlivesTheFreedConnection) {
    std::array<int, 2> base_dbcs{};  // only their addresses are used
    for (const bool disconnect_first : {true, false}) {
        SCOPED_TRACE(disconnect_first ? "SQLDisconnect, then SQLFreeHandle" : "SQLFreeHandle only");
        int& base_dbc = base_dbcs.at(disconnect_first ? 0 : 1);
        const int dead_before = DeadHandleCalls();
        {
            const std::lock_guard<std::mutex> lock(base_driver->mutex);
            base_driver->cancel_released = false;
        }
        auto* dbc = new DBC();
        dbc->env = &env_;
        dbc->wrapped_dbc = &base_dbc;
        dbc->conn_status = CONN_CONNECTED;
        dbc->plugin_service = std::make_shared<TestPluginService>(odbc_helper_);
        const auto chain = std::make_shared<DefaultPlugin>(dbc);
        dbc->plugin_head = chain.get();
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        ASSERT_EQ(SQL_SUCCESS, RDS_AllocStmt(dbc, &stmt));
        const SQLHSTMT base_stmt = static_cast<STMT*>(stmt)->wrapped_stmt;
        base_driver->block_cancel = true;
        std::future<SQLRETURN> cancel = std::async(std::launch::async, [stmt] { return SQLCancel(stmt); });
        const bool inside = WaitUntilTheCancelIsInside();

        std::future<void> freeing = std::async(std::launch::async, [dbc, stmt, disconnect_first] {
            EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
            if (disconnect_first) {
                EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
            }
            EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc));
        });
        const bool prompt = freeing.wait_for(PROMPT) == std::future_status::ready;
        const int frees_during_the_cancel = StatementFrees(base_stmt) + ConnectionFrees(&base_dbc);
        ReleaseTheCancel();
        freeing.get();

        EXPECT_TRUE(inside);
        EXPECT_TRUE(prompt);
        EXPECT_EQ(SQL_SUCCESS, cancel.get());
        EXPECT_EQ(0, frees_during_the_cancel);
        EXPECT_EQ(1, StatementFrees(base_stmt));
        EXPECT_EQ(disconnect_first ? 1 : 0, Disconnects(&base_dbc));
        EXPECT_EQ(1, ConnectionFrees(&base_dbc));
        EXPECT_EQ(dead_before, DeadHandleCalls());
    }
}

// The app frees the environment while the base driver's SQLCancel is still inside: after freeing the statement and the
// connection, after an SQLDisconnect that frees the statement (pyodbc's close()), or with both left. None of the frees
// waits for the cancel, and the base driver gets no call meanwhile: psqlODBC frees every connection and statement of an
// environment with it. When the cancel returns, the base statement is freed, then the base connection (disconnected
// first after an SQLDisconnect), and last the base environment, each once.
TEST_F(CancelTest, Cancel_BlockedInTheBaseDriverOutlivesTheFreedEnvironment) {
    struct Sequence {
        const char* name;
        bool free_the_statement;
        bool disconnect_and_free_the_connection;
    };
    const std::array<Sequence, 3> sequences{{
        {"(a) SQLFreeHandle(STMT), SQLDisconnect, SQLFreeHandle(DBC), SQLFreeHandle(ENV)", true, true},
        {"(b) SQLDisconnect with the statement left, SQLFreeHandle(DBC), SQLFreeHandle(ENV)", false, true},
        {"(c) SQLFreeHandle(ENV) with the connection and the statement left", false, false},
    }};
    std::array<int, 3> base_dbcs{};  // only their addresses are used
    for (size_t i = 0; i < sequences.size(); ++i) {
        const Sequence& sequence = sequences.at(i);
        SCOPED_TRACE(sequence.name);
        {
            const std::lock_guard<std::mutex> lock(base_driver->mutex);
            base_driver->cancel_released = false;
            base_driver->freed_env = false;
            base_driver->calls.clear();
            base_driver->env_frees.clear();
        }
        const int dead_before = DeadHandleCalls();
        int& base_dbc = base_dbcs.at(i);
        ENV* env = NewEnvironment();
        STMT* stmt = NewConnectedStatement(env, base_dbc);
        DBC* dbc = stmt->dbc;
        const SQLHSTMT base_stmt = stmt->wrapped_stmt;
        std::future<SQLRETURN> cancel = BlockedCancel(stmt);
        const bool inside = WaitUntilTheCancelIsInside();

        std::future<void> freeing = std::async(std::launch::async, [env, dbc, stmt, sequence] {
            if (sequence.free_the_statement) {
                EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
            }
            if (sequence.disconnect_and_free_the_connection) {
                EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
                EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc));
            }
            EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, env));
        });
        const bool prompt = freeing.wait_for(PROMPT) == std::future_status::ready;
        const std::vector<std::string> calls_during_the_cancel = Calls();
        ReleaseTheCancel();
        freeing.get();

        EXPECT_TRUE(inside);
        EXPECT_TRUE(prompt);
        EXPECT_EQ(SQL_SUCCESS, cancel.get());
        EXPECT_EQ(std::vector<std::string>{}, calls_during_the_cancel);
        std::vector<std::string> expected_calls{"Cancel", "FreeHandle(STMT)"};
        if (sequence.disconnect_and_free_the_connection) {
            expected_calls.emplace_back("Disconnect");
        }
        expected_calls.insert(expected_calls.end(), {"FreeHandle(DBC)", "FreeHandle(ENV)"});
        EXPECT_EQ(expected_calls, Calls());
        EXPECT_EQ(1, StatementFrees(base_stmt));
        EXPECT_EQ(1, ConnectionFrees(&base_dbc));
        EXPECT_EQ(1, EnvironmentFrees(&base_env_));
        EXPECT_EQ(dead_before, DeadHandleCalls());
    }
}

// Without a cancel inside, the base environment goes in SQLFreeHandle(ENV), after its connections' base handles, as
// before.
TEST_F(CancelTest, FreeEnv_WithoutACancelFreesTheBaseEnvironmentAtOnce) {
    ENV* env = NewEnvironment();
    int base_dbc = 0;
    const SQLHSTMT base_stmt = NewConnectedStatement(env, base_dbc)->wrapped_stmt;

    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, env));

    EXPECT_EQ((std::vector<std::string>{"FreeHandle(STMT)", "FreeHandle(DBC)", "FreeHandle(ENV)"}), Calls());
    EXPECT_EQ(1, StatementFrees(base_stmt));
    EXPECT_EQ(1, ConnectionFrees(&base_dbc));
    EXPECT_EQ(1, EnvironmentFrees(&base_env_));
    EXPECT_EQ(0, DeadHandleCalls());
}

// The app frees the environment while the base driver's SQLCancel is inside on one of its two connections. The other
// connection's base handles go at once; the canceled one's when the cancel returns, and then the base environment,
// once.
TEST_F(CancelTest, FreeEnv_TwoConnectionsOneBlockedCancel_FreesTheBaseEnvironmentOnceAfterIt) {
    ENV* env = NewEnvironment();
    int canceled_base_dbc = 0;
    int idle_base_dbc = 0;
    STMT* canceled = NewConnectedStatement(env, canceled_base_dbc);
    const SQLHSTMT canceled_base_stmt = canceled->wrapped_stmt;
    const SQLHSTMT idle_base_stmt = NewConnectedStatement(env, idle_base_dbc)->wrapped_stmt;
    std::future<SQLRETURN> cancel = BlockedCancel(canceled);
    const bool inside = WaitUntilTheCancelIsInside();

    std::future<SQLRETURN> freeing = std::async(std::launch::async, [env] { return SQLFreeHandle(SQL_HANDLE_ENV, env); });
    const bool prompt = freeing.wait_for(PROMPT) == std::future_status::ready;
    const std::vector<std::string> calls_during_the_cancel = Calls();
    const int idle_frees_during_the_cancel = StatementFrees(idle_base_stmt) + ConnectionFrees(&idle_base_dbc);
    ReleaseTheCancel();

    EXPECT_TRUE(inside);
    EXPECT_TRUE(prompt);
    EXPECT_EQ(SQL_SUCCESS, freeing.get());
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ((std::vector<std::string>{"FreeHandle(STMT)", "FreeHandle(DBC)"}), calls_during_the_cancel);
    EXPECT_EQ(2, idle_frees_during_the_cancel);
    EXPECT_EQ((std::vector<std::string>{"FreeHandle(STMT)", "FreeHandle(DBC)", "Cancel", "FreeHandle(STMT)",
                                        "FreeHandle(DBC)", "FreeHandle(ENV)"}),
              Calls());
    EXPECT_EQ(1, StatementFrees(canceled_base_stmt));
    EXPECT_EQ(1, ConnectionFrees(&canceled_base_dbc));
    EXPECT_EQ(1, EnvironmentFrees(&base_env_));
    EXPECT_EQ(0, DeadHandleCalls());
}

// The app frees another statement of the connection, then the environment, while the base driver's SQLCancel is
// inside on one statement. Neither waits for it, and the base driver gets neither free meanwhile. When the cancel
// returns, both base statements are freed, then the base connection, and last the base environment, each once.
TEST_F(CancelTest, Cancel_BlockedOnOneStatement_OtherStatementFreedThenEnvironment) {
    ENV* env = NewEnvironment();
    int base_dbc = 0;
    STMT* canceled = NewConnectedStatement(env, base_dbc);
    STMT* other = NewStatement(canceled->dbc);
    const SQLHSTMT canceled_base_stmt = canceled->wrapped_stmt;
    const SQLHSTMT other_base_stmt = other->wrapped_stmt;
    std::future<SQLRETURN> cancel = BlockedCancel(canceled);
    const bool inside = WaitUntilTheCancelIsInside();

    std::future<void> freeing = std::async(std::launch::async, [env, other] {
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other));
        EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, env));
    });
    const bool prompt = freeing.wait_for(PROMPT) == std::future_status::ready;
    const std::vector<std::string> calls_during_the_cancel = Calls();
    ReleaseTheCancel();
    freeing.get();

    EXPECT_TRUE(inside);
    EXPECT_TRUE(prompt);
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ(std::vector<std::string>{}, calls_during_the_cancel);
    EXPECT_EQ((std::vector<std::string>{"Cancel", "FreeHandle(STMT)", "FreeHandle(STMT)", "FreeHandle(DBC)",
                                        "FreeHandle(ENV)"}),
              Calls());
    EXPECT_EQ(1, StatementFrees(other_base_stmt));
    EXPECT_EQ(1, StatementFrees(canceled_base_stmt));
    EXPECT_EQ(1, ConnectionFrees(&base_dbc));
    EXPECT_EQ(1, EnvironmentFrees(&base_env_));
    EXPECT_EQ(0, DeadHandleCalls());
}

// A cancel that reaches the base driver pins this library first: the base call can outlive the app's last free, and
// the driver manager may unload the driver then, while this thread still has to return into it.
TEST_F(CancelTest, Cancel_PinsTheDriverModuleBeforeTheBaseCall) {
    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    ASSERT_EQ(1, base_driver->cancels);
#ifndef _WIN32
    if (!DriverModule::IsPinned()) {
        // Here the library is the unit-test executable, which dlopen doesn't find by the name dladdr gives it (its
        // argv[0]): nothing is pinned, and Pin() only has to be safe to call again.
        DriverModule::Pin();
        DriverModule::Pin();
        EXPECT_FALSE(DriverModule::IsPinned());
        return;
    }
#endif
    EXPECT_TRUE(base_driver->module_pinned_at_cancel);
}

// The failover plugin (strict writer), then the default plugin.
class CancelFailoverTest : public CancelTest {
protected:
    void SetUp() override {
        CancelTest::SetUp();
        dbc_.conn_attr.insert_or_assign(KEY_FAILOVER_MODE, VALUE_FAILOVER_MODE_STRICT_WRITER);
        dbc_.conn_attr.insert_or_assign(KEY_MONITORING_CONN_UUID, "cancel-test");
        UsePluginChain(std::make_shared<FailoverPlugin>(&dbc_, std::make_shared<DefaultPlugin>(&dbc_)));
    }
};

// The connection drops while the base driver's cancel can't get through. Failover detaches the statement, disconnects,
// reconnects and disconnects again (the new connection's role can't be read) at once; the base connections are
// disconnected once each, after the cancel returns.
TEST_F(CancelFailoverTest, Cancel_BlockedInTheBaseDriverDoesNotHoldUpFailover) {
    base_driver->blocked_execs = 1;
    base_driver->block_cancel = true;
    std::future<SQLRETURN> exec = ExecDirectOnAnotherThread();
    ASSERT_TRUE(WaitUntilTheBaseExecutes());
    std::future<SQLRETURN> cancel = CancelOnAnotherThread();
    const bool inside = WaitUntilTheCancelIsInside();

    ReleaseTheExec(SQL_ERROR);
    const bool prompt = exec.wait_for(PROMPT) == std::future_status::ready;
    const int disconnects_during_the_cancel = Disconnects(&base_dbc_);
    ReleaseTheCancel();

    EXPECT_TRUE(inside);
    EXPECT_TRUE(prompt);
    EXPECT_EQ(SQL_ERROR, exec.get());
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ(1, base_driver->cancels_during_exec);
    EXPECT_EQ(1, base_driver->driver_connects);
    EXPECT_EQ(0, disconnects_during_the_cancel);
    EXPECT_EQ(1, Disconnects(&base_dbc_));
    EXPECT_EQ(0, base_driver->dead_handle_calls);
    EXPECT_EQ(nullptr, stmt_->wrapped_stmt);
}

// The read/write splitting plugin, then the default plugin, on the writer of a writer and a reader. A SET ... READ
// ONLY statement switches the statement's connection to a new reader connection.
class CancelReadWriteSplittingTest : public CancelTest {
protected:
    std::shared_ptr<TestReadWriteSplittingPlugin> rws_;
    std::vector<SQLTCHAR> read_only_ = AppText("SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY");

    void SetUp() override {
        CancelTest::SetUp();
        rws_ = std::make_shared<TestReadWriteSplittingPlugin>(&dbc_, std::make_shared<DefaultPlugin>(&dbc_));
        UsePluginChain(rws_);
    }

    std::future<SQLRETURN> SetReadOnlyOnAnotherThread() {
        return std::async(std::launch::async, [this] { return RDS_SQLExecDirect(stmt_, read_only_.data(), SQL_NTS); });
    }
};

// A cancel before the switch stops the call with the connection as it was.
TEST_F(CancelReadWriteSplittingTest, Cancel_BeforeTheSwitchLeavesTheConnectionAsItWas) {
    const auto waiting = std::make_shared<WaitingPlugin>(&dbc_, rws_, SAFETY_TIMEOUT);
    UsePluginChain(waiting);
    std::future<SQLRETURN> exec = SetReadOnlyOnAnotherThread();
    ASSERT_TRUE(waiting->WaitUntilWaiting());

    EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_));
    ASSERT_EQ(std::future_status::ready, exec.wait_for(PROMPT));
    EXPECT_EQ(SQL_ERROR, exec.get());
    ASSERT_NE(nullptr, stmt_->err);
    EXPECT_EQ("HY008", stmt_->err->sqlstate);
    EXPECT_EQ(&base_dbc_, dbc_.wrapped_dbc);
    EXPECT_EQ(0, base_driver->driver_connects);
    EXPECT_EQ(0, base_driver->execs);
}

// Once the plugin has switched the connection, the statement runs: an HY008 would leave the connection on the reader
// while the app thinks the SET didn't run.
TEST_F(CancelReadWriteSplittingTest, Cancel_DuringTheSwitchLetsTheStatementRun) {
    base_driver->on_driver_connect = [this] { EXPECT_EQ(SQL_SUCCESS, SQLCancel(stmt_)); };

    EXPECT_EQ(SQL_SUCCESS, RDS_SQLExecDirect(stmt_, read_only_.data(), SQL_NTS));
    EXPECT_EQ(nullptr, stmt_->err);
    EXPECT_EQ(1, base_driver->driver_connects);
    EXPECT_NE(&base_dbc_, dbc_.wrapped_dbc);
    EXPECT_EQ(1, base_driver->execs);
    EXPECT_FALSE(stmt_->cancel.IsCanceled());
}

// The read/write splitting plugin's switch and its error path don't wait for a blocked base cancel either.
TEST_F(CancelReadWriteSplittingTest, Cancel_BlockedInTheBaseDriverDoesNotHoldUpASwitch) {
    const SQLHSTMT writer_stmt = stmt_->wrapped_stmt;
    int reader_base_dbc = 0;
    DBC reader{};
    reader.env = &env_;
    reader.wrapped_dbc = &reader_base_dbc;
    base_driver->block_cancel = true;
    std::future<SQLRETURN> cancel = CancelOnAnotherThread();
    const bool inside = WaitUntilTheCancelIsInside();

    std::future<SQLHSTMT> switched = std::async(std::launch::async, [this, &reader] {
        rws_->SetCurrentConnection(&base_dbc_);
        rws_->SwitchCurrentConnectionTo(&reader, READER_HOST);
        SQLHSTMT reader_stmt = SQL_NULL_HSTMT;
        FakeAllocHandle(SQL_HANDLE_STMT, dbc_.wrapped_dbc, &reader_stmt);
        OdbcHelper::SetWrappedStmt(stmt_, reader_stmt);
        rws_->SetStmtError("The switch failed.", ERR_RW_SWITCH_FAILED);
        return reader_stmt;
    });
    const bool prompt = switched.wait_for(PROMPT) == std::future_status::ready;
    const int frees_during_the_cancel = StatementFrees(writer_stmt);
    const int disconnects_during_the_cancel = Disconnects(&base_dbc_);
    ReleaseTheCancel();
    const SQLHSTMT reader_stmt = switched.get();

    EXPECT_TRUE(inside);
    EXPECT_TRUE(prompt);
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ(&reader_base_dbc, dbc_.wrapped_dbc);
    EXPECT_EQ(0, frees_during_the_cancel);
    EXPECT_EQ(0, disconnects_during_the_cancel);
    EXPECT_EQ(1, StatementFrees(writer_stmt));
    EXPECT_EQ(1, Disconnects(&base_dbc_));
    EXPECT_EQ(1, StatementFrees(reader_stmt));
    EXPECT_EQ(0, base_driver->dead_handle_calls);
    dbc_.wrapped_dbc = &base_dbc_;
}

// The read/write splitting plugin's error path frees the base handle the blocked cancel uses: once, after it returns.
TEST_F(CancelReadWriteSplittingTest, Cancel_BlockedInTheBaseDriverDoesNotHoldUpTheErrorPath) {
    const SQLHSTMT base_stmt = stmt_->wrapped_stmt;
    base_driver->block_cancel = true;
    std::future<SQLRETURN> cancel = CancelOnAnotherThread();
    const bool inside = WaitUntilTheCancelIsInside();

    std::future<void> failed = std::async(std::launch::async,
        [this] { rws_->SetStmtError("The switch failed.", ERR_RW_SWITCH_FAILED); });
    const bool prompt = failed.wait_for(PROMPT) == std::future_status::ready;
    const int frees_during_the_cancel = StatementFrees(base_stmt);
    ReleaseTheCancel();
    failed.get();

    EXPECT_TRUE(inside);
    EXPECT_TRUE(prompt);
    EXPECT_EQ(SQL_SUCCESS, cancel.get());
    EXPECT_EQ(0, frees_during_the_cancel);
    EXPECT_EQ(1, StatementFrees(base_stmt));
    EXPECT_EQ(0, base_driver->dead_handle_calls);
    EXPECT_EQ(nullptr, stmt_->wrapped_stmt);
}

// Cancels on two threads while the real failover and read/write splitting plugins and OdbcHelper::Disconnect detach,
// free and disconnect the statement's base handles, and the default plugin allocates the next one. No cancel may reach
// a handle that was freed or disconnected.
class CancelStressTest : public CancelFailoverTest {
protected:
    std::shared_ptr<TestReadWriteSplittingPlugin> rws_;
    std::vector<std::unique_ptr<DBC>> switched_to_;  // the connections the switches move the statement to

    void SetUp() override {
        CancelFailoverTest::SetUp();
        rws_ = std::make_shared<TestReadWriteSplittingPlugin>(&dbc_, nullptr);
    }

    void TearDown() override {
        rws_ = nullptr;
        CancelFailoverTest::TearDown();
        switched_to_.clear();
    }
};

TEST_F(CancelStressTest, Cancel_RacingFailoverAndReadWriteSplittingNeverReachesAFreedHandle) {
    constexpr int ROUNDS = 400;
    // Between two cancels, so that most calls get to the base driver and fail over
    constexpr int PAUSE_YIELDS = 500;
    base_driver->exec_yields = 500;
    std::atomic<bool> done = false;
    const auto cancel_until_done = [this, &done] {
        int cancels = 0;
        while (!done.load()) {
            RDS_SQLCancel(stmt_);
            ++cancels;
            for (int i = 0; i < PAUSE_YIELDS; ++i) {
                std::this_thread::yield();
            }
        }
        return cancels;
    };
    std::future<int> canceler = std::async(std::launch::async, cancel_until_done);
    std::future<int> other_canceler = std::async(std::launch::async, cancel_until_done);

    int unexpected_results = 0;
    for (int i = 0; i < ROUNDS; ++i) {
        switch (i % 4) {
            case 0: {
                // The connection drops: failover detaches the statement, disconnects, reconnects and disconnects again
                {
                    const std::lock_guard<std::mutex> lock(base_driver->mutex);
                    base_driver->failing_execs = 1;
                }
                if (RDS_SQLExecDirect(stmt_, text_.data(), SQL_NTS) != SQL_ERROR) {
                    ++unexpected_results;
                }
                break;
            }
            case 1: {
                // A read/write splitting switch to another connection
                auto other = std::make_unique<DBC>();
                other->env = &env_;
                FakeAllocHandle(SQL_HANDLE_DBC, env_.wrapped_env, &other->wrapped_dbc);
                rws_->SetCurrentConnection(dbc_.wrapped_dbc);
                rws_->SwitchCurrentConnectionTo(other.get(), READER_HOST);
                switched_to_.push_back(std::move(other));
                break;
            }
            case 2:
                // Its error path
                rws_->SetStmtError("The switch failed.", ERR_RW_SWITCH_FAILED);
                break;
            default:
                // As the Blue/Green and initial connection strategy plugins and failover's reconnect do
                odbc_helper_->Disconnect(&dbc_);
                break;
        }
        {
            const std::lock_guard<std::recursive_mutex> dbc_lock(dbc_.lock);
            if (dbc_.wrapped_dbc == SQL_NULL_HDBC) {
                FakeAllocHandle(SQL_HANDLE_DBC, env_.wrapped_env, &dbc_.wrapped_dbc);
            }
        }
        // The default plugin allocates the next base handle, unless a cancel stops the call first (HY008)
        const SQLRETURN rc = RDS_SQLExecDirect(stmt_, text_.data(), SQL_NTS);
        if (rc != SQL_SUCCESS && rc != SQL_ERROR) {
            ++unexpected_results;
        }
    }
    done = true;
    const int cancels = canceler.get() + other_canceler.get();

    EXPECT_EQ(0, unexpected_results);
    EXPECT_EQ(0, base_driver->dead_handle_calls);
    EXPECT_GT(base_driver->cancels, 0);
    EXPECT_GE(cancels, base_driver->cancels);
    // Failover ran, not only the cancels
    EXPECT_GT(base_driver->driver_connects, 0);
}
