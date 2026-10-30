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

#ifndef OCC_RETRY_CONFIG_H_
#define OCC_RETRY_CONFIG_H_

#include <map>
#include <optional>
#include <string>

// The Aurora DSQL OCC retry options: ENABLE_OCC_RETRY and the OCC_* keys.
struct OccRetryConfig {
    static constexpr int DEFAULT_MAX_RETRIES = 3;
    static constexpr int MAX_RETRIES_LIMIT = 100;
    static constexpr int DEFAULT_BASE_DELAY_MS = 100;
    static constexpr int DEFAULT_MAX_DELAY_MS = 5000;
    // No single wait is longer before jitter, which adds up to OCC_JITTER_FACTOR times the wait (about 120 s at most
    // in all): the connection is held while the plugin waits.
    static constexpr int DELAY_LIMIT_MS = 60000;
    static constexpr double DEFAULT_JITTER_FACTOR = 0.25;

    bool enabled = false;
    int max_retries = DEFAULT_MAX_RETRIES;
    int base_delay_ms = DEFAULT_BASE_DELAY_MS;
    int max_delay_ms = DEFAULT_MAX_DELAY_MS;
    double jitter_factor = DEFAULT_JITTER_FACTOR;

    // Reads the options from upper-cased connection attributes; an option that isn't set keeps its default. On a bad
    // value, returns std::nullopt and sets error to the option, its value and what to use instead, for example
    // "OCC_MAX_RETRIES=500 (use an integer from 0 to 100)".
    static std::optional<OccRetryConfig> Parse(const std::map<std::string, std::string>& conn_attr, std::string& error);
};

#endif // OCC_RETRY_CONFIG_H_
