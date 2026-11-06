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

#ifndef TEST_SCOPED_ENV_VARS_H
#define TEST_SCOPED_ENV_VARS_H

#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Environment variables for the tests. On Windows, _putenv_s with "" deletes the variable, so a test can't set an
// empty value there.
namespace test_env {
    inline std::optional<std::string> GetEnvVar(const char* name) {
        const char* value = std::getenv(name);
        return value ? std::optional<std::string>(value) : std::nullopt;
    }

    // Sets the variable, or unsets it for std::nullopt.
    inline void SetEnvVar(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
        _putenv_s(name, value ? value->c_str() : "");
#else
        if (value) {
            setenv(name, value->c_str(), 1);
        } else {
            unsetenv(name);
        }
#endif
    }

    inline void UnsetEnvVar(const char* name) {
        SetEnvVar(name, std::nullopt);
    }
}  // namespace test_env

// Sets or unsets (std::nullopt) environment variables for one scope, and puts the old values back, the last one
// first, when the scope ends, also when the test fails.
class ScopedEnvVars {
public:
    using Var = std::pair<const char*, std::optional<std::string>>;

    ScopedEnvVars(const char* name, const std::optional<std::string>& value)
        : ScopedEnvVars(std::initializer_list<Var>{Var(name, value)}) {}
    explicit ScopedEnvVars(std::initializer_list<Var> vars) {
        for (const auto& [name, value] : vars) {
            saved_.emplace_back(name, test_env::GetEnvVar(name));
            test_env::SetEnvVar(name, value);
        }
    }
    ~ScopedEnvVars() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            test_env::SetEnvVar(it->first, it->second);
        }
    }
    ScopedEnvVars(const ScopedEnvVars&) = delete;
    ScopedEnvVars& operator=(const ScopedEnvVars&) = delete;

private:
    std::vector<Var> saved_;
};

#endif // TEST_SCOPED_ENV_VARS_H
