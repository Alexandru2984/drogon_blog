#pragma once

#include <drogon/HttpRequest.h>

#include <cstdint>
#include <optional>
#include <string>

// Session-bound state used between the password and second-factor steps, and
// between a factor-enrolment re-authentication and its finish request. Drogon's
// Session::insert() deliberately does not overwrite an existing key; all
// updates and claims here therefore operate on the complete SessionMap under
// one lock.
namespace two_factor_session {

constexpr std::int64_t kPendingLoginTtlSeconds = std::int64_t{10} * 60;

enum class PendingLoginStatus {
    Missing,
    Valid,
    Expired,
};

struct PendingLogin {
    PendingLoginStatus status = PendingLoginStatus::Missing;
    std::optional<int> userId;
};

// Replaces any earlier pending login and its WebAuthn challenge.
void beginPendingLogin(const drogon::HttpRequestPtr& req, int userId);

// Reads and validates the pending state atomically. Expired or malformed state
// is removed together with any outstanding WebAuthn login challenge.
PendingLogin getPendingLogin(const drogon::HttpRequestPtr& req);

void clearPendingLogin(const drogon::HttpRequestPtr& req);

// Factor-management step-up state. The string keys are internal constants at
// each call site (TOTP and WebAuthn use independent authorizations).
void authorizeEnrollment(const drogon::HttpRequestPtr& req,
                         int userId,
                         const char* userKey,
                         const char* timeKey);

bool enrollmentAuthorized(const drogon::HttpRequestPtr& req,
                          int userId,
                          const char* userKey,
                          const char* timeKey,
                          std::int64_t ttlSeconds);

void clearEnrollmentAuthorization(const drogon::HttpRequestPtr& req,
                                  const char* userKey,
                                  const char* timeKey);

// Challenge replacement and consumption are atomic. A second begin replaces
// the first challenge; exactly one concurrent finish can claim it.
void storeChallenge(const drogon::HttpRequestPtr& req,
                    const char* challengeKey,
                    std::string challenge);

std::optional<std::string> claimChallenge(
    const drogon::HttpRequestPtr& req,
    const char* challengeKey);

// Starts/claims a WebAuthn enrolment ceremony while keeping the password
// step-up authorization and challenge consistent under the same session lock.
void beginEnrollmentChallenge(const drogon::HttpRequestPtr& req,
                              int userId,
                              const char* userKey,
                              const char* timeKey,
                              const char* challengeKey,
                              std::string challenge);

std::optional<std::string> claimEnrollmentChallenge(
    const drogon::HttpRequestPtr& req,
    int userId,
    const char* userKey,
    const char* timeKey,
    const char* challengeKey,
    std::int64_t ttlSeconds);

} // namespace two_factor_session
