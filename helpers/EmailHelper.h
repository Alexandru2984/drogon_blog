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

    // Pending jobs in the worker queue. Exposed for metrics.
    static std::size_t queueDepth();
};
