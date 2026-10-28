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
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>

#include "../../util/connection_string_keys.h"
#include "../../util/dsql_auth_provider.h"
#include "../../util/logger_wrapper.h"
#include "../../util/map_utils.h"
#include "../../util/rds_strings.h"
#include "../../util/rds_utils.h"

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
    const std::string ROOT_CERT_KEYWORD = "sslrootcert";
    const std::string SYSTEM_ROOT_CERT = "system";
    const std::string DEFAULT_PROFILE = "default";
    const std::string DEFAULT_DATABASE = "postgres";
    const std::string APPLICATION_NAME = std::string("aurora-dsql-odbc/") + DRIVER_VERSION;
    // An Aurora DSQL cluster ID: 26 letters or digits. The connectors match lowercase only; any case is accepted
    // here and lowercased.
    constexpr size_t CLUSTER_ID_LENGTH = 26;
    // The characters libpq's isspace() skips between conninfo options.
    constexpr std::string_view CONNINFO_SPACES = " \t\n\v\f\r";

    bool ContainsAny(const std::map<std::string, std::string>& conn_attr, std::initializer_list<std::string> keys)
    {
        return std::ranges::any_of(keys, [&conn_attr](const std::string& key) { return conn_attr.contains(key); });
    }

    std::string ToLower(std::string text)
    {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // Letters match in any case, as the endpoint pattern does; the expansion lowercases them.
    bool IsClusterId(const std::string& host)
    {
        return host.size() == CLUSTER_ID_LENGTH && std::ranges::all_of(host, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        });
    }

    // The mode Aurora DSQL keeps for the requested one, lowercase, or "" when it's replaced. verify-ca and require
    // still encrypt, and a PrivateLink endpoint's own DNS name can't pass verify-full, so they stay. psqlODBC reads
    // the mode's first letter case-sensitively, so the result is lowercase.
    std::string KeptSslMode(const std::string& requested_mode)
    {
        const std::string requested_upper = RDS_STR_UPPER(requested_mode);
        for (const std::string& mode : {SSLMODE_VERIFY_FULL, SSLMODE_VERIFY_CA, SSLMODE_REQUIRE}) {
            if (requested_upper == RDS_STR_UPPER(mode)) {
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
        const auto skip_spaces = [&conninfo, &pos]() {
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
    // sslrootcert=system with any mode weaker than verify-full, so it's only a default for verify-full.
    std::string MergePqopt(const std::string& pqopt, const std::string& ssl_mode)
    {
        const std::string_view conninfo = UnwrapPqopt(pqopt);
        const std::map<std::string, std::string> options = ParseConninfo(conninfo);
        std::string merged(conninfo);
        const auto add_if_missing = [&options, &merged](const std::string& keyword, const std::string& value) {
            if (!options.contains(keyword)) {
                merged += (merged.empty() ? "" : " ") + keyword + "=" + value;
            }
        };
        if (ssl_mode == SSLMODE_VERIFY_FULL) {
            add_if_missing(ROOT_CERT_KEYWORD, SYSTEM_ROOT_CERT);
        }
        add_if_missing("application_name", APPLICATION_NAME);
        return "{" + merged + "}";
    }

    bool UsesSystemRootCert(const std::string& pqopt)
    {
        const std::map<std::string, std::string> options = ParseConninfo(UnwrapPqopt(pqopt));
        const auto it = options.find(ROOT_CERT_KEYWORD);
        return it != options.end() && it->second == SYSTEM_ROOT_CERT;
    }

    // pqopt, or its short form D5 when only that one is set.
    const std::string& PqoptKey(const std::map<std::string, std::string>& conn_attr)
    {
        return (!conn_attr.contains(PG_KEY_PQOPT) && conn_attr.contains(PG_KEY_PQOPT_SHORT)) ? PG_KEY_PQOPT_SHORT
                                                                                             : PG_KEY_PQOPT;
    }

    std::string GetEnvRegion()
    {
        for (const char* name : {"AWS_REGION", "AWS_DEFAULT_REGION"}) {
            if (const char* value = std::getenv(name); value && value[0] != '\0') {
                return value;
            }
        }
        return "";
    }

    // The profile the credentials come from when one is named: the AWS_PROFILE key, else the AWS_PROFILE variable.
    // This is the SDK's profile provider order; it doesn't read AWS_DEFAULT_PROFILE.
    std::string GetNamedProfile(const std::map<std::string, std::string>& conn_attr)
    {
        if (std::string profile = MapUtils::GetStringValue(conn_attr, KEY_AWS_PROFILE, ""); !profile.empty()) {
            return profile;
        }
        if (const char* value = std::getenv("AWS_PROFILE"); value && value[0] != '\0') {
            return value;
        }
        return "";
    }

    // The profile the credentials come from, "default" when none is named.
    std::string GetCredentialsProfile(const std::map<std::string, std::string>& conn_attr)
    {
        const std::string profile = GetNamedProfile(conn_attr);
        return profile.empty() ? DEFAULT_PROFILE : profile;
    }

    // The connect error for missing or expired credentials, worded as the IAM plugin's. With a named profile an
    // expired AWS IAM Identity Center (SSO) session is the usual cause, so it says how to renew one.
    std::string CredentialsError(const std::map<std::string, std::string>& conn_attr)
    {
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
    // empty result fails the connect with "Missing required parameters: Region". The result is lowercase: SigV4 signs
    // the region as given, and region names are lowercase.
    std::string ResolveDsqlRegion(const std::map<std::string, std::string>& conn_attr)
    {
        std::string region = MapUtils::GetStringValue(conn_attr, KEY_REGION, "");
        for (const char* host_key : {KEY_IAM_HOST, KEY_SERVER}) {
            if (region.empty()) {
                region = RdsUtils::GetDsqlRegion(MapUtils::GetStringValue(conn_attr, host_key, ""));
            }
        }
        if (region.empty()) {
            region = GetEnvRegion();
        }
        if (region.empty()) {
            region = AuthProvider::GetRegionForProfile(GetCredentialsProfile(conn_attr));
        }
        return ToLower(region);
    }

    std::shared_ptr<AuthProvider> CreateDsqlAuthProvider(DBC* dbc, const std::shared_ptr<AuthProvider>& auth_provider)
    {
        if (auth_provider) {
            return auth_provider;
        }
        const std::string profile = MapUtils::GetStringValue(dbc->conn_attr, KEY_AWS_PROFILE, "");
        return std::make_shared<DsqlAuthProvider>(ResolveDsqlRegion(dbc->conn_attr), profile);
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
    // Read before the defaults replace the requested mode.
    const std::string tls_warning = GetTlsOverrideWarning(dbc->conn_attr);
    ApplyConnectionDefaults(dbc->conn_attr);
    const SQLRETURN ret = BaseTokenAuthPlugin::Connect(
        ConnectionHandle, WindowHandle, OutConnectionString, BufferLength, StringLengthPtr, DriverCompletion);
    // The wrapper keeps one diagnostic record per handle, and it hides the base driver's. Report the override only
    // when the base driver has nothing to report (a short output buffer, for one, gets psqlODBC's 01004).
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

void DsqlAuthPlugin::ApplyConnectionDefaults(std::map<std::string, std::string>& conn_attr)
{
    const std::string ssl_mode = ResolveSslMode(MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE,
        MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE_SHORT, "")));
    conn_attr.erase(PG_KEY_SSLMODE_SHORT);
    conn_attr.insert_or_assign(PG_KEY_SSLMODE, ssl_mode);

    // A connection string keeps USER and USERNAME as typed (only DSN entries are mapped to UID), but the token is
    // signed for UID. Move an alias to UID, so psqlODBC also gets a single user name.
    for (const char* alias : {ALIAS_KEY_USERNAME_1, ALIAS_KEY_USERNAME_2}) {
        if (const auto it = conn_attr.find(alias); it != conn_attr.end()) {
            conn_attr.try_emplace(KEY_DB_USERNAME, it->second);
            conn_attr.erase(it);
        }
    }
    conn_attr.try_emplace(KEY_DB_USERNAME, DsqlAuthProvider::ADMIN_USER);
    if (!ContainsAny(conn_attr, {KEY_DATABASE, PG_KEY_DATABASE_SHORT})) {
        conn_attr.insert_or_assign(KEY_DATABASE, DEFAULT_DATABASE);
    }

    const std::string& pqopt_key = PqoptKey(conn_attr);
    conn_attr.insert_or_assign(pqopt_key, MergePqopt(MapUtils::GetStringValue(conn_attr, pqopt_key, ""), ssl_mode));
}

std::string_view DsqlAuthPlugin::GetRootCertHint(const std::map<std::string, std::string>& conn_attr,
                                                 const std::string& error_message)
{
    // sslrootcert=system needs libpq 16 or later: older libpq looks for a file named "system". On Windows it means
    // OpenSSL's default paths, not the Windows certificate store, which can leave no root to verify against; there
    // libpq can also report OpenSSL's "unregistered scheme" (DSQL user guide). A host name mismatch isn't matched: the
    // roots worked there.
    const bool root_cert_error = error_message.find("root certificate file \"system\"") != std::string::npos
        || error_message.find("certificate verify failed") != std::string::npos
        || error_message.find("unregistered scheme") != std::string::npos;
    if (!root_cert_error || !UsesSystemRootCert(MapUtils::GetStringValue(conn_attr, PqoptKey(conn_attr), ""))) {
        return "";
    }
    return "[DSQL] The server certificate couldn't be verified with sslrootcert=system, which needs libpq 16 or "
           "later and, on Windows, OpenSSL's default certificate paths. Set pqopt={sslrootcert=<path to "
           "AmazonRootCA1.pem>} instead.";
}

bool DsqlAuthPlugin::ExpandClusterId(std::map<std::string, std::string>& conn_attr, std::string& out_error)
{
    if (AuthProvider::AuthTypeFromString(MapUtils::GetStringValue(conn_attr, KEY_AUTH_TYPE, "")) != AuthType::DSQL) {
        return true;
    }
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
        it->second = ToLower(it->second) + ".dsql." + region + ".on.aws";
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

std::string DsqlAuthPlugin::GetTlsOverrideWarning(const std::map<std::string, std::string>& conn_attr)
{
    const std::string requested = MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE,
        MapUtils::GetStringValue(conn_attr, PG_KEY_SSLMODE_SHORT, ""));
    if (requested.empty() || !KeptSslMode(requested).empty()) {
        return "";
    }
    return "SSLmode=" + requested + " was changed to verify-full: Aurora DSQL requires TLS. Set SSLmode to verify-full, "
           "verify-ca or require.";
}
