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

#ifndef TEST_SCOPED_CTYPE_LOCALE_H
#define TEST_SCOPED_CTYPE_LOCALE_H

#include <gtest/gtest.h>

#include <clocale>
#include <cstdlib>
#include <string>
#include <string_view>

// Sets the C library's LC_CTYPE for one scope, as hosts such as Python do from the environment, and puts the old one
// back when the scope ends, also when the test fails. IsSet() is false when the locale isn't installed.
class ScopedCtypeLocale {
public:
    explicit ScopedCtypeLocale(const char* locale)
        : saved_(std::setlocale(LC_CTYPE, nullptr)), is_set_(std::setlocale(LC_CTYPE, locale) != nullptr) {}
    ~ScopedCtypeLocale() { std::setlocale(LC_CTYPE, saved_.c_str()); }
    ScopedCtypeLocale(const ScopedCtypeLocale&) = delete;
    ScopedCtypeLocale& operator=(const ScopedCtypeLocale&) = delete;

    bool IsSet() const { return is_set_; }

private:
    std::string saved_;
    bool is_set_;
};

// True when REQUIRE_TR_LOCALE=1: the Linux unit test job sets it next to locale-gen, so there the tests that need
// tr_TR.UTF-8 fail instead of skipping when it's missing.
inline bool TurkishLocaleRequired() {
    const char* value = std::getenv("REQUIRE_TR_LOCALE");
    return value != nullptr && std::string_view(value) == "1";
}

// In a test body, when tr_TR.UTF-8 couldn't be set: fails the test if TurkishLocaleRequired(), else skips it. A macro
// because GTEST_FAIL and GTEST_SKIP return from the test.
#define SKIP_OR_FAIL_WITHOUT_TURKISH_LOCALE()                                                            \
    do {                                                                                                 \
        if (TurkishLocaleRequired()) {                                                                   \
            GTEST_FAIL() << "REQUIRE_TR_LOCALE=1, but the tr_TR.UTF-8 locale isn't installed";           \
        }                                                                                                \
        GTEST_SKIP() << "no tr_TR.UTF-8 locale";                                                         \
    } while (false)

#endif // TEST_SCOPED_CTYPE_LOCALE_H
