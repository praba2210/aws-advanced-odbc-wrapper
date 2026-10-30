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
// connection; from MAX_CONNECTION_LIFETIME on, the driver answers SQL_CD_TRUE, so the pool disconnects the connection
// and opens a new one, with a new token. The 60-minute hint reads the same age.
class DsqlConnectionLifetime {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // MAX_CONNECTION_LIFETIME's default and largest value: the Aurora DSQL connectors' 55 minutes.
    static constexpr std::chrono::seconds DEFAULT_MAX_LIFETIME{3300};
    // Its smallest value other than 0, which turns retirement off.
    static constexpr std::chrono::seconds MIN_MAX_LIFETIME{120};

    // The steady clock, unless a test passes its own.
    explicit DsqlConnectionLifetime(Clock clock = nullptr);

    // MAX_CONNECTION_LIFETIME from upper-cased connection attributes: the default when it isn't set, 0 for off. On a
    // bad value, returns std::nullopt and sets error to the option, its value and what to use instead, for example
    // "MAX_CONNECTION_LIFETIME=3600 (use 0 for off, or an integer from 120 to 3300)".
    static std::optional<std::chrono::seconds> ParseMaxLifetime(const std::map<std::string, std::string>& conn_attr,
                                                                std::string& error);

    // True when MAX_CONNECTION_LIFETIME is set to an integer other than 0. The attribute validator checks its range.
    static bool IsSet(const std::map<std::string, std::string>& conn_attr);

    // For SQLGetConnectAttr(SQL_ATTR_CONNECTION_DEAD), without a round trip: true when dbc is an open Aurora DSQL
    // connection at least MAX_CONNECTION_LIFETIME old. Logs one INFO line per connection, the first time.
    static bool IsRetired(const DBC* dbc);

    [[nodiscard]] std::chrono::steady_clock::time_point Now() const;

    // A connect that began at started_at succeeded. A max_lifetime of 0 never retires the connection.
    void Start(std::chrono::steady_clock::time_point started_at, std::chrono::seconds max_lifetime);

    // No connection: a connect began, or failed.
    void Clear();

    // How long ago the current connection's connect began, or std::nullopt without one.
    [[nodiscard]] std::optional<std::chrono::steady_clock::duration> Age() const;

private:
    // True from MAX_CONNECTION_LIFETIME on, on an Aurora DSQL connection; logs the INFO line the first time.
    bool CheckRetired(const std::map<std::string, std::string>& conn_attr);

    Clock clock_;
    mutable std::mutex mutex_;
    std::optional<std::chrono::steady_clock::time_point> started_at_;
    std::chrono::seconds max_lifetime_ = DEFAULT_MAX_LIFETIME;
    bool retirement_logged_ = false;
};

#endif // DSQL_CONNECTION_LIFETIME_H_
