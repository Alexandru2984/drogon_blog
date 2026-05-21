#pragma once

#include <string>
#include <vector>

// Single-use account-recovery codes for the 2FA flow. Plaintext is shown
// to the user exactly once at issuance and stored as Argon2id hashes
// thereafter — same hashing parameters as account passwords, since a
// recovery code is in effect an alternative password.
namespace recovery_codes {

constexpr int kBatchSize     = 10;
constexpr int kCodeBytes     = 5;     // ~40 bits of entropy per code,
                                      // formatted XXXX-XXXX (8 alpha chars)

// Generates `kBatchSize` plaintext codes. Caller is expected to hash
// each one via `hashOne()` before persisting, and to surface the
// plaintexts to the user *exactly once*.
std::vector<std::string> generateBatch();

// Hash a single plaintext code (Argon2id, same cost as account
// passwords). Wrapper over security::hashPassword().
std::string hashOne(const std::string& code);

// Constant-time verify the plaintext against a stored Argon2id hash.
// Wrapper over security::verifyPassword().
bool verifyOne(const std::string& storedHash, const std::string& candidate);

// Normalises a user-typed code to the canonical XXXX-XXXX upper-case
// form. Accepts inputs with extra spaces / lower case / missing dash.
std::string normalize(const std::string& raw);

} // namespace recovery_codes
