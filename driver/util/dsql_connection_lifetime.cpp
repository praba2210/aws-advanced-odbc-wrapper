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

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "../driver.h"
#include "connection_string_keys.h"
#include "dsql_connection_validator.h"
#include "logger_wrapper.h"
#include "number_utils.h"

namespace {
    // Monotonic: a change of the system time neither ages a connection nor makes it younger.
    std::chrono::steady_clock::time_point SteadyNow()
    {
        return std::chrono::steady_clock::now();
    }
}  // namespace

DsqlConnectionLifetime::DsqlConnectionLifetime(Clock clock)
    : clock_(clock != nullptr ? std::move(clock) : Clock(SteadyNow))
{
}

std::optional<std::chrono::seconds> DsqlConnectionLifetime::ParseMaxLifetime(
    const std::map<std::string, std::string>& conn_attr, std::string& error)
{
    const auto it = conn_attr.find(KEY_MAX_CONNECTION_LIFETIME);
    if (it == conn_attr.end()) {
        return DEFAULT_MAX_LIFETIME;
    }
    const std::optional<int> seconds = NumberUtils::ParseInt(it->second);
    if (seconds.has_value()
        && (seconds.value() == 0
            || (seconds.value() >= MIN_MAX_LIFETIME.count() && seconds.value() <= DEFAULT_MAX_LIFETIME.count()))) {
        return std::chrono::seconds(seconds.value());
    }
    error = std::string(KEY_MAX_CONNECTION_LIFETIME) + "=" + it->second + " (use 0 for off, or an integer from "
            + std::to_string(MIN_MAX_LIFETIME.count()) + " to " + std::to_string(DEFAULT_MAX_LIFETIME.count()) + ")";
    return std::nullopt;
}

bool DsqlConnectionLifetime::IsSet(const std::map<std::string, std::string>& conn_attr)
{
    const auto it = conn_attr.find(KEY_MAX_CONNECTION_LIFETIME);
    return it != conn_attr.end() && NumberUtils::ParseInt(it->second).value_or(0) != 0;
}

std::chrono::steady_clock::time_point DsqlConnectionLifetime::Now() const
{
    return clock_();
}

void DsqlConnectionLifetime::Start(std::chrono::steady_clock::time_point started_at, std::chrono::seconds max_lifetime)
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
    return clock_() - started_at_.value();
}

bool DsqlConnectionLifetime::IsRetired(const DBC* dbc)
{
    if (!HasWrappedHandle(dbc) || dbc->dsql_lifetime == nullptr || dbc->conn_status != CONN_CONNECTED) {
        return false;
    }
    return dbc->dsql_lifetime->CheckRetired(dbc->conn_attr);
}

bool DsqlConnectionLifetime::CheckRetired(const std::map<std::string, std::string>& conn_attr)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!started_at_.has_value() || max_lifetime_.count() == 0) {
        return false;
    }
    const std::chrono::steady_clock::duration age = clock_() - started_at_.value();
    // The age first: a driver manager asks on every pooled reuse.
    if (age < max_lifetime_ || !DsqlConnectionValidator::IsDsqlTarget(conn_attr)) {
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
