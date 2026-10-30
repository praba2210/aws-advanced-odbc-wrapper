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

#include "occ_retry_config.h"

#include <cmath>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include "connection_string_keys.h"
#include "number_utils.h"

namespace {
    // A decimal read in the C locale, so "0.25" means the same whatever locale the application set.
    std::optional<double> ParseDecimal(const std::string& text)
    {
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());
        double value = 0.0;
        char trailing = 0;
        if (!(stream >> value) || (stream >> trailing)) {
            return std::nullopt;
        }
        return value;
    }

    // The option's value, or nullptr when it isn't set.
    const std::string* FindValue(const std::map<std::string, std::string>& conn_attr, const std::string& key)
    {
        const auto it = conn_attr.find(key);
        return it == conn_attr.end() ? nullptr : &it->second;
    }

    std::string InvalidValue(const std::string& key, const std::string& value, const std::string& expected)
    {
        return key + "=" + value + " (" + expected + ")";
    }
}  // namespace

std::optional<OccRetryConfig> OccRetryConfig::Parse(const std::map<std::string, std::string>& conn_attr, std::string& error)
{
    OccRetryConfig config;

    if (const std::string* value = FindValue(conn_attr, KEY_ENABLE_OCC_RETRY)) {
        if (*value != VALUE_BOOL_TRUE && *value != VALUE_BOOL_FALSE) {
            error = InvalidValue(KEY_ENABLE_OCC_RETRY, *value, "use 0 or 1");
            return std::nullopt;
        }
        config.enabled = *value == VALUE_BOOL_TRUE;
    }

    if (const std::string* value = FindValue(conn_attr, KEY_OCC_MAX_RETRIES)) {
        const std::optional<int> retries = NumberUtils::ParseInt(*value);
        if (!retries.has_value() || *retries < 0 || *retries > MAX_RETRIES_LIMIT) {
            error = InvalidValue(KEY_OCC_MAX_RETRIES, *value,
                "use an integer from 0 to " + std::to_string(MAX_RETRIES_LIMIT));
            return std::nullopt;
        }
        config.max_retries = *retries;
    }

    if (const std::string* value = FindValue(conn_attr, KEY_OCC_BASE_DELAY_MS)) {
        const std::optional<int> delay = NumberUtils::ParseInt(*value);
        if (!delay.has_value() || *delay < 1 || *delay > DELAY_LIMIT_MS) {
            error = InvalidValue(KEY_OCC_BASE_DELAY_MS, *value,
                "use an integer from 1 to " + std::to_string(DELAY_LIMIT_MS));
            return std::nullopt;
        }
        config.base_delay_ms = *delay;
    }

    const std::string range = std::string("an integer from ") + KEY_OCC_BASE_DELAY_MS + ", "
        + std::to_string(config.base_delay_ms) + ", to " + std::to_string(DELAY_LIMIT_MS);
    const std::string* max_delay = FindValue(conn_attr, KEY_OCC_MAX_DELAY_MS);
    if (max_delay != nullptr) {
        const std::optional<int> delay = NumberUtils::ParseInt(*max_delay);
        if (!delay.has_value()) {
            error = InvalidValue(KEY_OCC_MAX_DELAY_MS, *max_delay, "use " + range);
            return std::nullopt;
        }
        config.max_delay_ms = *delay;
    }
    // Checked even when only the base is set: the 5000 ms default cap can be below it.
    if (config.max_delay_ms < config.base_delay_ms || config.max_delay_ms > DELAY_LIMIT_MS) {
        error = max_delay != nullptr
            ? InvalidValue(KEY_OCC_MAX_DELAY_MS, *max_delay, "use " + range)
            : InvalidValue(KEY_OCC_MAX_DELAY_MS, std::to_string(DEFAULT_MAX_DELAY_MS), "the default; set it to " + range);
        return std::nullopt;
    }

    if (const std::string* value = FindValue(conn_attr, KEY_OCC_JITTER_FACTOR)) {
        const std::optional<double> jitter = ParseDecimal(*value);
        if (!jitter.has_value() || std::isnan(*jitter) || *jitter < 0.0 || *jitter > 1.0) {
            error = InvalidValue(KEY_OCC_JITTER_FACTOR, *value, "use a number from 0.0 to 1.0");
            return std::nullopt;
        }
        config.jitter_factor = *jitter;
    }

    return config;
}
