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

#include "dsql_connection_lifetime.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "../driver.h"
#include "connection_string_keys.h"
#include "logger_wrapper.h"
#include "number_utils.h"

namespace {
    // Monotonic: a change of the system time doesn't move it, but it stops while the machine is asleep.
    std::chrono::steady_clock::time_point SteadyNow()
    {
        return std::chrono::steady_clock::now();
    }

    // Counts the time the machine was asleep, but jumps when the system time is set.
    std::chrono::system_clock::time_point SystemNow()
    {
        return std::chrono::system_clock::now();
    }
}  // namespace

DsqlConnectionLifetime::DsqlConnectionLifetime(SteadyClock steady_clock, SystemClock system_clock)
{
    const bool test_clocks = steady_clock != nullptr;
    steady_clock_ = test_clocks ? std::move(steady_clock) : SteadyClock(SteadyNow);
    if (system_clock != nullptr) {
        system_clock_ = std::move(system_clock);
    } else if (!test_clocks) {
        system_clock_ = SystemNow;
    }
}

std::optional<std::chrono::seconds> DsqlConnectionLifetime::ParseMaxLifetime(
    const std::map<std::string, std::string>& conn_attr, std::string& error)
{
    const auto it = conn_attr.find(KEY_MAX_CONNECTION_LIFETIME);
    // A blank value (any ASCII whitespace), as the setup dialog leaves an empty field, is the same as unset.
    if (it == conn_attr.end() || it->second.find_first_not_of(" \t\r\n\v\f") == std::string::npos) {
        return DEFAULT_MAX_LIFETIME;
    }
    const std::optional<int> seconds = NumberUtils::ParseInt(it->second);
    if (seconds.has_value()
        && (seconds.value() == 0
            || (seconds.value() >= MIN_MAX_LIFETIME.count() && seconds.value() <= LARGEST_MAX_LIFETIME.count()))) {
        return std::chrono::seconds(seconds.value());
    }
    error = std::string(KEY_MAX_CONNECTION_LIFETIME) + "=" + it->second + " (use 0 for off, or an integer from "
            + std::to_string(MIN_MAX_LIFETIME.count()) + " to " + std::to_string(LARGEST_MAX_LIFETIME.count()) + ")";
    return std::nullopt;
}

bool DsqlConnectionLifetime::IsSet(const std::map<std::string, std::string>& conn_attr)
{
    const auto it = conn_attr.find(KEY_MAX_CONNECTION_LIFETIME);
    return it != conn_attr.end() && NumberUtils::ParseInt(it->second).value_or(0) != 0;
}

DsqlConnectionLifetime::Instant DsqlConnectionLifetime::Now() const
{
    return {steady_clock_(), system_clock_ != nullptr ? std::optional(system_clock_()) : std::nullopt};
}

void DsqlConnectionLifetime::Start(Instant started_at, std::chrono::seconds max_lifetime)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    started_at_ = started_at;
    max_lifetime_ = max_lifetime;
    retirement_logged_ = false;
}

void DsqlConnectionLifetime::Clear()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    started_at_.reset();
    retirement_logged_ = false;
}

std::optional<std::chrono::steady_clock::duration> DsqlConnectionLifetime::Age() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!started_at_.has_value()) {
        return std::nullopt;
    }
    return AgeSince(started_at_.value());
}

std::chrono::steady_clock::duration DsqlConnectionLifetime::AgeSince(const Instant& started_at) const
{
    std::chrono::steady_clock::duration age = steady_clock_() - started_at.steady;
    if (system_clock_ != nullptr && started_at.system.has_value()) {
        // A system time set back gives a smaller (or negative) age: the steady one wins.
        age = std::max(age, std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                system_clock_() - started_at.system.value()));
    }
    return age;
}

bool DsqlConnectionLifetime::IsRetired(const DBC* dbc)
{
    if (!HasWrappedHandle(dbc) || !dbc->is_dsql || dbc->dsql_lifetime == nullptr
        || dbc->conn_status != CONN_CONNECTED) {
        return false;
    }
    return dbc->dsql_lifetime->CheckRetired();
}

bool DsqlConnectionLifetime::CheckRetired()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!started_at_.has_value() || max_lifetime_.count() == 0) {
        return false;
    }
    const std::chrono::steady_clock::duration age = AgeSince(started_at_.value());
    if (age < max_lifetime_) {
        return false;
    }
    if (!retirement_logged_) {
        retirement_logged_ = true;
        LOG(INFO) << "[DSQL] Retiring the connection after "
                  << std::chrono::duration_cast<std::chrono::seconds>(age).count() << " seconds ("
                  << KEY_MAX_CONNECTION_LIFETIME << "=" << max_lifetime_.count()
                  << "): SQL_ATTR_CONNECTION_DEAD now reports it dead, so a connection pool replaces it.";
    }
    return true;
}
