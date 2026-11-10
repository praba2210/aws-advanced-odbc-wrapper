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

#ifndef BASE_HANDLE_GUARD_H_
#define BASE_HANDLE_GUARD_H_

#include "windows_headers.h"

#include <sql.h>

#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

class BaseHandleGuard;

// A statement's base handle (STMT::wrapped_stmt). It reads as the SQLHSTMT it holds (or Get()), but it can't be
// assigned: once the app has the statement, an SQLCancel from another thread reads it, so it's written only through
// its connection's BaseHandleGuard (Store, Detach; OdbcHelper::SetWrappedStmt, FreeWrappedStmt, ReleaseBaseHandles).
// A plain write, such as `stmt->wrapped_stmt = nullptr`, doesn't compile.
class WrappedStmt {
public:
    WrappedStmt() = default;
    // A copy is a read: RdsLibLoader::CallFunctionChecked takes its arguments by value.
    WrappedStmt(const WrappedStmt&) = default;
    WrappedStmt& operator=(const WrappedStmt&) = delete;
    WrappedStmt& operator=(SQLHSTMT) = delete;
    ~WrappedStmt() = default;

    // Implicit, so that reads stay `stmt->wrapped_stmt` wherever an SQLHSTMT is taken.
    operator SQLHSTMT() const { return handle_; }  // NOLINT(google-explicit-constructor,misc-explicit-constructor)

    [[nodiscard]] SQLHSTMT Get() const { return handle_; }

private:
    friend class BaseHandleGuard;

    SQLHSTMT handle_ = SQL_NULL_HSTMT;
};

// The base driver handles of one connection (DBC::base_handles) against an SQLCancel from another thread, which uses
// a statement's base handle without the statement's lock.
// - Once the app has a statement, its STMT::wrapped_stmt is stored, nulled and read for a cancel only through this
//   (Store, Detach, PinnedStatement). The mutex is held only for that, never across a base driver call: a leaf lock,
//   after dbc->lock and the statement's lock.
// - SQLCancel pins the connection while it calls the base driver. A teardown of base handles that comes meanwhile (a
//   detached statement handle's free, the base disconnect, the base connection's free) is handed to the last pinned
//   call, which runs it when the base driver returns, instead of waiting for it: a base SQLCancel can block for as long
//   as the OS takes to give up a TCP connect, and failover, read/write splitting and disconnects must not wait behind
//   it.
// - Teardowns run one at a time, in the order they came (a statement's free before its connection's disconnect): one
//   that comes while handed-over ones run is handed over too.
// - The pins and the teardowns handed over live as long as the last pin: the app can free the connection, the DBC
//   with this guard, and its environment while an SQLCancel is still in the base driver: the state keeps the base
//   environment (KeepAlive).
class BaseHandleGuard {
public:
    using Teardown = std::function<void()>;

private:
    struct State {
        std::mutex mutex;
        int pins = 0;                     // the base driver calls that use the connection's handles
        std::vector<Teardown> deferred;   // run when pins is back to 0
        bool running_deferred = false;    // an Unpin() runs handed-over teardowns
        // What the teardowns need beyond their captures (the base environment): kept until the guard and the last pin
        // are gone, after every teardown handed over.
        std::shared_ptr<const void> keep_alive;
    };

public:
    // The statement's base handle, pinned for one base driver call while this lives; null (and nothing pinned) when
    // the statement has none. The last pin to end runs the teardowns handed over meanwhile.
    class PinnedStatement {
    public:
        PinnedStatement(const BaseHandleGuard& guard, const WrappedStmt& wrapped_stmt)
            : state_(guard.state_), handle_(Pin(*state_, wrapped_stmt)) {}

        ~PinnedStatement() {
            if (handle_ != SQL_NULL_HSTMT) {
                Unpin(*state_);
            }
        }

        PinnedStatement(const PinnedStatement&) = delete;
        PinnedStatement(PinnedStatement&&) = delete;
        PinnedStatement& operator=(const PinnedStatement&) = delete;
        PinnedStatement& operator=(PinnedStatement&&) = delete;

        [[nodiscard]] SQLHSTMT Handle() const { return handle_; }

    private:
        const std::shared_ptr<State> state_;  // the guard's, which may go first
        const SQLHSTMT handle_;
    };

    BaseHandleGuard() = default;
    BaseHandleGuard(const BaseHandleGuard&) = delete;
    BaseHandleGuard(BaseHandleGuard&&) = delete;
    BaseHandleGuard& operator=(const BaseHandleGuard&) = delete;
    BaseHandleGuard& operator=(BaseHandleGuard&&) = delete;

    // With a call still pinned, what was handed over stays for the last pin; else nothing is left to run.
    ~BaseHandleGuard() {
        std::vector<Teardown> teardowns;
        {
            const std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->pins > 0 || state_->running_deferred) {
                return;
            }
            teardowns.swap(state_->deferred);
        }
        RunAll(teardowns);
    }

    // Keeps object as long as the guard or a pin: past every teardown handed to the last pin.
    void KeepAlive(std::shared_ptr<const void> object) {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->keep_alive = std::move(object);
    }

    // Stores a statement's base handle.
    void Store(WrappedStmt& wrapped_stmt, SQLHSTMT handle) {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        wrapped_stmt.handle_ = handle;
    }

    // Nulls a statement's base handle and returns it; the caller frees it with RunOrDefer.
    [[nodiscard]] SQLHSTMT Detach(WrappedStmt& wrapped_stmt) {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        const SQLHSTMT handle = wrapped_stmt.handle_;
        wrapped_stmt.handle_ = SQL_NULL_HSTMT;
        return handle;
    }

    // Runs a teardown of handles already detached (Detach, or the connection's base handle taken out of the DBC):
    // now, or when the last pinned call ends. It mustn't use the DBC, which may be gone by then.
    void RunOrDefer(Teardown teardown) {
        {
            const std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->pins > 0 || state_->running_deferred) {
                state_->deferred.push_back(std::move(teardown));
                return;
            }
        }
        teardown();
    }

    // Whether a base driver call is pinned, or handed-over teardowns run: a teardown now would be handed over. Once
    // none of the connection's statements has a base handle, nothing can pin it, so false stays false.
    [[nodiscard]] bool Busy() const {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->pins > 0 || state_->running_deferred;
    }

private:
    static SQLHSTMT Pin(State& state, const WrappedStmt& wrapped_stmt) {
        const std::lock_guard<std::mutex> lock(state.mutex);
        if (wrapped_stmt.handle_ != SQL_NULL_HSTMT) {
            ++state.pins;
        }
        return wrapped_stmt.handle_;
    }

    // The last pin runs the handed-over teardowns, and those handed over while they run, unless a new pin came.
    static void Unpin(State& state) {
        std::vector<Teardown> teardowns;
        {
            const std::lock_guard<std::mutex> lock(state.mutex);
            if (--state.pins > 0 || state.running_deferred || state.deferred.empty()) {
                return;
            }
            state.running_deferred = true;
            teardowns.swap(state.deferred);
        }
        while (!teardowns.empty()) {
            RunAll(teardowns);
            teardowns.clear();
            const std::lock_guard<std::mutex> lock(state.mutex);
            if (state.pins > 0 || state.deferred.empty()) {
                state.running_deferred = false;
            } else {
                teardowns.swap(state.deferred);
            }
        }
    }

    static void RunAll(const std::vector<Teardown>& teardowns) {
        for (const Teardown& teardown : teardowns) {
            teardown();
        }
    }

    const std::shared_ptr<State> state_ = std::make_shared<State>();
};

#endif // BASE_HANDLE_GUARD_H_
