#pragma once

#include <string>

class EmailHelper
{
public:
    // Start the background SMTP worker. Call once at app startup, after .env is loaded.
    static void start();

    // Signal the worker to drain and exit. Blocks until joined.
    static void stop();

    // Cryptographically-random URL-safe token (alphanumeric).
    static std::string generateToken(std::size_t length = 32);

    // Fire-and-forget: returns immediately, real send happens on the worker thread.
    static void sendVerificationEmail(const std::string& email,
                                      const std::string& username,
                                      const std::string& token);

    static void sendPasswordResetEmail(const std::string& email,
                                       const std::string& username,
                                       const std::string& token);

    // Sent to a user whose email was used as a registration target by someone
    // else. Prevents email-enumeration via the register endpoint without
    // leaving the legitimate owner in the dark.
    static void sendRegistrationAttemptEmail(const std::string& email,
                                             const std::string& existingUsername);

    // Sent when repeated failed logins have engaged the per-account
    // throttle. Two reasons this exists: the owner should know their
    // account is being guessed at, and throttling by account is itself a
    // way to deny someone service — an attack that locks a user out ought
    // to be visible to them rather than presenting as an unexplained
    // "too many attempts".
    static void sendLoginThrottleEmail(const std::string& email,
                                       const std::string& username,
                                       int                minutes);

    // Pending jobs in the worker queue. Exposed for metrics.
    static std::size_t queueDepth();
};
