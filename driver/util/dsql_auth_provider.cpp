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

#include "dsql_auth_provider.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <aws/auth/credentials.h>
#include <aws/common/byte_buf.h>
#include <aws/core/Globals.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/auth/CrtCredentialsProvider.h>
#include <aws/core/auth/ProfileCredentialsProvider.h>
#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/auth/STSCredentialsProvider.h>
#include <aws/core/auth/signer/AWSAuthV4Signer.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/UserAgent.h>
#include <aws/core/config/AWSConfigFileProfileConfigLoader.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/stream/ResponseStream.h>
#include <aws/crt/Allocator.h>
#include <aws/crt/auth/Credentials.h>
#include <aws/crt/io/Bootstrap.h>
#include <aws/crt/io/TlsOptions.h>

#include "aws_sdk_helper.h"
#include "logger_wrapper.h"
#include "rds_strings.h"
#include "secure_wipe.h"

namespace {
    constexpr const char* SIGNING_SERVICE = "dsql";
    constexpr std::string_view HTTP_SCHEME = "http://";

    // Why these credentials can't sign a DSQL token, or "" when they can. The C++ SigV4 presigner skips signing when
    // either key is empty and returns an unsigned URL, and it signs with expired credentials; the server rejects both
    // at login.
    std::string_view CredentialsProblem(const Aws::Auth::AWSCredentials& credentials)
    {
        if (credentials.GetAWSAccessKeyId().empty() || credentials.GetAWSSecretKey().empty()) {
            return "No AWS credentials available for Aurora DSQL token generation";
        }
        // CRT-backed providers (profile, process) report "never expires" as UINT64_MAX seconds, which overflows the
        // SDK's DateTime and reads as at or before 1970. Only an expiration after the epoch is a real one.
        if (credentials.GetExpiration().Millis() > 0 && credentials.IsExpired()) {
            return "AWS credentials for Aurora DSQL token generation have expired. If they come from AWS IAM Identity "
                   "Center (SSO), run 'aws sso login'";
        }
        return "";
    }

    // The token with each '%' written again as %25, as AuthProvider::ExtraUrlEncodeString writes it (EXTRA_URL_ENCODE),
    // into out, which is reserved at the final size first: that function grows its copy, and the shorter buffers it
    // frees aren't cleared.
    void ExtraUrlEncodeInto(std::string_view token, std::string& out)
    {
        const auto percents = static_cast<size_t>(std::ranges::count(token, '%'));
        out.reserve(token.size() + (2 * percents));
        for (const char c : token) {
            if (c == '%') {
                out += "%25";
            } else {
                out += c;
            }
        }
    }

    // The SDK's chain class has a protected constructor and AddProvider.
    class ProfileOnlyChain final : public Aws::Auth::AWSCredentialsProviderChain {
    public:
        explicit ProfileOnlyChain(const std::vector<std::shared_ptr<Aws::Auth::AWSCredentialsProvider>>& providers)
        {
            for (const auto& provider : providers) {
                if (provider) {
                    AddProvider(provider);
                }
            }
        }
    };

    // The profile in the config file (with the "profile " prefix) or the credentials file, read now: the files can
    // change after InitAPI, which the SDK's cached copy doesn't see.
    std::optional<Aws::Config::Profile> FindProfile(const std::string& name, bool config_file)
    {
        Aws::Config::AWSConfigFileProfileConfigLoader loader(
            config_file ? Aws::Auth::GetConfigProfileFilename()
                        : Aws::Auth::ProfileCredentialsProvider::GetCredentialsProfileFilename(), config_file);
        if (!loader.Load()) {
            return std::nullopt;
        }
        const auto it = loader.GetProfiles().find(Aws::String(name.data(), name.size()));
        return it == loader.GetProfiles().end() ? std::nullopt : std::optional(it->second);
    }
}  // namespace

// The profile as the config file and the credentials file have it, so a build reads each file once.
struct DsqlAuthProvider::ProfileFiles {
    std::optional<Aws::Config::Profile> config;
    std::optional<Aws::Config::Profile> credentials;

    static ProfileFiles Find(const std::string& name)
    {
        return {.config = FindProfile(name, true), .credentials = FindProfile(name, false)};
    }
    [[nodiscard]] bool HasProfile() const
    {
        return config.has_value() || credentials.has_value();
    }
};

DsqlAuthProvider::DsqlAuthProvider(const std::string &region, const std::string &profile, ProfileSource source) {
    // The base default constructor skips Init, but the base destructor always calls Shutdown.
    AwsSdkHelper::Init();
    if (source == ProfileSource::KEY) {
        if (const ProfileFiles files = ProfileFiles::Find(profile); files.HasProfile()) {
            source_ = BuildProfileOnlyChain(profile, region, files);
        } else {
            // Nothing to sign with, so the connect fails and names the profile, instead of signing with the
            // environment, a container or the instance role.
            source_ =
                std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(Aws::Auth::AWSCredentials{});
        }
    } else if (profile.empty()) {
        source_ = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>();
    } else {
        Aws::Client::ClientConfiguration::CredentialProviderConfiguration credential_config{};
        credential_config.profile = profile;
        source_ = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>(credential_config);
    }
    // Keep the provider, not a snapshot, so expiring SSO and assume-role credentials refresh on their own. It is
    // read once per connect, not here: the failure is reported by the connect.
}

DsqlAuthProvider::DsqlAuthProvider(const std::string & /*region*/, const Aws::Auth::AWSCredentials& credentials) {
    AwsSdkHelper::Init();
    source_ = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(credentials);
}

DsqlAuthProvider::DsqlAuthProvider(const std::string & /*region*/,
                                   std::shared_ptr<Aws::Auth::AWSCredentialsProvider> source) {
    AwsSdkHelper::Init();
    source_ = std::move(source);
}

std::pair<std::string, bool> DsqlAuthProvider::GetToken(
    const std::string &server,
    const std::string &region,
    const std::string & /*port*/,
    const std::string &username,
    bool /*use_cache*/,
    bool extra_url_encode,
    std::chrono::milliseconds time_to_expire_ms)
{
    // Check before signing, so an unusable token is never sent. Credentials can disappear or expire after
    // construction, for example when an SSO session ends.
    // The credentials the connect's check read, else one read here. Either way this is the connect's only read.
    const Aws::Auth::AWSCredentials credentials = TakeCheckedCredentials();
    if (const std::string_view problem = CredentialsProblem(credentials); !problem.empty()) {
        LOG(ERROR) << problem;
        return {"", false};
    }

    // The signature covers the host; DNS is case-insensitive, so sign the lowercase form.
    const std::string host = AsciiLower(server);
    // TOKEN_EXPIRATION=0 means the default lifetime, as in the Go connector: X-Amz-Expires=0 would make a token that
    // has already expired.
    std::chrono::seconds lifetime = std::chrono::duration_cast<std::chrono::seconds>(time_to_expire_ms);
    if (lifetime <= std::chrono::seconds::zero()) {
        lifetime = std::chrono::duration_cast<std::chrono::seconds>(DEFAULT_EXPIRATION_MS);
    }
    const bool is_admin = username == ADMIN_USER;
    // Explicit (data, size) copies here and for the token below: Aws::String is std::string only when the SDK is built
    // without custom memory management. Not .c_str(): clang-tidy flags it as redundant even where that build needs it.
    const Aws::String aws_host(host.data(), host.size());
    const Aws::String aws_region(region.data(), region.size());
    if (aws_host.empty() || aws_region.empty()) {
        LOG(ERROR) << "Failed to generate an Aurora DSQL token: the host and the region must be non-empty";
        return {"", false};
    }
    // A presigned GET of http://<host>?Action=DbConnect[Admin], without the scheme, as the SDK's
    // DSQLClient::GenerateDBConnect[Admin]AuthToken makes it. The signer is built without a provider, which it would
    // read when built, and signs with the credentials checked above.
    Aws::Http::URI uri(aws_host);
    uri.AddQueryStringParameter("Action", is_admin ? "DbConnectAdmin" : "DbConnect");
    const std::shared_ptr<Aws::Http::HttpRequest> request = Aws::Http::CreateHttpRequest(
        uri, Aws::Http::HttpMethod::HTTP_GET, Aws::Utils::Stream::DefaultResponseStreamFactoryMethod);
    const Aws::Client::AWSAuthV4Signer signer(nullptr, SIGNING_SERVICE, aws_region);
    if (!signer.PresignRequest(*request, credentials, aws_region.c_str(), SIGNING_SERVICE, lifetime.count())) {
        LOG(ERROR) << "Failed to generate an Aurora DSQL token";
        return {"", false};
    }
    // The signed URL and the token made from it are cleared on every return; the caller gets its own copy, in the
    // returned pair. The SDK's internal copies aren't cleared.
    Aws::String url = request->GetURIString();
    const ScopedWipe wipe_url(url);
    std::string_view signed_url(url.data(), url.size());
    if (signed_url.starts_with(HTTP_SCHEME)) {
        signed_url.remove_prefix(HTTP_SCHEME.size());
    }
    std::string token;
    const ScopedWipe wipe_token(token);  // reads the size when it wipes
    if (extra_url_encode) {
        ExtraUrlEncodeInto(signed_url, token);
    } else {
        token.assign(signed_url);
    }
    LOG(INFO) << "Generated new Aurora DSQL " << (is_admin ? "admin " : "") << "token length: " << token.length();
    return {token, false};
}

bool DsqlAuthProvider::ProfileExists(const std::string& profile)
{
    return FindProfile(profile, true) || FindProfile(profile, false);
}

std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> DsqlAuthProvider::MakeProfileOnlyChain(
    const std::string& profile, const std::string& region)
{
    return BuildProfileOnlyChain(profile, region, ProfileFiles::Find(profile));
}

std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> DsqlAuthProvider::MakeLoginCrtProvider(
    const std::string& profile, const std::string& region)
{
    // The login provider reads only the config file.
    const ProfileFiles files{.config = FindProfile(profile, true), .credentials = std::nullopt};
    return BuildLoginCrtProvider(profile, region, files);
}

std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> DsqlAuthProvider::BuildProfileOnlyChain(
    const std::string& profile, const std::string& region, const ProfileFiles& files)
{
    Aws::Client::ClientConfiguration::CredentialProviderConfiguration config{};
    config.profile = Aws::String(profile.data(), profile.size());
    config.region = Aws::String(region.data(), region.size());  // STS region for web identity
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> web_identity;
    // role_arn + web_identity_token_file without source_profile/credential_source: the CRT profile provider can't
    // read it, and the SDK's web identity provider would read AWS_ROLE_ARN / AWS_WEB_IDENTITY_TOKEN_FILE instead.
    for (const std::optional<Aws::Config::Profile>* file : {&files.config, &files.credentials}) {
        const std::optional<Aws::Config::Profile>& p = *file;
        if (p && !p->GetRoleArn().empty() && !p->GetValue("web_identity_token_file").empty()
            && p->GetSourceProfile().empty() && p->GetValue("credential_source").empty()) {
            config.stsCredentialsProviderConfig.roleArn = p->GetRoleArn();
            config.stsCredentialsProviderConfig.tokenFilePath = p->GetValue("web_identity_token_file");
            config.stsCredentialsProviderConfig.sessionName = p->GetValue("role_session_name");  // "" -> a UUID
            if (!p->GetRegion().empty()) {
                config.region = p->GetRegion();
            }
            // The SDK reads config in this constructor (CrtCredentialsProvider calls the factory there).
            web_identity = std::make_shared<Aws::Auth::STSAssumeRoleWebIdentityCredentialsProvider>(config);
            break;
        }
    }
    // `aws login`. The SDK's LoginCredentialsProvider doesn't pass the profile on, so the CRT would read the session
    // and endpoint settings of the profile the AWS_PROFILE variable names, or of [default]. The CRT provider is built
    // here for this profile instead, and wrapped as the SDK wraps its own.
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> login;
    if (std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> login_crt =
            BuildLoginCrtProvider(profile, region, files)) {
        // The SDK calls the factory once, in the constructor.
        login = std::make_shared<Aws::Auth::CrtCredentialsProvider>(
            [crt = std::move(login_crt)]() mutable { return std::move(crt); },
            config.loginCredentialProviderConfig.retrieveCredentialsFutureTimeout,
            Aws::Client::UserAgentFeature::CREDENTIALS_LOGIN, "LoginCredentialsProvider");
    }
    return std::make_shared<ProfileOnlyChain>(std::vector<std::shared_ptr<Aws::Auth::AWSCredentialsProvider>>{
        // Access keys, role_arn with source_profile or credential_source (Ec2InstanceMetadata, EcsContainer,
        // Environment: named in the profile, so allowed), credential_process.
        std::make_shared<Aws::Auth::ProfileCredentialsProvider>(config),
        web_identity,
        std::make_shared<Aws::Auth::SSOCredentialsProvider>(config.profile),
        login,
    });
}

std::shared_ptr<Aws::Crt::Auth::ICredentialsProvider> DsqlAuthProvider::BuildLoginCrtProvider(
    const std::string& profile, const std::string& region, const ProfileFiles& files)
{
    const Aws::Crt::Io::ClientBootstrap* bootstrap = Aws::GetDefaultClientBootstrap();
    const Aws::Crt::Io::TlsConnectionOptions* tls = Aws::GetDefaultTlsConnectionOptions();
    // With no profile name the CRT would pick one itself. When the default TLS context failed to build (no CA store),
    // the default connection options are there but were never initialized, and their ctx is garbage or null: the
    // connect fails with 28000 (no login provider) instead of crashing in the CRT.
    if (profile.empty() || bootstrap == nullptr || !*bootstrap || tls == nullptr || !*tls) {
        return nullptr;
    }
    // The CRT builds the sign-in endpoint from the region it is given, so one is always given: the profile's in the
    // config file, which the CRT would pick, else the DSQL region.
    const std::optional<Aws::Config::Profile>& config_profile = files.config;
    const std::string login_region = config_profile && !config_profile->GetRegion().empty()
        ? std::string(config_profile->GetRegion().data(), config_profile->GetRegion().size())
        : region;
    // The CRT copies these strings while it builds the provider. The login session comes from the profile.
    aws_credentials_provider_login_options options{};
    options.profile_name_override = aws_byte_cursor_from_array(profile.data(), profile.size());
    options.login_region = aws_byte_cursor_from_array(login_region.data(), login_region.size());
    options.bootstrap = bootstrap->GetUnderlyingHandle();
    options.tls_ctx = tls->GetUnderlyingHandle()->ctx;
    aws_credentials_provider* const provider = aws_credentials_provider_new_login(Aws::Crt::ApiAllocator(), &options);
    if (provider == nullptr) {
        return nullptr;
    }
    // Takes the reference and releases it when destroyed.
    return std::make_shared<Aws::Crt::Auth::CredentialsProvider>(provider, Aws::Crt::ApiAllocator());
}

bool DsqlAuthProvider::HasResolvedCredentials() const
{
    Aws::Auth::AWSCredentials credentials = source_->GetAWSCredentials();
    const bool usable = CredentialsProblem(credentials).empty();
    // Only a usable read is kept for the GetToken that follows; a failed check leaves nothing for the next token.
    const std::lock_guard lock(pending_mutex_);
    pending_ = usable ? std::optional(std::move(credentials)) : std::nullopt;
    return usable;
}

Aws::Auth::AWSCredentials DsqlAuthProvider::TakeCheckedCredentials() const
{
    {
        const std::lock_guard lock(pending_mutex_);
        if (pending_) {
            Aws::Auth::AWSCredentials credentials = std::move(*pending_);
            pending_.reset();
            return credentials;
        }
    }
    return source_->GetAWSCredentials();
}
