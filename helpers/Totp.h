#pragma once

#include <cstdint>
#include <string>

// RFC 6238 TOTP (HMAC-SHA1, 30-second time step, 6-digit codes).
//
// Implemented from scratch on top of OpenSSL's HMAC primitive rather than
// pulling in libcotp / liboath — the algorithm is ~30 lines and the test
// vectors from RFC 6238 are the same ones every implementation is judged
// against, so the right way to demonstrate competence is to write them.
namespace totp {

// 160-bit secret (20 bytes), Base32-encoded per RFC 4648 (no padding).
// 160 bits is what Google Authenticator / Authy / 1Password expect; tools
// will reject shorter keys silently.
std::string generateSecret();

// otpauth:// URL for QR rendering on the frontend. account_label appears
// next to the issuer in the authenticator app; pass the username or email.
std::string otpAuthUrl(const std::string& secret_b32,
                       const std::string& account_label,
                       const std::string& issuer);

// Generates the 6-digit code for the given UNIX time. Exposed mostly so
// the test suite can exercise the RFC vectors directly.
unsigned int generateCode(const std::string& secret_b32,
                          std::uint64_t      unix_seconds);

// Constant-time verification with a ±1 step window (~60 s of leniency)
// to absorb clock skew between server and authenticator. The window is
// the standard recommendation in RFC 6238 §5.2.
bool verify(const std::string& secret_b32,
            const std::string& candidate_code);

// As verify(), but returns the absolute time-step (UNIX seconds / 30) that
// matched, or 0 when no step in the window matched. Callers persist the
// returned step so a code cannot be replayed within its ~90 s validity
// window: a second attempt presenting a step <= the last accepted one is
// rejected. Step 0 (1970) is never a real value, so it doubles as the
// "no match" sentinel.
std::uint64_t verifyWithStep(const std::string& secret_b32,
                             const std::string& candidate_code);

// RFC 4648 Base32 (no padding) — exposed for the tests and for code paths
// that want to render the secret in groups of 4 for manual entry.
std::string base32Encode(const unsigned char* data, std::size_t len);
bool        base32Decode(const std::string& in, std::string& out);

} // namespace totp
