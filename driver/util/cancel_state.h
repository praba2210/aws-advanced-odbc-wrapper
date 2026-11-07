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

#ifndef CANCEL_STATE_H_
#define CANCEL_STATE_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Whether SQLCancel was called, from another thread, for the statement's cancelable call: SQLExecDirect or
// SQLExecute. Each call has a Scope, from before it waits for the statement's locks until it returns, and Enter()s
// once it holds them; calls on one statement (several threads can share it) run one at a time.
// A cancel stops the call running when SQLCancel is called. With none running, it stops the first of the calls
// already waiting to run. It never reaches a call issued after SQLCancel returned, or a call that was waiting behind
// the running one. Request() also says where the call is, since only a call inside the base driver's execute is the
// base driver's to cancel.
class CancelState {
public:
    // Where SQLCancel found the statement's cancelable call.
    enum class Target : std::uint8_t {
        NO_CALL,      // none: the base driver's to handle (it may run SQLFetch, a catalog function), as before
        WRAPPER,      // in the wrapper (waiting for the connection, in a plugin): the flag stops it at its next check;
                      // failover's reconnect and a Blue/Green suspend don't check it
        BASE_DRIVER,  // inside the base driver's execute: the base driver's to cancel too
        UNCANCELABLE, // past a state change a plugin can't undo (MarkUncancelable): the statement runs
    };

    // Marks a cancelable call for its lifetime. One per call; calls don't nest.
    class Scope {
    public:
        explicit Scope(CancelState& state) : state_(state), ticket_(state.Begin()) {}
        ~Scope() { state_.End(ticket_); }

        Scope(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope& operator=(Scope&&) = delete;

        // The call holds the statement now and is the running call. Returns whether it was canceled while it waited.
        bool Enter() { return state_.Enter(ticket_); }

    private:
        CancelState& state_;
        const std::uint64_t ticket_;
    };

    // SQLCancel: cancels the running call (or the first waiting one) and wakes its WaitFor. Without a call it does
    // nothing.
    Target Request() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (running_ != 0) {
            if (uncancelable_) {
                return Target::UNCANCELABLE;
            }
            canceled_ = running_;
            cv_.notify_all();
            return in_base_driver_ ? Target::BASE_DRIVER : Target::WRAPPER;
        }
        if (calls_ > 0) {
            cancel_before_ = next_ticket_;
            return Target::WRAPPER;
        }
        return Target::NO_CALL;
    }

    // Whether the running call was canceled. The call checks it before each step it can skip.
    [[nodiscard]] bool IsCanceled() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return IsRunningCallCanceled();
    }

    // A wait inside the running call, such as a retry delay: returns true as soon as the call is canceled (at once if
    // it already was), or false when the delay runs out.
    bool WaitFor(std::chrono::milliseconds delay) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, delay, [this] { return IsRunningCallCanceled(); });
    }

    // Just before the base driver's execute: returns false, for the call to skip it, if the call was canceled.
    // Otherwise the call is inside the base driver until LeaveBaseDriver().
    [[nodiscard]] bool EnterBaseDriver() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (IsRunningCallCanceled()) {
            return false;
        }
        in_base_driver_ = true;
        return true;
    }

    void LeaveBaseDriver() {
        const std::lock_guard<std::mutex> lock(mutex_);
        in_base_driver_ = false;
    }

    // A plugin changed state for the running call that it can't undo (it switched the statement's connection): from
    // here the statement runs, and a cancel, also one that came before, has no effect on it.
    void MarkUncancelable() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (running_ != 0) {
            uncancelable_ = true;
        }
    }

private:
    std::uint64_t Begin() {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++calls_;
        return next_ticket_++;
    }

    bool Enter(std::uint64_t ticket) {
        const std::lock_guard<std::mutex> lock(mutex_);
        running_ = ticket;
        in_base_driver_ = false;
        uncancelable_ = false;
        if (ticket < cancel_before_) {
            canceled_ = ticket;
            cancel_before_ = 0;
        }
        return canceled_ == ticket;
    }

    void End(std::uint64_t ticket) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (running_ == ticket) {
            running_ = 0;
            in_base_driver_ = false;
            uncancelable_ = false;
        }
        if (canceled_ == ticket) {
            canceled_ = 0;
        }
        if (--calls_ == 0) {
            cancel_before_ = 0;
        }
    }

    [[nodiscard]] bool IsRunningCallCanceled() const {
        return running_ != 0 && canceled_ == running_ && !uncancelable_;
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t next_ticket_ = 1;  // the next call's ticket; 0 means no call
    int calls_ = 0;                  // the calls begun and not ended: the running one and those waiting
    std::uint64_t running_ = 0;      // the call that holds the statement
    std::uint64_t canceled_ = 0;     // the canceled call
    std::uint64_t cancel_before_ = 0;  // with no call running: the first of the calls begun before it to run is canceled
    bool in_base_driver_ = false;    // the running call is inside the base driver's execute
    bool uncancelable_ = false;      // the running call is past MarkUncancelable()
};

#endif // CANCEL_STATE_H_
