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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../../driver/driver.h"
#include "../../driver/error.h"
#include "../../driver/host_list_providers/host_list_provider.h"
#include "../../driver/odbcapi.h"
#include "../../driver/odbcapi_rds_helper.h"
#include "../../driver/plugin/base_plugin.h"
#include "../../driver/plugin/default_plugin.h"
#include "../../driver/util/connection_string_keys.h"
#include "../../driver/util/dsql_connection_lifetime.h"
#include "../../driver/util/dsql_connection_validator.h"
#include "../../driver/util/plugin_service.h"
#include "../../driver/util/rds_lib_loader.h"
#include "../../driver/util/rds_strings.h"

namespace {
    const std::string BASE_DRIVER_PATH = "/nonexistent/default-plugin-test/libbase.so";
    const std::string DSQL_HOST = "abcdefghijklmnopqrstuvwxyz.dsql.us-east-1.on.aws";
    const std::string OTHER_HOST = "db.example.com";
    // A password as the token plugins set it: as long as a signed token, and only letters and digits, which psqlODBC
    // writes as they are.
    const std::string TOKEN = "SignedTokenStart" + std::string(600, '7') + "SignedTokenEnd";
    // The token a later connect on the same handle gets: longer than the wrapper's own output string, so the base
    // driver's string reaches past it.
    const std::string RENEWED_TOKEN = "RenewedTokenStart" + std::string(2000, '8') + "RenewedTokenEnd";
    // A password as the Secrets Manager plugin sets it: short.
    const std::string SECRET = "ShortSecret42";
    const char* const PLUGIN_WARNING = "Option value changed by a plugin";
    // Some of the options psqlODBC lists after the connection's own keys.
    const std::string PSQLODBC_OPTIONS = ";SSLmode=verify-full;ReadOnly=0;Protocol=7.4;FakeOidIndex=0;ShowOidColumn=0;"
        "RowVersioning=0;ShowSystemTables=0;Fetch=100;UnknownSizes=0;MaxVarcharSize=255;MaxLongVarcharSize=8190;"
        "Debug=0;CommLog=0;UseDeclareFetch=0;TextAsLongVarchar=1;UnknownsAsLongVarchar=0;BoolsAsChar=1;Parse=0;"
        "LFConversion=0;UpdatableCursors=1;TrueIsMinus1=0;BI=0;ByteaAsLongVarBinary=1;UseServerSidePrepare=1";
    constexpr SQLSMALLINT BASE_DRIVER_BUFFER_LENGTH = 4096;
    constexpr SQLSMALLINT SHORT_BUFFER_LENGTH = 8;
    constexpr SQLTCHAR FILL = 'Z';

    // What the base driver's last SQLDriverConnect got and did, in the current test.
    struct BaseDriverCall {
        SQLTCHAR* out = nullptr;
        SQLSMALLINT buffer_length = 0;
        SQLSMALLINT* length_ptr = nullptr;
        size_t text_length = 0;
        bool truncated = false;
    };
    BaseDriverCall base_call;
    int connect_calls = 0;
    // The first SQLDriverConnect fails, as with a base driver built with a 4-byte SQLWCHAR that got 2-byte text.
    bool fail_first_connect = false;
    // Every SQLDriverConnect fails.
    bool fail_connects = false;
    // Runs inside each SQLDriverConnect, for example to move a clock while the connect takes time.
    std::function<void()> during_connect;
    // The base driver is psqlODBC's Unicode driver, whose SQLDriverConnectW converts through a buffer of its own.
    bool unicode_base_driver = false;
    // The base driver is psqlODBC 17.00.0005 or later, which builds only what fits the buffer length (790a1f0).
    bool cuts_to_buffer_length = false;
    int fake_wrapped_dbc = 0;

    SQLRETURN FakeAllocHandle(SQLSMALLINT /*handle_type*/, SQLHANDLE /*input_handle*/, SQLHANDLE* output_handle) {
        *output_handle = static_cast<SQLHANDLE>(&fake_wrapped_dbc);
        return SQL_SUCCESS;
    }

    // psqlODBC before 17.00.0005, such as the 16.00 distributions ship: it builds its output connection string, PWD
    // included, and copies it into the buffer it got, NUL-padded. When the string doesn't fit, it cuts the copy after
    // its last ';' and returns SQL_SUCCESS_WITH_INFO with 01004 (drvconn.c). With cuts_to_buffer_length, 17.00.0005 and
    // later: it builds at most buffer_length - 1 characters (none for 0), so it never warns, and reports that length.
    // A failed connect writes nothing. The string here is the input string and some options.
    SQLRETURN FakeDriverConnect(SQLHDBC /*connection_handle*/, SQLHWND /*window_handle*/,
                                SQLTCHAR* in_connection_string, SQLSMALLINT /*string_length_1*/,
                                SQLTCHAR* out_connection_string,
                                SQLSMALLINT buffer_length, SQLSMALLINT* string_length_2_ptr,
                                SQLUSMALLINT /*driver_completion*/) {
        ++connect_calls;
        if (during_connect) {
            during_connect();
        }
        if (fail_connects || (fail_first_connect && connect_calls == 1)) {
            return SQL_ERROR;
        }
        std::string text;
        for (const SQLTCHAR* c = in_connection_string; *c != 0; ++c) {
            text.push_back(static_cast<char>(*c));
        }
        text += PSQLODBC_OPTIONS;
        if (cuts_to_buffer_length) {
            text.resize(std::min(text.size(), buffer_length > 0 ? static_cast<size_t>(buffer_length) - 1 : size_t{0}));
        }
        base_call = {.out = out_connection_string, .buffer_length = buffer_length, .length_ptr = string_length_2_ptr,
                     .text_length = text.size(), .truncated = false};
        if (out_connection_string != nullptr) {
            const size_t capacity = buffer_length > 0 ? static_cast<size_t>(buffer_length) : 0;
            for (size_t i = 0; i < capacity; ++i) {
                out_connection_string[i] = i < text.size() ? static_cast<SQLTCHAR>(text[i]) : 0;
            }
            if (text.size() >= capacity) {
                for (size_t i = capacity; i > 0 && out_connection_string[i - 1] != ';'; --i) {
                    out_connection_string[i - 1] = 0;
                }
                base_call.truncated = true;
            }
        } else if (unicode_base_driver && buffer_length > 0) {
            // SQLDriverConnectW (odbcapiw.c) hands psqlODBC's connect code a buffer of its own of buffer_length
            // characters, also without the caller's, so the string is cut there and the warning is the same.
            base_call.truncated = text.size() >= static_cast<size_t>(buffer_length);
        }
        if (string_length_2_ptr != nullptr) {
            *string_length_2_ptr = static_cast<SQLSMALLINT>(text.size());
        }
        return base_call.truncated ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    }

#ifdef UNICODE
    // A base driver built with a 4-byte SQLWCHAR: its SQLSTATE runs past the first MAX_SQL_STATE_LEN 2-byte
    // characters, which is how DefaultPlugin tells it to connect again with 4-byte text.
    SQLRETURN FakeGetDiagRec(SQLSMALLINT /*handle_type*/, SQLHANDLE /*handle*/, SQLSMALLINT /*record_number*/,
                             SQLTCHAR* sql_state, SQLINTEGER* /*native_error*/, SQLTCHAR* /*message_text*/,
                             SQLSMALLINT /*buffer_length*/, SQLSMALLINT* /*text_length*/) {
        sql_state[MAX_SQL_STATE_LEN] = '0';
        return SQL_SUCCESS;
    }
#endif

    // The base driver: answers SQLAllocHandle and SQLDriverConnect (and SQLGetDiagRecW). Nothing is at the path;
    // RDS_InitializeConnection only compares it with BASE_DRIVER.
    class FakeBaseDriver : public RdsLibLoader {
    public:
        FakeBaseDriver() : RdsLibLoader(BASE_DRIVER_PATH) {}

        RdsPlatform::FuncHandle GetFunction(const std::string& function_name) override {
            if (function_name == RdsFuncNames::AllocHandle) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeAllocHandle);
            }
            if (function_name == RdsFuncNames::DriverConnect) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeDriverConnect);
            }
#ifdef UNICODE
            if (function_name == RdsFuncNames::GetDiagRec) {
                return reinterpret_cast<RdsPlatform::FuncHandle>(&FakeGetDiagRec);
            }
#endif
            return nullptr;
        }
    };

    // The plugin service an earlier connect would have built, without a host list provider: InitHostListProvider
    // would start one for SERVER.
    class NoTopologyPluginService : public PluginService {
    public:
        NoTopologyPluginService(const std::shared_ptr<RdsLibLoader>& lib_loader, const ENV* env,
                                const std::string& server)
            : PluginService(lib_loader, env, {{KEY_SERVER, server}, {KEY_DRIVER, BASE_DRIVER_PATH}}) {}

        std::shared_ptr<HostListProvider> GetHostListProvider() override { return host_list_provider_; }
        void InitHostListProvider() override {}

    private:
        std::shared_ptr<HostListProvider> host_list_provider_ =
            std::make_shared<HostListProvider>("default-plugin-test");
    };

    // Stands in for the plugins above DefaultPlugin: puts a token in PWD, as the token and secret plugins do, and, when
    // asked, adds its own warning, only after a connect that returned SQL_SUCCESS: the wrapper keeps one diagnostic
    // record per handle, and its own would hide the base driver's.
    class TokenPlugin : public BasePlugin {
    public:
        TokenPlugin(DBC* dbc, std::shared_ptr<BasePlugin> next_plugin) : BasePlugin(dbc, std::move(next_plugin)) {}

        SQLRETURN Connect(SQLHDBC connection_handle, SQLHWND window_handle, SQLTCHAR* out_connection_string,
                          SQLSMALLINT buffer_length, SQLSMALLINT* string_length_ptr,
                          SQLUSMALLINT driver_completion) override {
            DBC* dbc = static_cast<DBC*>(connection_handle);
            dbc->conn_attr.insert_or_assign(KEY_DB_PASSWORD, password);
            const SQLRETURN ret = ConnectNext(connection_handle, window_handle, out_connection_string, buffer_length,
                                              string_length_ptr, driver_completion);
            if (!add_warning || ret != SQL_SUCCESS) {
                return ret;
            }
            ClearError(dbc);
            dbc->err = std::make_unique<ErrInfo>(PLUGIN_WARNING, WARN_OPTION_VALUE_CHANGED);
            return SQL_SUCCESS_WITH_INFO;
        }

        bool add_warning = false;
        std::string password = TOKEN;
    };

    // A connection string as the application passes it, NUL-terminated.
    std::vector<SQLTCHAR> AppString(const std::string& text) {
#ifdef UNICODE
        const std::vector<uint16_t> utf16 = ConvertUTF8ToUTF16(text);
        return std::vector<SQLTCHAR>(utf16.begin(), utf16.end());
#else
        std::vector<SQLTCHAR> out(text.begin(), text.end());
        out.push_back(0);
        return out;
#endif
    }

    // The string at the start of a buffer, up to its terminator.
    std::string Text(const SQLTCHAR* buffer, size_t size) {
        std::string text;
        for (size_t i = 0; i < size && buffer[i] != 0; ++i) {
            text.push_back(static_cast<char>(buffer[i]));
        }
        return text;
    }

    std::string Text(const std::vector<SQLTCHAR>& buffer) {
        return Text(buffer.data(), buffer.size());
    }

    // Whether the text is anywhere in the buffer, after the terminator too.
    bool Contains(const std::vector<SQLTCHAR>& buffer, const std::string& text) {
        std::string all;
        for (const SQLTCHAR c : buffer) {
            all.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        return all.find(text) != std::string::npos;
    }

    // Whether any part of the token is anywhere in the buffer: its start, its middle or its end.
    bool HoldsToken(const std::vector<SQLTCHAR>& buffer, const std::string& token) {
        constexpr size_t PART = 16;
        return Contains(buffer, token.substr(0, PART)) || Contains(buffer, token.substr(token.size() / 2, 2 * PART))
            || Contains(buffer, token.substr(token.size() - PART));
    }

    // Whether the characters are what the caller left in the buffer.
    bool AllFill(std::vector<SQLTCHAR>::const_iterator begin, std::vector<SQLTCHAR>::const_iterator end) {
        return std::all_of(begin, end, [](SQLTCHAR c) { return c == FILL; });
    }

    // Whether the characters are what the caller left in the buffer, or zeros where the wrapper cleared it.
    bool FillOrZero(std::vector<SQLTCHAR>::const_iterator begin, std::vector<SQLTCHAR>::const_iterator end) {
        return std::all_of(begin, end, [](SQLTCHAR c) { return c == FILL || c == 0; });
    }

    std::string ConnStr(const std::string& server) {
        return "DRIVER={AWS Advanced ODBC Wrapper};BASE_DRIVER=" + BASE_DRIVER_PATH + ";SERVER=" + server
            + ";DATABASE=postgres;UID=app";
    }
}  // namespace

// DefaultPlugin gives the base driver a buffer of its own for the output connection string: the base driver's string
// carries the password or token, and the application gets the wrapper's string from RDS_SQLDriverConnect.
class DefaultPluginOutputTest : public testing::Test {
protected:
    std::shared_ptr<FakeBaseDriver> base_driver_ = std::make_shared<FakeBaseDriver>();
    ENV env_;
    std::vector<std::unique_ptr<DBC>> dbcs_;
    std::vector<std::shared_ptr<BasePlugin>> chains_;
    int fake_env_ = 0;

    void SetUp() override {
        env_.driver_lib_loader = base_driver_;
        env_.wrapped_env = static_cast<SQLHENV>(&fake_env_);  // never called through: the fake allocates the DBC
        base_call = {};
        connect_calls = 0;
        fail_first_connect = false;
        fail_connects = false;
        during_connect = nullptr;
        unicode_base_driver = false;
        cuts_to_buffer_length = false;
    }

    void TearDown() override {
        // The fake handles point at test objects; never let them outlive the test.
        for (const std::unique_ptr<DBC>& dbc : dbcs_) {
            dbc->plugin_head = nullptr;
            dbc->wrapped_dbc = SQL_NULL_HDBC;
        }
        env_.wrapped_env = SQL_NULL_HENV;
        during_connect = nullptr;
    }

    // A handle with the plugin service and the chain (TokenPlugin, then DefaultPlugin) an earlier connect to the
    // server would have built.
    DBC& NewHandle(const std::string& server, bool add_warning = false, const std::string& password = TOKEN) {
        dbcs_.push_back(std::make_unique<DBC>());
        DBC& dbc = *dbcs_.back();
        dbc.env = &env_;
        dbc.plugin_service = std::make_shared<NoTopologyPluginService>(base_driver_, &env_, server);
        const auto token_plugin = std::make_shared<TokenPlugin>(&dbc, std::make_shared<DefaultPlugin>(&dbc));
        token_plugin->add_warning = add_warning;
        token_plugin->password = password;
        chains_.push_back(token_plugin);
        dbc.plugin_head = token_plugin.get();
        return dbc;
    }

    // The chain's Connect, as RDS_SQLDriverConnect calls it once the attributes are parsed.
    static SQLRETURN ChainConnect(DBC& dbc, SQLTCHAR* out, SQLSMALLINT buffer_length, SQLSMALLINT* length) {
        dbc.conn_attr = {{KEY_SERVER, OTHER_HOST}, {KEY_DATABASE, "postgres"}, {KEY_DB_USERNAME, "app"}};
        return dbc.plugin_head->Connect(&dbc, nullptr, out, buffer_length, length, SQL_DRIVER_NOPROMPT);
    }

    // SQLDriverConnect as an application calls it.
    static SQLRETURN DriverConnect(DBC& dbc, const std::string& server, std::vector<SQLTCHAR>* out,
                                   SQLSMALLINT* length) {
        std::vector<SQLTCHAR> in = AppString(ConnStr(server));
        return RDS_SQLDriverConnect(&dbc, nullptr, in.data(), SQL_NTS, out != nullptr ? out->data() : nullptr,
            out != nullptr ? static_cast<SQLSMALLINT>(out->size()) : 0, length, SQL_DRIVER_NOPROMPT);
    }

    // The wrapper's output string for the server, from a connect with room for it, on a handle of its own.
    std::string FullOutput(const std::string& server, SQLSMALLINT* length) {
        DBC& dbc = NewHandle(server);
        std::vector<SQLTCHAR> out(BASE_DRIVER_BUFFER_LENGTH, FILL);
        EXPECT_EQ(SQL_SUCCESS, DriverConnect(dbc, server, &out, length));
        return Text(out);
    }
};

TEST_F(DefaultPluginOutputTest, Connect_BaseDriverWritesIntoItsOwnBuffer) {
    DBC& dbc = NewHandle(OTHER_HOST);
    std::vector<SQLTCHAR> out(64, FILL);
    SQLSMALLINT length = 99;
    EXPECT_EQ(SQL_SUCCESS, ChainConnect(dbc, out.data(), static_cast<SQLSMALLINT>(out.size()), &length));
    EXPECT_NE(out.data(), base_call.out);
    EXPECT_EQ(BASE_DRIVER_BUFFER_LENGTH, base_call.buffer_length);
    EXPECT_NE(&length, base_call.length_ptr);
    EXPECT_FALSE(base_call.truncated);
    // The caller's buffer holds an empty string and nothing else; its length is the caller's to set.
    EXPECT_EQ(0, out[0]);
    EXPECT_TRUE(AllFill(out.begin() + 1, out.end()));
    EXPECT_EQ(99, length);
}

// A buffer of length 0: psqlODBC reports 01004 for it on every version. The base driver gets its own buffer, and the
// caller's, which has no room, stays as it was.
TEST_F(DefaultPluginOutputTest, Connect_ZeroLengthBuffer_BaseDriverWritesIntoItsOwnBuffer) {
    DBC& dbc = NewHandle(OTHER_HOST);
    std::vector<SQLTCHAR> out(SHORT_BUFFER_LENGTH, FILL);
    SQLSMALLINT length = 99;
    EXPECT_EQ(SQL_SUCCESS, ChainConnect(dbc, out.data(), 0, &length));
    EXPECT_NE(out.data(), base_call.out);
    EXPECT_EQ(BASE_DRIVER_BUFFER_LENGTH, base_call.buffer_length);
    EXPECT_FALSE(base_call.truncated);
    EXPECT_TRUE(AllFill(out.begin(), out.end()));
    EXPECT_EQ(99, length);
}

// No buffer: the base driver gets none, and a length of the wrapper's, since its length is of its own string. The
// caller's length is RDS_SQLDriverConnect's to set.
TEST_F(DefaultPluginOutputTest, Connect_WithoutABuffer_BaseDriverGetsALengthOfItsOwn) {
    DBC& dbc = NewHandle(OTHER_HOST);
    SQLSMALLINT length = 99;
    EXPECT_EQ(SQL_SUCCESS, ChainConnect(dbc, nullptr, 0, &length));
    EXPECT_EQ(nullptr, base_call.out);
    EXPECT_EQ(0, base_call.buffer_length);
    EXPECT_NE(nullptr, base_call.length_ptr);
    EXPECT_NE(&length, base_call.length_ptr);
    EXPECT_EQ(99, length);
}

// No buffer but a length: psqlODBC's Unicode driver before 17.00.0005 would still cut its string to that length and
// warn (01004). The base driver gets no length either, so the connect returns SQL_SUCCESS and a plugin's warning
// survives.
TEST_F(DefaultPluginOutputTest, Connect_NoBufferButALength_NoTruncationWarning) {
    unicode_base_driver = true;
    DBC& dbc = NewHandle(OTHER_HOST, true);
    SQLSMALLINT length = 99;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, ChainConnect(dbc, nullptr, SHORT_BUFFER_LENGTH, &length));
    EXPECT_EQ(nullptr, base_call.out);
    EXPECT_EQ(0, base_call.buffer_length);
    EXPECT_FALSE(base_call.truncated);
    EXPECT_NE(&length, base_call.length_ptr);
    EXPECT_EQ(99, length);
    ASSERT_TRUE(dbc.err);
    EXPECT_EQ("01S02", dbc.err->sqlstate);
}

// psqlODBC's string always fits its buffer. A base driver whose string doesn't truncates it there and warns; the
// warning passes up, and the caller's buffer still gets none of the string.
TEST_F(DefaultPluginOutputTest, Connect_BaseDriverStringLongerThanItsBuffer_ItsWarningPassesUp) {
    DBC& dbc = NewHandle(OTHER_HOST);
    std::vector<SQLTCHAR> out(64, FILL);
    SQLSMALLINT length = 0;
    dbc.conn_attr = {{KEY_SERVER, OTHER_HOST}, {KEY_DATABASE, std::string(BASE_DRIVER_BUFFER_LENGTH, 'd')}};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, dbc.plugin_head->Connect(&dbc, nullptr, out.data(),
        static_cast<SQLSMALLINT>(out.size()), &length, SQL_DRIVER_NOPROMPT));
    EXPECT_TRUE(base_call.truncated);
    EXPECT_NE(out.data(), base_call.out);
    EXPECT_EQ(0, out[0]);
    EXPECT_TRUE(AllFill(out.begin() + 1, out.end()));
}

#ifdef UNICODE
// An application with a 4-byte SQLWCHAR reads 4 bytes a character: its empty string is 4 zero bytes.
TEST_F(DefaultPluginOutputTest, Connect_FourByteApplication_GetsAFourByteEmptyString) {
    constexpr uint32_t FILL_32 = 0x5A5A5A5A;
    env_.use_4_bytes_user_app.store(true);
    DBC& dbc = NewHandle(OTHER_HOST);
    std::vector<uint32_t> out(16, FILL_32);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, ChainConnect(dbc, reinterpret_cast<SQLTCHAR*>(out.data()),
        static_cast<SQLSMALLINT>(out.size()), &length));
    EXPECT_EQ(0U, out[0]);
    EXPECT_TRUE(std::all_of(out.begin() + 1, out.end(), [](uint32_t c) { return c == FILL_32; }));
}

// A base driver built with a 4-byte SQLWCHAR fails the first connect, and DefaultPlugin connects again with 4-byte
// text. The second call gets the wrapper's buffer too: that driver writes 4 bytes a character, twice what each
// character of the application's buffer holds.
TEST_F(DefaultPluginOutputTest, Connect_FourByteBaseDriverRetry_WritesIntoItsOwnBuffer) {
    fail_first_connect = true;
    DBC& dbc = NewHandle(OTHER_HOST);
    std::vector<SQLTCHAR> out(64, FILL);
    SQLSMALLINT length = 99;
    EXPECT_EQ(SQL_SUCCESS, ChainConnect(dbc, out.data(), static_cast<SQLSMALLINT>(out.size()), &length));
    EXPECT_EQ(2, connect_calls);
    EXPECT_NE(out.data(), base_call.out);
    EXPECT_EQ(BASE_DRIVER_BUFFER_LENGTH, base_call.buffer_length);
    EXPECT_NE(&length, base_call.length_ptr);
    EXPECT_EQ(0, out[0]);
    EXPECT_TRUE(AllFill(out.begin() + 1, out.end()));
    EXPECT_EQ(99, length);
}
#endif

// The wrapper's string fits but psqlODBC's, with the token, doesn't: nothing the application gets was truncated, so
// there is no truncation to report.
TEST_F(DefaultPluginOutputTest, DriverConnect_WrapperStringFits_NoTruncationReported) {
    for (const std::string& server : {DSQL_HOST, OTHER_HOST}) {
        SQLSMALLINT full_length = 0;
        const std::string full = FullOutput(server, &full_length);
        ASSERT_EQ(static_cast<size_t>(full_length), full.size()) << server;

        DBC& dbc = NewHandle(server);
        std::vector<SQLTCHAR> out(static_cast<size_t>(full_length) + 1, FILL);
        SQLSMALLINT length = 0;
        EXPECT_EQ(SQL_SUCCESS, DriverConnect(dbc, server, &out, &length)) << server;
        EXPECT_EQ(full_length, length) << server;
        EXPECT_EQ(full, Text(out)) << server;
        EXPECT_FALSE(dbc.err) << server;
        EXPECT_FALSE(base_call.truncated) << server;
    }
}

// No buffer: the length pointer still gets the length of the wrapper's string, as ODBC asks, not the base driver's:
// psqlODBC's own string holds the token, and from 17.00.0005 its length is 0 without a buffer.
TEST_F(DefaultPluginOutputTest, DriverConnect_NoBuffer_GetsTheWrapperStringLength) {
    for (const bool cuts : {false, true}) {
        cuts_to_buffer_length = cuts;
        for (const std::string& server : {DSQL_HOST, OTHER_HOST}) {
            SQLSMALLINT full_length = 0;
            FullOutput(server, &full_length);
            for (const SQLSMALLINT buffer_length : {SQLSMALLINT{0}, SHORT_BUFFER_LENGTH}) {
                DBC& dbc = NewHandle(server);
                std::vector<SQLTCHAR> in = AppString(ConnStr(server));
                SQLSMALLINT length = 0;
                EXPECT_EQ(SQL_SUCCESS, RDS_SQLDriverConnect(&dbc, nullptr, in.data(), SQL_NTS, nullptr, buffer_length,
                    &length, SQL_DRIVER_NOPROMPT)) << server << " " << cuts << " " << buffer_length;
                EXPECT_EQ(full_length, length) << server << " " << cuts << " " << buffer_length;
            }
        }
    }
}

TEST_F(DefaultPluginOutputTest, DriverConnect_TokenNeverReachesTheApplicationBuffer) {
    for (const std::string& server : {DSQL_HOST, OTHER_HOST}) {
        SQLSMALLINT full_length = 0;
        const std::string full = FullOutput(server, &full_length);

        // With a length pointer: the wrapper's string; after its terminator, what the buffer held before, or zeros
        // where the wrapper cleared it; never the base driver's string.
        DBC& with_length = NewHandle(server);
        std::vector<SQLTCHAR> out(BASE_DRIVER_BUFFER_LENGTH, FILL);
        SQLSMALLINT length = 0;
        EXPECT_EQ(SQL_SUCCESS, DriverConnect(with_length, server, &out, &length)) << server;
        EXPECT_EQ(full, Text(out)) << server;
        EXPECT_FALSE(HoldsToken(out, TOKEN)) << server;
        EXPECT_TRUE(FillOrZero(out.begin() + full_length + 1, out.end())) << server;

        // Without one: the wrapper's string, or the empty string DefaultPlugin leaves when RDS_SQLDriverConnect
        // writes none; never psqlODBC's.
        DBC& without_length = NewHandle(server);
        std::vector<SQLTCHAR> no_length_out(BASE_DRIVER_BUFFER_LENGTH, FILL);
        EXPECT_EQ(SQL_SUCCESS, DriverConnect(without_length, server, &no_length_out, nullptr)) << server;
        EXPECT_FALSE(HoldsToken(no_length_out, TOKEN)) << server;
        const std::string text = Text(no_length_out);
        EXPECT_TRUE(text.empty() || text == full) << server;
    }
}

// A handle reused after SQLDisconnect (the Windows driver manager keeps the driver's handle for the next connect) gets
// a new token on the next connect. Only the base driver's string holds the new token, so none of it may reach the
// application's buffer. The wrapper's own string is built from conn_attr before the plugins run, and conn_attr starts
// from the new connection string, so it doesn't hold the token the first connect left there either.
TEST_F(DefaultPluginOutputTest, DriverConnect_ReusedHandle_NoTokenReachesTheApplicationBuffer) {
    for (const std::string& server : {DSQL_HOST, OTHER_HOST}) {
        DBC& dbc = NewHandle(server);
        ASSERT_EQ(SQL_SUCCESS, DriverConnect(dbc, server, nullptr, nullptr)) << server;
        // What SQLDisconnect leaves: the handle isn't connected, and conn_attr keeps what the first connect put there.
        dbc.conn_status = CONN_NOT_CONNECTED;
        static_cast<TokenPlugin*>(dbc.plugin_head)->password = RENEWED_TOKEN;

        std::vector<SQLTCHAR> out(BASE_DRIVER_BUFFER_LENGTH, FILL);
        SQLSMALLINT length = 0;
        EXPECT_EQ(SQL_SUCCESS, DriverConnect(dbc, server, &out, &length)) << server;
        EXPECT_FALSE(base_call.truncated) << server;
        EXPECT_FALSE(HoldsToken(out, RENEWED_TOKEN)) << server;
        EXPECT_FALSE(HoldsToken(out, TOKEN)) << server;
        const auto terminator = static_cast<std::ptrdiff_t>(Text(out).size());
        EXPECT_TRUE(FillOrZero(out.begin() + terminator + 1, out.end())) << server;
    }
}

// A buffer too short for the wrapper's string. The Unicode build used to copy nothing into it (only the last
// character became NUL), so the buffer kept what psqlODBC wrote there: its string, cut after a ';', with a short
// password in it. The warning comes with the wrapper's own 01004 record, since psqlODBC, with a buffer of the
// wrapper's, has none.
TEST_F(DefaultPluginOutputTest, DriverConnect_ShortBuffer_NoPasswordInTheBuffer) {
    constexpr SQLSMALLINT BUFFER_LENGTH = 40;
    // psqlODBC's string starts with the keys the wrapper passes it, sorted: this much fits the buffer.
    ASSERT_LT(("DATABASE=postgres;PWD=" + SECRET + ";").size(), static_cast<size_t>(BUFFER_LENGTH));
    SQLSMALLINT full_length = 0;
    FullOutput(OTHER_HOST, &full_length);
    ASSERT_GT(full_length, BUFFER_LENGTH);

    DBC& dbc = NewHandle(OTHER_HOST, false, SECRET);
    std::vector<SQLTCHAR> out(BUFFER_LENGTH, FILL);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, DriverConnect(dbc, OTHER_HOST, &out, &length));
    EXPECT_EQ(full_length, length);
    EXPECT_FALSE(Contains(out, SECRET));

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native = 0;
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT message_length = 0;
    ASSERT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc, 1, state, &native, message, MAX_MSG_LEN,
        &message_length, false));
    EXPECT_EQ("01004", Text(state, MAX_SQL_STATE_LEN));
    EXPECT_EQ("SQLDriverConnect - The output connection string was truncated: the buffer needs "
              + std::to_string(full_length + 1) + " characters", Text(message, MAX_MSG_LEN));
}

// A plugin's warning (for one, the Aurora DSQL plugin's 01S02 for an overridden SSL mode) needs a connect that
// returned SQL_SUCCESS. A short application buffer no longer turns psqlODBC's connect into SQL_SUCCESS_WITH_INFO, and
// the wrapper's own 01004 doesn't replace the plugin's record.
TEST_F(DefaultPluginOutputTest, DriverConnect_ShortBuffer_PluginWarningSurvives) {
    SQLSMALLINT full_length = 0;
    FullOutput(DSQL_HOST, &full_length);

    DBC& dbc = NewHandle(DSQL_HOST, true);
    std::vector<SQLTCHAR> out(SHORT_BUFFER_LENGTH, FILL);
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, DriverConnect(dbc, DSQL_HOST, &out, &length));
    EXPECT_EQ(full_length, length);
    ASSERT_TRUE(dbc.err);
    EXPECT_EQ("01S02", dbc.err->sqlstate);

    SQLTCHAR state[MAX_SQL_STATE_LEN] = {0};
    SQLINTEGER native = 0;
    SQLTCHAR message[MAX_MSG_LEN] = {0};
    SQLSMALLINT message_length = 0;
    EXPECT_EQ(SQL_SUCCESS, RDS_SQLGetDiagRec(SQL_HANDLE_DBC, &dbc, 1, state, &native, message, MAX_MSG_LEN,
        &message_length, false));
    EXPECT_EQ("01S02", Text(state, MAX_SQL_STATE_LEN));
    EXPECT_EQ(PLUGIN_WARNING, Text(message, MAX_MSG_LEN));
}

// SQLConnect on a reused handle starts from its own arguments too. The DSN isn't registered, so nothing names a base
// driver and the connect fails before the plugin chain; the first connect's token, SERVER and BASE_DRIVER don't carry
// over.
TEST_F(DefaultPluginOutputTest, Connect_ReusedHandle_StartsFromItsArguments) {
    DBC& dbc = NewHandle(OTHER_HOST);
    ASSERT_EQ(SQL_SUCCESS, DriverConnect(dbc, OTHER_HOST, nullptr, nullptr));
    ASSERT_EQ(TOKEN, dbc.conn_attr.at(KEY_DB_PASSWORD));
    dbc.conn_status = CONN_NOT_CONNECTED;

    std::vector<SQLTCHAR> dsn = AppString("no-such-dsn-default-plugin-test");
    std::vector<SQLTCHAR> user = AppString("second_user");
    std::vector<SQLTCHAR> password = AppString("second_password");
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(&dbc, dsn.data(), SQL_NTS, user.data(), SQL_NTS, password.data(), SQL_NTS));
    EXPECT_EQ(1, connect_calls);
    EXPECT_FALSE(dbc.conn_attr.contains(KEY_SERVER));
    EXPECT_FALSE(dbc.conn_attr.contains(KEY_BASE_DRIVER));
    EXPECT_EQ("second_user", dbc.conn_attr.at(KEY_DB_USERNAME));
    EXPECT_EQ("second_password", dbc.conn_attr.at(KEY_DB_PASSWORD));
}

// DefaultPlugin, which every connect reaches, starts an Aurora DSQL connection's age on each successful base connect;
// SQL_ATTR_CONNECTION_DEAD and the 60-minute hint read it.
class DefaultPluginDsqlLifetimeTest : public DefaultPluginOutputTest {
protected:
    // The clock of the connection's lifetime.
    std::chrono::steady_clock::time_point now_{};

    // A handle as RDS_InitializeConnection leaves it for these attributes: is_dsql decided and, for Aurora DSQL, a
    // lifetime on the test's clock.
    DBC& NewConnection(const std::map<std::string, std::string>& conn_attr) {
        DBC& dbc = NewHandle(conn_attr.at(KEY_SERVER));
        dbc.conn_attr = conn_attr;
        dbc.is_dsql = DsqlConnectionValidator::IsDsqlTarget(conn_attr);
        if (dbc.is_dsql) {
            dbc.dsql_lifetime = std::make_shared<DsqlConnectionLifetime>([this] { return now_; });
        }
        return dbc;
    }

    // The chain's Connect, as RDS_SQLDriverConnect calls it.
    static SQLRETURN Connect(DBC& dbc) {
        return dbc.plugin_head->Connect(&dbc, nullptr, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    }
};

// The age counts from before the base connect, so it is never less than the server's; a failed connect leaves none.
TEST_F(DefaultPluginDsqlLifetimeTest, Connect_StartsTheAgeBeforeTheConnectAndClearsItOnFailure) {
    DBC& dbc = NewConnection({{KEY_SERVER, DSQL_HOST}});
    during_connect = [this] { now_ += std::chrono::seconds(5); };

    ASSERT_EQ(SQL_SUCCESS, Connect(dbc));
    const std::optional<std::chrono::steady_clock::duration> age = dbc.dsql_lifetime->Age();
    ASSERT_TRUE(age.has_value());
    EXPECT_EQ(5, std::chrono::duration_cast<std::chrono::seconds>(age.value()).count());

    fail_connects = true;
    EXPECT_EQ(SQL_ERROR, Connect(dbc));
    EXPECT_FALSE(dbc.dsql_lifetime->Age().has_value());
}

// Each connect starts the lifetime with the connection's MAX_CONNECTION_LIFETIME, which SQL_ATTR_CONNECTION_DEAD reads.
TEST_F(DefaultPluginDsqlLifetimeTest, Connect_StartsTheLifetimeWithMaxConnectionLifetime) {
    DBC& dbc = NewConnection({{KEY_SERVER, DSQL_HOST}, {KEY_MAX_CONNECTION_LIFETIME, "1800"}});
    ASSERT_EQ(SQL_SUCCESS, Connect(dbc));
    now_ += std::chrono::seconds(1800) - std::chrono::milliseconds(1);
    EXPECT_FALSE(DsqlConnectionLifetime::IsRetired(&dbc));
    now_ += std::chrono::milliseconds(1);
    EXPECT_TRUE(DsqlConnectionLifetime::IsRetired(&dbc));

    // Read again on every connect: 0 turns retirement off.
    dbc.conn_attr.insert_or_assign(KEY_MAX_CONNECTION_LIFETIME, "0");
    ASSERT_EQ(SQL_SUCCESS, Connect(dbc));
    now_ += std::chrono::hours(24);
    EXPECT_FALSE(DsqlConnectionLifetime::IsRetired(&dbc));
}

// Without MAX_CONNECTION_LIFETIME a connection is never retired, though its age is kept for the 60-minute hint.
TEST_F(DefaultPluginDsqlLifetimeTest, Connect_LifetimeUnset_NeverRetires) {
    DBC& dbc = NewConnection({{KEY_SERVER, DSQL_HOST}});
    ASSERT_EQ(SQL_SUCCESS, Connect(dbc));
    now_ += std::chrono::hours(24);
    ASSERT_TRUE(dbc.dsql_lifetime->Age().has_value());
    EXPECT_FALSE(DsqlConnectionLifetime::IsRetired(&dbc));
}

// A connect to another database creates no lifetime, and neither clears nor starts the one an earlier Aurora DSQL
// connect on the handle left.
TEST_F(DefaultPluginDsqlLifetimeTest, Connect_OtherDatabase_LeavesTheLifetimeAlone) {
    DBC& other = NewConnection({{KEY_SERVER, OTHER_HOST}});
    ASSERT_EQ(SQL_SUCCESS, Connect(other));
    EXPECT_TRUE(other.dsql_lifetime == nullptr);

    DBC& reused = NewConnection({{KEY_SERVER, OTHER_HOST}});
    reused.dsql_lifetime = std::make_shared<DsqlConnectionLifetime>([this] { return now_; });
    reused.dsql_lifetime->Start(now_, DsqlConnectionLifetime::LARGEST_MAX_LIFETIME);
    now_ += std::chrono::minutes(10);
    during_connect = [this] { now_ += std::chrono::seconds(5); };
    ASSERT_EQ(SQL_SUCCESS, Connect(reused));
    const std::optional<std::chrono::steady_clock::duration> age = reused.dsql_lifetime->Age();
    ASSERT_TRUE(age.has_value());
    EXPECT_EQ(605, std::chrono::duration_cast<std::chrono::seconds>(age.value()).count());
}

// Each connect starts as no Aurora DSQL connect, until RDS_InitializeConnection decides: one that fails before then
// gets no Aurora DSQL answers from the handle's last connect.
TEST_F(DefaultPluginDsqlLifetimeTest, Connect_FailingBeforeTheDecision_IsNotDsql) {
    DBC& dbc = NewConnection({{KEY_SERVER, DSQL_HOST}});
    ASSERT_TRUE(dbc.is_dsql);
    // A cluster ID with a region that can't be part of a host name: the expansion fails.
    std::vector<SQLTCHAR> in = AppString("DRIVER={AWS Advanced ODBC Wrapper};BASE_DRIVER=" + BASE_DRIVER_PATH
        + ";SERVER=abcdefghijklmnopqrstuvwxyz;RDS_AUTH_TYPE=DSQL;REGION=us_east_1");
    EXPECT_EQ(SQL_ERROR, RDS_SQLDriverConnect(&dbc, nullptr, in.data(), SQL_NTS, nullptr, 0, nullptr,
        SQL_DRIVER_NOPROMPT));
    EXPECT_FALSE(dbc.is_dsql);

    // SQLConnect without a DSN.
    dbc.is_dsql = true;
    EXPECT_EQ(SQL_ERROR, RDS_SQLConnect(&dbc, nullptr, 0, nullptr, 0, nullptr, 0));
    EXPECT_FALSE(dbc.is_dsql);
    EXPECT_EQ(0, connect_calls);
}
