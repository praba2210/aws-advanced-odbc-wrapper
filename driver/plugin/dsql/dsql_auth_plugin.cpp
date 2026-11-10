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

#include "dsql_auth_plugin.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "../../util/connection_string_keys.h"
#include "../../util/dsql_auth_provider.h"
#include "../../util/logger_wrapper.h"
#include "../../util/map_utils.h"
#include "../../util/rds_strings.h"
#include "../../util/rds_utils.h"
#include "../../util/secure_wipe.h"
#include "dsql_plugin_helper.h"

namespace {
    // psqlODBC keywords, upper-cased as in conn_attr. They aren't wrapper keys, so they reach the base driver.
    const std::string PG_KEY_SSLMODE = "SSLMODE";
    const std::string PG_KEY_SSLMODE_SHORT = "CA";
    const std::string PG_KEY_DATABASE_SHORT = "DX";
    const std::string PG_KEY_PQOPT = "PQOPT";
    const std::string PG_KEY_PQOPT_SHORT = "D5";

    const std::string SSLMODE_VERIFY_FULL = "verify-full";
    const std::string SSLMODE_VERIFY_CA = "verify-ca";
    const std::string SSLMODE_REQUIRE = "require";
    const std::string SSLMODE_PREFER = "prefer";
    const std::string SSLMODE_ALLOW = "allow";
    const std::string ROOT_CERT_KEYWORD = "sslrootcert";
    const std::string SYSTEM_ROOT_CERT = "system";
    constexpr int LIBPQ_16 = 160000;  // first libpq where sslrootcert=system means the system's roots
    // System CA bundles, each with Amazon Root CA 1, in this order.
    constexpr std::array<const char*, 4> SYSTEM_CA_BUNDLES = {
        "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Alpine, Arch
        "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL, Fedora, Amazon Linux
        "/etc/ssl/ca-bundle.pem",              // SUSE
        "/etc/ssl/cert.pem",                   // macOS
    };
    const std::string DEFAULT_PROFILE = "default";
    const std::string DEFAULT_DATABASE = "postgres";
    const std::string APPLICATION_NAME = std::string("aurora-dsql-odbc/") + DRIVER_VERSION;
    // An Aurora DSQL cluster ID: 26 letters or digits. The connectors match lowercase only; any case is accepted
    // here and lowercased.
    constexpr size_t CLUSTER_ID_LENGTH = 26;
    // The characters libpq's isspace() skips between conninfo options.
    constexpr std::string_view CONNINFO_SPACES = " \t\n\v\f\r";

    // The error for a connect that names two different hosts in SERVER and SERVERNAME (any key case; hosts compare
    // ASCII case-insensitively), or "" when there is at most one. The token is signed for one host, and the key that
    // loses would be dropped without a word: a DSN's SERVER would beat the connection string's SERVERNAME.
    std::string ServernameConflict(const std::map<std::string, std::string>& conn_attr)
    {
        const std::string* first = nullptr;
        for (const auto& [key, value] : conn_attr) {
            const std::string upper = ToAsciiUpper(key);
            if (upper != KEY_SERVER && upper != ALIAS_KEY_SERVER_1) {
                continue;
            }
            if (first == nullptr) {
                first = &value;
            } else if (AsciiLower(*first) != AsciiLower(value)) {
                return std::string("DSQL Authentication failed. ") + KEY_SERVER + " and " + ALIAS_KEY_SERVER_1
                    + " name different hosts ('" + *first + "' and '" + value + "'): set only one of them";
            }
        }
        return "";
    }

    // psqlODBC reads SERVERNAME as SERVER and the later one in its connection string wins; the wrapper writes the keys
    // in sorted order, so SERVERNAME would win. The token is signed for SERVER, so a DSQL connect keeps one host: the
    // SERVERNAME value when there is no SERVER, and nothing of SERVERNAME otherwise. DSN keys keep the case typed.
    void FoldServername(std::map<std::string, std::string>& conn_attr)
    {
        std::optional<std::string> servername;
        bool has_server = false;
        for (auto it = conn_attr.begin(); it != conn_attr.end();) {
            const std::string key = ToAsciiUpper(it->first);
            if (key == KEY_SERVER) {
                has_server = true;
            } else if (key == ALIAS_KEY_SERVER_1) {
                if (!servername) {
                    servername = std::move(it->second);
                }
                it = conn_attr.erase(it);
                continue;
            }
            ++it;
        }
        if (servername && !has_server) {
            conn_attr.emplace(KEY_SERVER, std::move(*servername));
        }
    }

    // Letters match in any case, as the endpoint pattern does; the expansion lowercases them.
    bool IsClusterId(const std::string& host)
    {
        return host.size() == CLUSTER_ID_LENGTH && std::ranges::all_of(host, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        });
    }

    // The region is one label of the cluster endpoint, so it must follow the DNS host label rule: letters, digits and
    // hyphens, starting and ending with a letter or digit, at most 63 characters. It's checked after the lowercase
    // fold. The region names of every partition pass it (us-gov-west-1, cn-north-1, us-isob-east-1, eusc-de-east-1
    // too); a list of names or the partition patterns would need a driver update for each new one. The check is only
    // about the label: the expansion's .on.aws suffix is the commercial partition's.
    bool IsRegionName(std::string_view region)
    {
        constexpr size_t MAX_LABEL_LENGTH = 63;
        const auto is_alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
        return !region.empty() && region.size() <= MAX_LABEL_LENGTH && is_alnum(region.front())
            && is_alnum(region.back()) && std::ranges::all_of(region, [&is_alnum](char c) {
                   return is_alnum(c) || c == '-';
               });
    }

    // The mode Aurora DSQL keeps for the requested one, lowercase, or "" when it's replaced. verify-ca and require
    // still encrypt, and a PrivateLink endpoint's own DNS name can't pass verify-full, so they stay. psqlODBC reads
    // the mode's first letter case-sensitively, so the result is lowercase. The names match in any case, compared in
    // ASCII: RDS_STR_UPPER follows ICU's default locale, which under tr turns the i of verify-full into U+0130.
    std::string KeptSslMode(const std::string& requested_mode)
    {
        const std::string requested_lower = AsciiLower(requested_mode);
        for (const std::string& mode : {SSLMODE_VERIFY_FULL, SSLMODE_VERIFY_CA, SSLMODE_REQUIRE}) {
            if (requested_lower == mode) {
                return mode;
            }
        }
        return "";
    }

    // Aurora DSQL rejects plaintext. Verify the server by default, as the Go, .NET, PHP, Ruby and Rust connectors do.
    // Any mode that isn't kept becomes verify-full.
    std::string ResolveSslMode(const std::string& requested_mode)
    {
        const std::string mode = KeptSslMode(requested_mode);
        if (mode == SSLMODE_REQUIRE) {
            LOG(WARNING) << "[DSQL] SSLmode=require encrypts but doesn't verify the server certificate";
        } else if (mode == SSLMODE_VERIFY_CA) {
            LOG(WARNING) << "[DSQL] SSLmode=verify-ca verifies the server certificate but not its host name";
        } else if (mode.empty() && !requested_mode.empty()) {
            LOG(WARNING) << "[DSQL] Overriding SSLmode=" << requested_mode << " with " << SSLMODE_VERIFY_FULL;
        }
        return mode.empty() ? SSLMODE_VERIFY_FULL : mode;
    }

    // psqlODBC reads SSLmode by its first byte, case-sensitively, and runs what it doesn't know as disable.
    bool RunsAsDisable(const std::string& mode)
    {
        return mode.empty() || (mode[0] != 'a' && mode[0] != 'p' && mode[0] != 'r' && mode[0] != 'v');
    }

    // The mode another auth type gets in place of one psqlODBC would run as disable, which Aurora DSQL always refuses:
    // a full mode name in another case, lowercased (psqlODBC reads Require as disable), else verify-full.
    std::string RaiseSslModeForOtherAuth(const std::string& requested_mode)
    {
        const std::string requested_lower = AsciiLower(requested_mode);
        for (const std::string& mode :
             {SSLMODE_ALLOW, SSLMODE_PREFER, SSLMODE_REQUIRE, SSLMODE_VERIFY_CA, SSLMODE_VERIFY_FULL}) {
            if (requested_lower == mode) {
                return mode;
            }
        }
        if (!requested_mode.empty()) {
            LOG(WARNING) << "[DSQL] Overriding SSLmode=" << requested_mode << " with " << SSLMODE_VERIFY_FULL
                         << ": psqlODBC would connect without TLS, which Aurora DSQL refuses";
        }
        return SSLMODE_VERIFY_FULL;
    }

    // The SSLmode the user set, under its long or short keyword, or "". The long one wins, as it does in psqlODBC,
    // which reads the sorted connection string from left to right.
    std::string RequestedSslMode(const std::map<std::string, std::string>& conn_attr)
    {
        return MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE,
                                        MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE_SHORT, ""));
    }

    // The 01S02 text for a requested SSLmode replaced with verify-full.
    std::string SslModeChangedText(const std::string& requested_mode)
    {
        return "SSLmode=" + requested_mode + " was changed to verify-full: Aurora DSQL requires TLS. Set SSLmode to "
               "verify-full, verify-ca or require.";
    }

    bool IsConninfoSpace(char c)
    {
        return CONNINFO_SPACES.find(c) != std::string_view::npos;
    }

    std::string_view TrimSpaces(std::string_view text)
    {
        const size_t first = text.find_first_not_of(CONNINFO_SPACES);
        if (first == std::string_view::npos) {
            return {};
        }
        return text.substr(first, text.find_last_not_of(CONNINFO_SPACES) - first + 1);
    }

    // The options of a libpq conninfo string ("keyword = value ..."), read as libpq's conninfo_parse reads them: a value
    // may be single-quoted, and a backslash escapes the next character. Keywords are case-sensitive, and the last of a
    // repeated keyword wins. A malformed string yields what parses; libpq reports the error itself.
    std::map<std::string, std::string> ParseConninfo(std::string_view conninfo)
    {
        std::map<std::string, std::string> options;
        size_t pos = 0;
        const auto skip_spaces = [&conninfo, &pos] {
            while (pos < conninfo.size() && IsConninfoSpace(conninfo[pos])) {
                ++pos;
            }
        };
        while (true) {
            skip_spaces();
            if (pos >= conninfo.size()) {
                break;
            }
            const size_t keyword_start = pos;
            while (pos < conninfo.size() && conninfo[pos] != '=' && !IsConninfoSpace(conninfo[pos])) {
                ++pos;
            }
            const std::string keyword(conninfo.substr(keyword_start, pos - keyword_start));
            skip_spaces();
            if (pos < conninfo.size() && conninfo[pos] == '=') {
                ++pos;
            }
            skip_spaces();
            const bool quoted = pos < conninfo.size() && conninfo[pos] == '\'';
            if (quoted) {
                ++pos;
            }
            std::string value;
            while (pos < conninfo.size()) {
                const char c = conninfo[pos++];
                if (c == '\\') {
                    if (pos < conninfo.size()) {
                        value += conninfo[pos++];
                    }
                    continue;
                }
                if (quoted ? c == '\'' : IsConninfoSpace(c)) {
                    break;
                }
                value += c;
            }
            options.insert_or_assign(keyword, value);
        }
        return options;
    }

    // The conninfo string inside pqopt's braces. A connection string value keeps the spaces typed around it.
    std::string_view UnwrapPqopt(std::string_view pqopt)
    {
        pqopt = TrimSpaces(pqopt);
        if (pqopt.size() >= 2 && pqopt.front() == '{' && pqopt.back() == '}') {
            pqopt = TrimSpaces(pqopt.substr(1, pqopt.size() - 2));
        }
        return pqopt;
    }

    // pqopt is a brace-quoted libpq conninfo string. Add each option the user didn't set; keep theirs. libpq rejects
    // sslrootcert=system with any mode weaker than verify-full, so the root certificate is only a default for
    // verify-full, and not added when root_cert is empty.
    std::string MergePqopt(const std::string& pqopt, const std::string& ssl_mode, std::string_view root_cert)
    {
        const std::string_view conninfo = UnwrapPqopt(pqopt);
        const std::map<std::string, std::string> options = ParseConninfo(conninfo);
        std::string merged(conninfo);
        const auto add_if_missing = [&options, &merged](const std::string& keyword, const std::string& value) {
            if (!options.contains(keyword)) {
                merged += (merged.empty() ? "" : " ") + keyword + "=" + value;
            }
        };
        if (ssl_mode == SSLMODE_VERIFY_FULL && !root_cert.empty()) {
            add_if_missing(ROOT_CERT_KEYWORD, std::string(root_cert));
        }
        add_if_missing("application_name", APPLICATION_NAME);
        return "{" + merged + "}";
    }

    // What pqopt gives for sslrootcert: the default the plugin adds ("system" or a system CA bundle), a value of the
    // user's, or none.
    enum class RootCertSource : std::uint8_t { NONE, DEFAULT, BUNDLE, USER };

    RootCertSource ClassifyRootCert(const std::string& pqopt)
    {
        const std::map<std::string, std::string> options = ParseConninfo(UnwrapPqopt(pqopt));
        const auto it = options.find(ROOT_CERT_KEYWORD);
        if (it == options.end()) {
            return RootCertSource::NONE;
        }
        if (it->second == SYSTEM_ROOT_CERT) {
            return RootCertSource::DEFAULT;
        }
        const bool is_bundle = std::ranges::any_of(SYSTEM_CA_BUNDLES,
            [&it](const char* bundle) { return it->second == bundle; });
        return is_bundle ? RootCertSource::BUNDLE : RootCertSource::USER;
    }

    // pqopt, or its short form D5 when only that one is set.
    const std::string& PqoptKey(const std::map<std::string, std::string>& conn_attr)
    {
        return (!conn_attr.contains(PG_KEY_PQOPT) && conn_attr.contains(PG_KEY_PQOPT_SHORT)) ? PG_KEY_PQOPT_SHORT
                                                                                             : PG_KEY_PQOPT;
    }

    // A region or profile value without the spaces, tabs and line breaks around it, so one that is only those counts
    // as unset.
    std::string TrimValue(std::string_view value)
    {
        return std::string(TrimSpaces(value));
    }

    std::string GetEnvRegion()
    {
        for (const char* name : {"AWS_REGION", "AWS_DEFAULT_REGION"}) {
            if (const char* value = std::getenv(name); value) {
                if (std::string region = TrimValue(value); !region.empty()) {
                    return region;
                }
            }
        }
        return "";
    }

    // The AWS_PROFILE key, trimmed; "" when it's unset or blank.
    std::string GetProfileKey(const std::map<std::string, std::string>& conn_attr)
    {
        return TrimValue(MapUtils::GetStringValue(conn_attr, KEY_AWS_PROFILE, ""));
    }

    // The profile the credentials come from when one is named: the AWS_PROFILE key, else the AWS_PROFILE variable,
    // both trimmed, so a blank one counts as unset. This is the SDK's profile provider order; it doesn't read
    // AWS_DEFAULT_PROFILE.
    std::string GetNamedProfile(const std::map<std::string, std::string>& conn_attr)
    {
        if (std::string profile = GetProfileKey(conn_attr); !profile.empty()) {
            return profile;
        }
        const char* value = std::getenv("AWS_PROFILE");
        return value ? TrimValue(value) : "";
    }

    // The profile the credentials come from, "default" when none is named.
    std::string GetCredentialsProfile(const std::map<std::string, std::string>& conn_attr)
    {
        const std::string profile = GetNamedProfile(conn_attr);
        return profile.empty() ? DEFAULT_PROFILE : profile;
    }

    // The profile the SDK's default chain is given when there is no AWS_PROFILE key: "", so that the chain reads the
    // AWS_PROFILE variable itself. The SDK reads the variable as is, even when it's empty, so one with blanks around
    // it is passed trimmed instead, and an empty or blank one as "default", the profile the SDK uses when the variable
    // is unset.
    std::string GetProviderProfile(const std::map<std::string, std::string>& conn_attr)
    {
        const char* value = std::getenv("AWS_PROFILE");
        if (value == nullptr || (value[0] != '\0' && TrimValue(value) == value)) {
            return "";
        }
        return GetCredentialsProfile(conn_attr);
    }

    // The connect error for missing or expired credentials, worded as the IAM plugin's. With a named profile an
    // expired AWS IAM Identity Center (SSO) session is the usual cause, so it says how to renew one.
    std::string CredentialsError(const std::map<std::string, std::string>& conn_attr)
    {
        if (const std::string key = GetProfileKey(conn_attr); !key.empty() && !DsqlAuthProvider::ProfileExists(key)) {
            return "Unable to resolve AWS credentials for DSQL authentication: the '" + std::string(KEY_AWS_PROFILE)
                + "' profile '" + key + "' isn't in the AWS config or credentials file (~/.aws/config, "
                  "~/.aws/credentials, or the files AWS_CONFIG_FILE and AWS_SHARED_CREDENTIALS_FILE name). Check the "
                  "name, or remove " + std::string(KEY_AWS_PROFILE) + " to use the default credential chain.";
        }
        const std::string profile = GetNamedProfile(conn_attr);
        if (profile.empty()) {
            return "Unable to resolve AWS credentials for DSQL authentication from the default credential chain (such "
                   "as environment variables, ~/.aws/credentials, an AWS IAM Identity Center (SSO) session, or a "
                   "container or instance role)";
        }
        return "Unable to resolve AWS credentials for DSQL authentication with the '" + std::string(KEY_AWS_PROFILE)
            + "' profile '" + profile + "'. If it is an AWS IAM Identity Center (SSO) profile, run 'aws sso login "
            "--profile " + profile + "'.";
    }

    // REGION, then the region in the DSQL endpoint being signed (IAM_HOST, then SERVER: behind a PrivateLink
    // endpoint without private DNS, SERVER is the endpoint's own name), then AWS_REGION / AWS_DEFAULT_REGION (the
    // order most DSQL connectors use), then the region of the credentials' profile. There is no default region: an
    // empty result fails the connect with "Missing required parameters: Region". Each value is trimmed, so a blank one
    // counts as unset, and the result is lowercased in ASCII: SigV4 signs the region as given, region names are
    // lowercase, and the signing region must match the one a cluster ID expands with.
    std::string ResolveDsqlRegion(const std::map<std::string, std::string>& conn_attr)
    {
        std::string region = TrimValue(MapUtils::GetStringValue(conn_attr, KEY_REGION, ""));
        for (const char* host_key : {KEY_IAM_HOST, KEY_SERVER}) {
            if (region.empty()) {
                region = TrimValue(RdsUtils::GetDsqlRegion(MapUtils::GetStringValue(conn_attr, host_key, "")));
            }
        }
        if (region.empty()) {
            region = GetEnvRegion();
        }
        if (region.empty()) {
            region = TrimValue(AuthProvider::GetRegionForProfile(GetCredentialsProfile(conn_attr)));
        }
        return AsciiLower(region);
    }

    // With the AWS_PROFILE key only that profile signs. Without it the SDK's default chain does, which reads the
    // AWS_PROFILE variable after the environment's keys.
    std::shared_ptr<AuthProvider> MakeDsqlAuthProvider(const std::map<std::string, std::string>& conn_attr,
                                                       const std::string& region)
    {
        if (const std::string key = GetProfileKey(conn_attr); !key.empty()) {
            return std::make_shared<DsqlAuthProvider>(region, key, DsqlAuthProvider::ProfileSource::KEY);
        }
        return std::make_shared<DsqlAuthProvider>(region, GetProviderProfile(conn_attr));
    }

    std::shared_ptr<AuthProvider> CreateDsqlAuthProvider(DBC* dbc, const std::shared_ptr<AuthProvider>& auth_provider)
    {
        if (auth_provider) {
            return auth_provider;
        }
        return MakeDsqlAuthProvider(dbc->conn_attr, ResolveDsqlRegion(dbc->conn_attr));
    }
}  // namespace

DsqlAuthPlugin::DsqlAuthPlugin(DBC *dbc, std::shared_ptr<BasePlugin> next_plugin)
    : DsqlAuthPlugin(dbc, next_plugin, nullptr, nullptr, nullptr) {}

DsqlAuthPlugin::DsqlAuthPlugin(DBC *dbc, std::shared_ptr<BasePlugin> next_plugin, const std::shared_ptr<AuthProvider>& auth_provider)
    : DsqlAuthPlugin(dbc, next_plugin, auth_provider, nullptr, nullptr) {}

DsqlAuthPlugin::DsqlAuthPlugin(DBC *dbc, std::shared_ptr<BasePlugin> next_plugin,
                               const std::shared_ptr<AuthProvider>& auth_provider,
                               std::shared_ptr<Dialect> dialect,
                               std::shared_ptr<OdbcHelper> odbc_helper)
    : BaseTokenAuthPlugin(dbc, next_plugin, CreateDsqlAuthProvider(dbc, auth_provider), dialect, odbc_helper)
{
    this->plugin_name_ = "DSQL";
    if (!auth_provider) {
        built_for_ = ReadProviderInputs(dbc->conn_attr);
    }
}

DsqlAuthPlugin::ProviderInputs DsqlAuthPlugin::ReadProviderInputs(const std::map<std::string, std::string>& conn_attr)
{
    ProviderInputs inputs{.key_profile = GetProfileKey(conn_attr), .env_profile = std::nullopt,
                          .region = ResolveDsqlRegion(conn_attr),
    };
    if (const char* value = std::getenv("AWS_PROFILE"); inputs.key_profile.empty() && value) {
        inputs.env_profile = value;
    }
    return inputs;
}

bool DsqlAuthPlugin::RebuildProviderIfNeeded(DBC* dbc)
{
    ProviderInputs inputs = ReadProviderInputs(dbc->conn_attr);
    if (!rebuild_provider_ && built_for_ == inputs) {
        return true;
    }
    try {
        // Built before the old one goes, so the SDK's reference count never reaches 0 in between.
        auth_provider_ = MakeDsqlAuthProvider(dbc->conn_attr, inputs.region);
    } catch (const std::exception& ex) {
        const std::string message = std::string("Unable to create the AWS credentials provider: ") + ex.what();
        LOG(ERROR) << message;
        ClearError(dbc);
        dbc->err = std::make_unique<ErrInfo>(message.c_str(), ERR_CLIENT_UNABLE_TO_ESTABLISH_CONNECTION);
        return false;
    }
    built_for_ = std::move(inputs);
    rebuild_provider_ = false;
    return true;
}

std::string DsqlAuthPlugin::ResolveRegion(DBC* dbc)
{
    return ResolveDsqlRegion(dbc->conn_attr);
}

bool DsqlAuthPlugin::EnsureCredentials(DBC* dbc, const std::string& region, std::string& out_error)
{
    if (!auth_provider_) {
        return BaseTokenAuthPlugin::EnsureCredentials(dbc, region, out_error);
    }
    // DsqlAuthProvider reads the credentials provider on every call, so this sees a renewed or expired session.
    if (auth_provider_->HasResolvedCredentials()) {
        return true;
    }
    rebuild_provider_ = true;
    out_error = CredentialsError(dbc->conn_attr);
    return false;
}

SQLRETURN DsqlAuthPlugin::Connect(
    SQLHDBC        ConnectionHandle,
    SQLHWND        WindowHandle,
    SQLTCHAR *     OutConnectionString,
    SQLSMALLINT    BufferLength,
    SQLSMALLINT *  StringLengthPtr,
    SQLUSMALLINT   DriverCompletion)
{
    DBC* dbc = static_cast<DBC*>(ConnectionHandle);
    // A reused handle keeps its first connect's plugin chain: a connect without RDS_AUTH_TYPE=DSQL gets no token.
    if (AuthProvider::AuthTypeFromString(MapUtils::GetStringValue(dbc->conn_attr, KEY_AUTH_TYPE, "")) != AuthType::DSQL) {
        return ConnectNext(ConnectionHandle, WindowHandle, OutConnectionString, BufferLength, StringLengthPtr,
                           DriverCompletion);
    }
    if (built_for_ && !RebuildProviderIfNeeded(dbc)) {
        return SQL_ERROR;
    }
    const std::string tls_warning = ApplyConnectionDefaults(dbc->conn_attr,
        DefaultRootCert(DsqlPluginHelper::BaseLibpqVersion(dbc->env ? dbc->env->driver_lib_loader : nullptr)));
    const SQLRETURN ret = BaseTokenAuthPlugin::Connect(
        ConnectionHandle, WindowHandle, OutConnectionString, BufferLength, StringLengthPtr, DriverCompletion);
    // The token was for this connect only: a reconnect signs a new one, and nothing reads PWD after the connect (the
    // output connection string was built before the chain ran). psqlODBC's own copy stays with its connection.
    if (const auto password = dbc->conn_attr.find(KEY_DB_PASSWORD); password != dbc->conn_attr.end()) {
        SecureWipeString(password->second);
        dbc->conn_attr.erase(password);
    }
    // The wrapper keeps one diagnostic record per handle, and it hides the base driver's. Report the override only
    // when the base driver has nothing to report.
    if (ret != SQL_SUCCESS || tls_warning.empty()) {
        return ret;
    }
    ClearError(dbc);
    dbc->err = std::make_unique<ErrInfo>(tls_warning.c_str(), WARN_OPTION_VALUE_CHANGED);
    return SQL_SUCCESS_WITH_INFO;
}

void DsqlAuthPlugin::OnConnectError(DBC* dbc, const std::string& sql_state, const std::string& error_message)
{
    if (const std::string_view hint = GetRootCertHint(dbc->conn_attr, error_message); !hint.empty()) {
        LOG(WARNING) << hint;
    }
}

std::string DsqlAuthPlugin::DefaultRootCert(const std::optional<int> libpq_version,
                                            const std::function<bool(const char*)>& is_trusted_file)
{
    if (!libpq_version) {
        LOG(INFO) << "[DSQL] The base driver's libpq version is unknown; sslrootcert=system is used.";
        return SYSTEM_ROOT_CERT;
    }
    if (*libpq_version >= LIBPQ_16) {
        return SYSTEM_ROOT_CERT;
    }
    for (const char* bundle : SYSTEM_CA_BUNDLES) {
        if (is_trusted_file(bundle)) {
            return bundle;
        }
    }
    return "";
}

bool DsqlAuthPlugin::IsRootOwnedFile(const char* path)
{
#ifdef _WIN32
    (void)path;
    return false;
#else
    struct stat info {};
    return stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == 0
        && (info.st_mode & static_cast<mode_t>(S_IWGRP | S_IWOTH)) == 0;
#endif
}

std::string DsqlAuthPlugin::ApplyTlsDefaults(std::map<std::string, std::string>& conn_attr, const TlsPolicy policy,
                                             const std::string_view root_cert)
{
    const std::string requested = RequestedSslMode(conn_attr);
    // Another auth type keeps a mode psqlODBC runs exactly as set, pqopt included: the connection works today, maybe
    // with the user's own root certificate (~/.postgresql/root.crt, PGSSLROOTCERT) or PGAPPNAME, which a default here
    // would override.
    if (policy == TlsPolicy::OTHER_AUTH && !RunsAsDisable(requested)) {
        return "";
    }
    const std::string ssl_mode =
        policy == TlsPolicy::DSQL_AUTH ? ResolveSslMode(requested) : RaiseSslModeForOtherAuth(requested);
    conn_attr.erase(PG_KEY_SSLMODE_SHORT);
    conn_attr.insert_or_assign(PG_KEY_SSLMODE, ssl_mode);

    const std::string& pqopt_key = PqoptKey(conn_attr);
    conn_attr.insert_or_assign(pqopt_key, MergePqopt(MapUtils::GetStringValue(conn_attr, pqopt_key, ""), ssl_mode,
                                                     root_cert));

    // A mode that was set, and wasn't verify-full in any case, was replaced.
    if (requested.empty() || ssl_mode != SSLMODE_VERIFY_FULL || AsciiLower(requested) == SSLMODE_VERIFY_FULL) {
        return "";
    }
    return SslModeChangedText(requested);
}

std::string DsqlAuthPlugin::ApplyConnectionDefaults(std::map<std::string, std::string>& conn_attr,
                                                    const std::string_view root_cert)
{
    FoldServername(conn_attr);
    std::string tls_warning = ApplyTlsDefaults(conn_attr, TlsPolicy::DSQL_AUTH, root_cert);

    // A connection string keeps USER and USERNAME as typed (only DSN entries are mapped to UID), but the token is
    // signed for UID. Move an alias to UID, so psqlODBC also gets a single user name.
    for (const char* alias : {ALIAS_KEY_USERNAME_1, ALIAS_KEY_USERNAME_2}) {
        if (const auto it = conn_attr.find(alias); it != conn_attr.end()) {
            conn_attr.try_emplace(KEY_DB_USERNAME, it->second);
            conn_attr.erase(it);
        }
    }
    conn_attr.try_emplace(KEY_DB_USERNAME, DsqlAuthProvider::ADMIN_USER);
    ApplyDatabaseDefault(conn_attr);
    return tls_warning;
}

void DsqlAuthPlugin::ApplyDatabaseDefault(std::map<std::string, std::string>& conn_attr)
{
    if (!conn_attr.contains(KEY_DATABASE) && !conn_attr.contains(PG_KEY_DATABASE_SHORT)) {
        conn_attr.emplace(KEY_DATABASE, DEFAULT_DATABASE);
    }
}

std::string_view DsqlAuthPlugin::GetRootCertHint(const std::map<std::string, std::string>& conn_attr,
                                                 const std::string& error_message)
{
    // sslrootcert=system needs libpq 16 or later: older libpq looks for a file named "system". Before 16 the plugin
    // adds a system CA bundle instead, or nothing when none is found, and libpq then names the root certificate file
    // it looked for. On Windows system means OpenSSL's default paths, not the Windows certificate store, which can
    // leave no root to verify against; there libpq can also report OpenSSL's "unregistered scheme" (DSQL user guide).
    // A host name mismatch isn't matched: the roots worked there.
    const bool root_cert_error = error_message.find("root certificate file") != std::string::npos
        || error_message.find("certificate verify failed") != std::string::npos
        || error_message.find("unregistered scheme") != std::string::npos;
    if (!root_cert_error) {
        return "";
    }
    switch (ClassifyRootCert(MapUtils::GetStringValue(conn_attr, PqoptKey(conn_attr), ""))) {
    case RootCertSource::DEFAULT:
        return "[DSQL] The server certificate couldn't be verified with sslrootcert=system, which needs libpq 16 or "
               "later and, on Windows, OpenSSL's default certificate paths. Set pqopt={sslrootcert=<path to "
               "AmazonRootCA1.pem>} instead.";
    case RootCertSource::BUNDLE:
        return "[DSQL] The server certificate couldn't be verified with the system CA bundle, which may lack Amazon "
               "Root CA 1. Set pqopt={sslrootcert=<path to AmazonRootCA1.pem>} instead.";
    case RootCertSource::NONE:
        // verify-ca and require need no root default, so a missing one isn't what failed.
        if (MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE, "") != SSLMODE_VERIFY_FULL) {
            return "";
        }
        return "[DSQL] libpq 15 or earlier has no sslrootcert=system, and no system CA bundle was found. Set "
               "pqopt={sslrootcert=<path to AmazonRootCA1.pem>}.";
    case RootCertSource::USER:
        break;
    }
    return "";
}

bool DsqlAuthPlugin::ExpandClusterId(std::map<std::string, std::string>& conn_attr, std::string& out_error)
{
    if (AuthProvider::AuthTypeFromString(MapUtils::GetStringValue(conn_attr, KEY_AUTH_TYPE, "")) != AuthType::DSQL) {
        return true;
    }
    if (std::string conflict = ServernameConflict(conn_attr); !conflict.empty()) {
        out_error = std::move(conflict);
        return false;
    }
    // This runs before the plugin chain and reads SERVER.
    FoldServername(conn_attr);
    std::string region;
    for (const char* key : {KEY_SERVER, KEY_IAM_HOST}) {
        const auto it = conn_attr.find(key);
        if (it == conn_attr.end() || !IsClusterId(it->second)) {
            continue;
        }
        if (region.empty()) {
            region = ResolveDsqlRegion(conn_attr);
        }
        if (region.empty()) {
            out_error = "DSQL Authentication failed. Region is required when host is a cluster ID (set '"
                + std::string(KEY_REGION) + "')";
            return false;
        }
        // The region becomes part of the host name. A space, a dot, a comma (libpq's host list separator) or a ';' (it
        // would end SERVER in psqlODBC's connection string) would only fail later, as a host name or option error.
        if (!IsRegionName(region)) {
            out_error = "DSQL Authentication failed. Invalid region '" + region + "' for a cluster ID host: it must "
                "be letters, digits and hyphens, start and end with a letter or digit, and be at most 63 characters "
                "long, such as us-east-1 (set '" + std::string(KEY_REGION) + "')";
            return false;
        }
        it->second = AsciiLower(it->second) + ".dsql." + region + ".on.aws";
        LOG(INFO) << "[DSQL] Expanded the cluster ID in " << key << " to " << it->second;
    }
    return true;
}

bool DsqlAuthPlugin::ExpandClusterId(DBC* dbc)
{
    std::string error;
    if (ExpandClusterId(dbc->conn_attr, error)) {
        return true;
    }
    LOG(ERROR) << "[DSQL] " << error;
    ClearError(dbc);
    dbc->err = std::make_unique<ErrInfo>(error.c_str(), ERR_CLIENT_UNABLE_TO_ESTABLISH_CONNECTION);
    return false;
}

bool DsqlAuthPlugin::SslModeRunsAsDisable(const std::map<std::string, std::string>& conn_attr)
{
    return RunsAsDisable(RequestedSslMode(conn_attr));
}

std::string DsqlAuthPlugin::GetTlsOverrideWarning(const std::map<std::string, std::string>& conn_attr)
{
    const std::string requested = RequestedSslMode(conn_attr);
    if (requested.empty() || !KeptSslMode(requested).empty()) {
        return "";
    }
    return SslModeChangedText(requested);
}
