/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */


#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/config/AWSProfileConfigLoader.h>
#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/platform/FileSystem.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/FileSystemUtils.h>
#include <aws/core/client/SpecifiedRetryableErrorsRetryStrategy.h>
#include <aws/core/utils/UUID.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/json/JsonSerializer.h>

using namespace Aws::Utils;
using namespace Aws::Utils::Logging;
using namespace Aws::Auth;
using namespace Aws::Internal;
using namespace Aws::FileSystem;
using namespace Aws::Client;
using Aws::Utils::Threading::ReaderLockGuard;


static const char SSO_CREDENTIALS_PROVIDER_LOG_TAG[] = "SSOCredentialsProvider";

SSOCredentialsProvider::SSOCredentialsProvider() : m_profileToUse(GetConfigProfileName())
{
    AWS_LOGSTREAM_INFO(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Setting sso credentials provider to read config from " <<  m_profileToUse);
}

SSOCredentialsProvider::SSOCredentialsProvider(const Aws::String& profile) : m_profileToUse(profile)
{
    AWS_LOGSTREAM_INFO(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Setting sso credentials provider to read config from " <<  m_profileToUse);
}

AWSCredentials SSOCredentialsProvider::GetAWSCredentials()
{
    RefreshIfExpired();
    ReaderLockGuard guard(m_reloadLock);
    return m_credentials;
}

void SSOCredentialsProvider::Reload()
{
    auto profile = Aws::Config::GetCachedConfigProfile(m_profileToUse);

    Aws::String hashedStartUrl = Aws::Utils::HashingUtils::HexEncode(Aws::Utils::HashingUtils::CalculateSHA1(profile.GetSsoStartUrl()));
    auto profileDirectory = ProfileConfigFileAWSCredentialsProvider::GetProfileDirectory();
    Aws::StringStream ssToken;
    ssToken << profileDirectory;
    ssToken << PATH_DELIM << "sso"  << PATH_DELIM << "cache" << PATH_DELIM << hashedStartUrl << ".json";
    auto ssoTokenPath = ssToken.str();
    AWS_LOGSTREAM_DEBUG(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Loading token from: " << ssoTokenPath)
    Aws::String accessToken = LoadAccessTokenFile(ssoTokenPath);
    if (m_expiresAt < Aws::Utils::DateTime::Now()) {
        AWS_LOGSTREAM_ERROR(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Cached Token expired at " << m_expiresAt.ToGmtString(DateFormat::ISO_8601));
        return;
    }
    SSOCredentialsClient::SSOGetRoleCredentialsRequest request;
    request.m_ssoAccountId = profile.GetSsoAccountId();
    request.m_ssoRoleName = profile.GetSsoRoleName();
    request.m_accessToken = accessToken;

    Aws::Client::ClientConfiguration config;
    config.scheme = Aws::Http::Scheme::HTTPS;
    config.region = profile.GetSsoRegion();
    AWS_LOGSTREAM_DEBUG(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Passing config to client for region: " << m_ssoRegion);

    Aws::Vector<Aws::String> retryableErrors;
    retryableErrors.push_back("TooManyRequestsException");

    config.retryStrategy = Aws::MakeShared<SpecifiedRetryableErrorsRetryStrategy>(SSO_CREDENTIALS_PROVIDER_LOG_TAG, retryableErrors, 3/*maxRetries*/);
    m_client = Aws::MakeUnique<Aws::Internal::SSOCredentialsClient>(SSO_CREDENTIALS_PROVIDER_LOG_TAG, config);

    AWS_LOGSTREAM_TRACE(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Requesting credentials with AWS_ACCESS_KEY: " << m_ssoAccountId);
    auto result = m_client->GetSSOCredentials(request);
    AWS_LOGSTREAM_TRACE(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Successfully retrieved credentials with AWS_ACCESS_KEY: " << result.creds.GetAWSAccessKeyId());

    m_credentials = result.creds;
}

void SSOCredentialsProvider::RefreshIfExpired()
{
    ReaderLockGuard guard(m_reloadLock);
    if (!m_credentials.IsExpiredOrEmpty())
    {
        return;
    }

    guard.UpgradeToWriterLock();
    if (!m_credentials.IsExpiredOrEmpty()) // double-checked lock to avoid refreshing twice
    {
        return;
    }

    Reload();
}

Aws::String SSOCredentialsProvider::LoadAccessTokenFile(const Aws::String& ssoAccessTokenPath)
{
    AWS_LOGSTREAM_DEBUG(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Preparing to load token from: " << ssoAccessTokenPath);

    Aws::IFStream inputFile(ssoAccessTokenPath.c_str());
    if(inputFile)
    {
        AWS_LOGSTREAM_DEBUG(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Reading content from token file: " << ssoAccessTokenPath);

        Json::JsonValue tokenDoc(inputFile);
        if (!tokenDoc.WasParseSuccessful())
        {
            AWS_LOGSTREAM_ERROR(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Failed to parse token file: " << ssoAccessTokenPath);
            return "";
        }
        Utils::Json::JsonView tokenView(tokenDoc);
        Aws::String tmpAccessToken, expirationStr;
        tmpAccessToken = tokenView.GetString("accessToken");
        expirationStr = tokenView.GetString("expiresAt");
        DateTime expiration(expirationStr, DateFormat::ISO_8601);

        AWS_LOGSTREAM_TRACE(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Token cache file contains accessToken [" << tmpAccessToken << "], expiration [" << expirationStr << "]");

        if (tmpAccessToken.empty() || !expiration.WasParseSuccessful()) {
            AWS_LOG_ERROR(SSO_CREDENTIALS_PROVIDER_LOG_TAG, R"(The SSO session associated with this profile has expired or is otherwise invalid. To refresh this SSO session run aws sso login with the corresponding profile.)");
            AWS_LOGSTREAM_TRACE(SSO_CREDENTIALS_PROVIDER_LOG_TAG, "Token cache file failed because "
                                 << (tmpAccessToken.empty()?"AccessToken was empty ":"")
                                 << (!expiration.WasParseSuccessful()? "failed to parse expiration":""));
            return "";
        }
        m_expiresAt = expiration;
        return  tmpAccessToken;
    }
    else
    {
        AWS_LOGSTREAM_INFO(SSO_CREDENTIALS_PROVIDER_LOG_TAG,"Unable to open token file on path: " << ssoAccessTokenPath);
        return "";
    }
}