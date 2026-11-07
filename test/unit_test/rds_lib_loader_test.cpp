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

#include <cstdlib>

#include <gtest/gtest.h>

#include "../../driver/odbcapi.h"
#include "../../driver/util/odbc_dsn_helper.h"
#include "../../driver/util/rds_lib_loader.h"

namespace {
const char* nonexistent_module = "/nonexistent/path/to/no-such-driver-module.so";

#ifndef _WIN32
const bool ODBCSYSINI_SET = [] {
    setenv("ODBCSYSINI", AWS_PROFILE_TEST_RESOURCES_DIR, 1);
    return true;
}();
#endif
} // namespace

TEST(RdsLibLoaderTest, NonexistentModuleIsNotLoaded) {
    RdsLibLoader loader(nonexistent_module);
    EXPECT_FALSE(loader.IsLoaded());
    EXPECT_FALSE(loader.GetLoadError().empty());
    EXPECT_EQ(nonexistent_module, loader.GetDriverPath());
}

TEST(RdsLibLoaderTest, GetFunctionOnUnloadedModuleReturnsNull) {
    RdsLibLoader loader(nonexistent_module);
    EXPECT_EQ(nullptr, loader.GetFunction("SQLDriverConnect"));
    EXPECT_EQ(nullptr, loader.GetFunction("SQLFreeHandle"));
}

TEST(RdsLibLoaderTest, CallFunctionOnUnloadedModuleFailsWithoutInvoking) {
    RdsLibLoader loader(nonexistent_module);
    SQLHENV env_handle = nullptr;
    const RdsLibResult res = loader.CallFunction<RDS_FP_SQLAllocHandle>(
        "SQLAllocHandle", static_cast<SQLSMALLINT>(SQL_HANDLE_ENV), nullptr, &env_handle);
    EXPECT_FALSE(res.fn_load_success);
    EXPECT_EQ(SQL_ERROR, res.fn_result);
    EXPECT_EQ(nullptr, env_handle);
}

#ifdef _WIN32
// The load error is "error <code>: <the system's text>". A smoke test: the shape is the same whichever FormatMessage
// gives the text, and an English system's text is ASCII.
TEST(RdsLibLoaderTest, LoadErrorHasTheSystemMessage) {
    RdsLibLoader loader(nonexistent_module);
    const std::string error = loader.GetLoadError();
    EXPECT_EQ(0U, error.find("error "));
    const size_t text = error.find(": ");
    ASSERT_NE(std::string::npos, text);
    EXPECT_LT(text + 2, error.size());
}
#endif

#if defined(_WIN32) && defined(UNICODE)
// The Unicode driver's load error carries the system's text in UTF-8: in a French system's text each U+00E9 is C3 A9.
// The text came from FormatMessageA in the ANSI code page, where it is the byte E9, which a W caller got as U+FFFD.
TEST(RdsLibLoaderTest, LoadErrorTextIsUtf8) {
    EXPECT_EQ("error 126: Le module sp\xC3\xA9" "cifi\xC3\xA9 est introuvable.",
        RdsPlatform::LoadErrorMessage(126, L"Le module sp\u00e9cifi\u00e9 est introuvable.\r\n"));
    EXPECT_EQ("error 5", RdsPlatform::LoadErrorMessage(5, nullptr));
}

// The loader's error is this system's own UTF-16 text for the code, converted: the same as converting FormatMessageW's
// text here. On a system whose text isn't ASCII this also tells it from FormatMessageA's.
TEST(RdsLibLoaderTest, LoadErrorIsTheSystemUtf16TextConverted) {
    RdsLibLoader loader(nonexistent_module);
    const std::string error = loader.GetLoadError();
    ASSERT_EQ(0U, error.find("error "));
    const DWORD code = std::stoul(error.substr(6));
    wchar_t text[512] = {0};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        text, sizeof(text) / sizeof(text[0]), nullptr);
    EXPECT_EQ(RdsPlatform::LoadErrorMessage(code, length > 0 ? text : nullptr), error);
}
#endif

TEST(RdsLibLoaderTest, DefaultConstructedLoaderIsNotLoaded) {
    RdsLibLoader loader;
    EXPECT_FALSE(loader.IsLoaded());
    EXPECT_EQ(nullptr, loader.GetFunction("SQLDriverConnect"));
}

TEST(ResolveDriverNameTest, UnregisteredNameReturnsEmpty) {
    EXPECT_TRUE(OdbcDsnHelper::ResolveDriverName("DefinitelyNotARegisteredOdbcDriver123").empty());
}

#ifndef _WIN32
// On Unix the odbcinst.ini location can be controlled by environment variable
// Windows Driver information lives in registry, no environment variable to control via test fixture
TEST(ResolveDriverNameTest, RegisteredNameResolvesToDriverPath) {
    EXPECT_EQ("/tmp/unit-test-fake-driver.so", OdbcDsnHelper::ResolveDriverName("UnitTestFakeDriver"));
}
#endif
