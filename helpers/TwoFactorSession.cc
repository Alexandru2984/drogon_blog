#include "TwoFactorSession.h"

#include <drogon/Session.h>

#include <any>
#include <chrono>
#include <utility>

namespace two_factor_session {
namespace {

constexpr const char* kPendingUserKey      = "pending_user_id";
constexpr const char* kPendingStartedAtKey = "pending_login_at";
constexpr const char* kLoginChallengeKey   = "pending_webauthn_challenge";

// Session storage is process-local, so a monotonic timestamp is both sufficient
// and safer than wall time: an NTP/administrator clock correction cannot extend
// an authorization window that has already elapsed.
std::int64_t monotonicSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename T>
std::optional<T> valueOf(const drogon::Session::SessionMap& values,
                         const char* key)
{
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    const auto* value = std::any_cast<T>(&it->second);
    if (!value) return std::nullopt;
    return *value;
}

bool freshTimestamp(const std::optional<std::int64_t>& timestamp,
                    std::int64_t now,
                    std::int64_t ttlSeconds)
{
    return timestamp && *timestamp <= now && *timestamp >= now - ttlSeconds;
}

void erasePendingLogin(drogon::Session::SessionMap& values)
{
    values.erase(kPendingUserKey);
    values.erase(kPendingStartedAtKey);
    values.erase(kLoginChallengeKey);
}

void eraseEnrollment(drogon::Session::SessionMap& values,
                     const char* userKey,
                     const char* timeKey)
{
    values.erase(userKey);
    values.erase(timeKey);
}

} // namespace

void beginPendingLogin(const drogon::HttpRequestPtr& req, int userId)
{
    auto startedAt = monotonicSeconds();
#if defined(BLOG_TEST_BUILD)
    if (req->getHeader("X-Test-Expire-Pending-2FA") == "1") {
        startedAt -= kPendingLoginTtlSeconds + 1;
    }
#endif

    req->session()->modify([userId, startedAt](drogon::Session::SessionMap& values) {
        values[kPendingUserKey]      = userId;
        values[kPendingStartedAtKey] = startedAt;
        values.erase(kLoginChallengeKey);
    });
}

PendingLogin getPendingLogin(const drogon::HttpRequestPtr& req)
{
    PendingLogin result;
    const auto now = monotonicSeconds();
    req->session()->modify([&result, now](drogon::Session::SessionMap& values) {
        const auto userId    = valueOf<int>(values, kPendingUserKey);
        const auto startedAt = valueOf<std::int64_t>(values, kPendingStartedAtKey);

        if (!userId && values.find(kPendingUserKey) == values.end() &&
            !startedAt && values.find(kPendingStartedAtKey) == values.end())
        {
            // A challenge without its parent login cannot authorize anything,
            // but retaining it makes the session state ambiguous for a later
            // flow.
            values.erase(kLoginChallengeKey);
            return;
        }

        result.userId = userId;
        if (userId && freshTimestamp(
                          startedAt, now, kPendingLoginTtlSeconds))
        {
            result.status = PendingLoginStatus::Valid;
            return;
        }

        result.status = PendingLoginStatus::Expired;
        erasePendingLogin(values);
    });
    return result;
}

void clearPendingLogin(const drogon::HttpRequestPtr& req)
{
    req->session()->modify([](drogon::Session::SessionMap& values) {
        erasePendingLogin(values);
    });
}

void authorizeEnrollment(const drogon::HttpRequestPtr& req,
                         int userId,
                         const char* userKey,
                         const char* timeKey)
{
    const auto authorizedAt = monotonicSeconds();
    req->session()->modify(
        [userId, userKey, timeKey, authorizedAt](drogon::Session::SessionMap& values) {
            values[userKey] = userId;
            values[timeKey] = authorizedAt;
        });
}

bool enrollmentAuthorized(const drogon::HttpRequestPtr& req,
                          int userId,
                          const char* userKey,
                          const char* timeKey,
                          std::int64_t ttlSeconds)
{
    bool authorized = false;
    const auto now = monotonicSeconds();
    req->session()->modify(
        [&](drogon::Session::SessionMap& values) {
            const auto authorizedUser = valueOf<int>(values, userKey);
            const auto authorizedAt = valueOf<std::int64_t>(values, timeKey);
            authorized = authorizedUser && *authorizedUser == userId &&
                         freshTimestamp(authorizedAt, now, ttlSeconds);
            if (!authorized) eraseEnrollment(values, userKey, timeKey);
        });
    return authorized;
}

void clearEnrollmentAuthorization(const drogon::HttpRequestPtr& req,
                                  const char* userKey,
                                  const char* timeKey)
{
    req->session()->modify(
        [userKey, timeKey](drogon::Session::SessionMap& values) {
            eraseEnrollment(values, userKey, timeKey);
        });
}

void storeChallenge(const drogon::HttpRequestPtr& req,
                    const char* challengeKey,
                    std::string challenge)
{
    req->session()->modify(
        [challengeKey, challenge = std::move(challenge)](
            drogon::Session::SessionMap& values) mutable {
            values[challengeKey] = std::move(challenge);
        });
}

std::optional<std::string> claimChallenge(
    const drogon::HttpRequestPtr& req,
    const char* challengeKey)
{
    std::optional<std::string> challenge;
    req->session()->modify(
        [&](drogon::Session::SessionMap& values) {
            challenge = valueOf<std::string>(values, challengeKey);
            values.erase(challengeKey);
        });
    return challenge;
}

void beginEnrollmentChallenge(const drogon::HttpRequestPtr& req,
                              int userId,
                              const char* userKey,
                              const char* timeKey,
                              const char* challengeKey,
                              std::string challenge)
{
    const auto authorizedAt = monotonicSeconds();
    req->session()->modify(
        [userId, userKey, timeKey, challengeKey, authorizedAt,
         challenge = std::move(challenge)](
            drogon::Session::SessionMap& values) mutable {
            values[userKey]      = userId;
            values[timeKey]      = authorizedAt;
            values[challengeKey] = std::move(challenge);
        });
}

std::optional<std::string> claimEnrollmentChallenge(
    const drogon::HttpRequestPtr& req,
    int userId,
    const char* userKey,
    const char* timeKey,
    const char* challengeKey,
    std::int64_t ttlSeconds)
{
    std::optional<std::string> challenge;
    const auto now = monotonicSeconds();
    req->session()->modify(
        [&](drogon::Session::SessionMap& values) {
            const auto authorizedUser = valueOf<int>(values, userKey);
            const auto authorizedAt = valueOf<std::int64_t>(values, timeKey);
            const auto storedChallenge =
                valueOf<std::string>(values, challengeKey);
            if (authorizedUser && *authorizedUser == userId &&
                freshTimestamp(authorizedAt, now, ttlSeconds) &&
                storedChallenge)
            {
                challenge = *storedChallenge;
            }
            values.erase(challengeKey);
            eraseEnrollment(values, userKey, timeKey);
        });
    return challenge;
}

} // namespace two_factor_session
