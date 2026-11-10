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

#ifndef DSQL_CONNECTION_LIFETIME_H_
#define DSQL_CONNECTION_LIFETIME_H_

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

struct DBC;

// How old an Aurora DSQL connection is, and the age at which the driver retires it (MAX_CONNECTION_LIFETIME). Aurora
// DSQL closes connections at 60 minutes. A driver manager asks SQL_ATTR_CONNECTION_DEAD before it hands out a pooled
// connection; once a connection is MAX_CONNECTION_LIFETIME old, the driver answers SQL_CD_TRUE, so the pool
// disconnects it and opens a new one, with a new token. Off unless MAX_CONNECTION_LIFETIME is set: an application that
// asks on a connection it is using (nanodbc, before each commit) would skip its commits. The 60-minute hint reads the
// same age.
class DsqlConnectionLifetime {
public:
    using SteadyClock = std::function<std::chrono::steady_clock::time_point()>;
    using SystemClock = std::function<std::chrono::system_clock::time_point()>;

    // A moment on both clocks. The steady clock stops while the machine sleeps, Aurora DSQL's 60 minutes don't; the
    // system clock counts the sleep but jumps when the time is set. The age is the larger of the two, so either can
    // only make a connection older (retired early: safe), never younger.
    struct Instant {
        // Implicit, so a test can pass a steady time point; it then has no system time.
        // NOLINTNEXTLINE(google-explicit-constructor,misc-explicit-constructor)
        Instant(std::chrono::steady_clock::time_point steady_time,
                std::optional<std::chrono::system_clock::time_point> system_time = std::nullopt)
            : steady(steady_time), system(system_time) {}
        std::chrono::steady_clock::time_point steady;
        std::optional<std::chrono::system_clock::time_point> system;
    };

    // MAX_CONNECTION_LIFETIME when it isn't set: off.
    static constexpr std::chrono::seconds DEFAULT_MAX_LIFETIME{0};
    // Its largest value: the Aurora DSQL connectors' 55 minutes.
    static constexpr std::chrono::seconds LARGEST_MAX_LIFETIME{3300};
    // Its smallest value other than 0, which turns retirement off.
    static constexpr std::chrono::seconds MIN_MAX_LIFETIME{120};

    // Both real clocks, unless a test passes its own. A test that passes only a steady clock counts no system time.
    explicit DsqlConnectionLifetime(SteadyClock steady_clock = nullptr, SystemClock system_clock = nullptr);

    // MAX_CONNECTION_LIFETIME from upper-cased connection attributes: 0 (off) when it isn't set or is blank. On a
    // bad value, returns std::nullopt and sets error to the option, its value and what to use instead, for example
    // "MAX_CONNECTION_LIFETIME=3600 (use 0 for off, or an integer from 120 to 3300)".
    static std::optional<std::chrono::seconds> ParseMaxLifetime(const std::map<std::string, std::string>& conn_attr,
                                                                std::string& error);

    // True when MAX_CONNECTION_LIFETIME is set to an integer other than 0. The attribute validator checks its range.
    static bool IsSet(const std::map<std::string, std::string>& conn_attr);

    // For SQLGetConnectAttr(SQL_ATTR_CONNECTION_DEAD), without a round trip: true when dbc is an open Aurora DSQL
    // connection (dbc->is_dsql) at least MAX_CONNECTION_LIFETIME old. Logs one INFO line per connection, the first
    // time.
    static bool IsRetired(const DBC* dbc);

    [[nodiscard]] Instant Now() const;

    // A connect that began at started_at succeeded. A max_lifetime of 0 never retires the connection.
    void Start(Instant started_at, std::chrono::seconds max_lifetime);

    // No connection: a connect began, or failed.
    void Clear();

    // How long ago the current connection's connect began, the larger of its steady and system ages, or std::nullopt
    // without one.
    [[nodiscard]] std::optional<std::chrono::steady_clock::duration> Age() const;

private:
    // True from MAX_CONNECTION_LIFETIME on; logs the INFO line the first time.
    bool CheckRetired();
    // The age of a connection whose connect began at started_at: the larger of its steady and system ages.
    [[nodiscard]] std::chrono::steady_clock::duration AgeSince(const Instant& started_at) const;

    SteadyClock steady_clock_;
    SystemClock system_clock_;  // null: no system age
    mutable std::mutex mutex_;
    std::optional<Instant> started_at_;
    std::chrono::seconds max_lifetime_ = DEFAULT_MAX_LIFETIME;
    bool retirement_logged_ = false;
};

#endif // DSQL_CONNECTION_LIFETIME_H_
