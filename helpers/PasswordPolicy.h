#pragma once

#include <string>

// Password acceptance rules for registration, reset and change.
//
// The shape follows NIST SP 800-63B: length is the control that matters,
// composition rules ("one uppercase, one digit, one symbol") are not
// required, and the checks that *are* worth doing are the ones that catch
// passwords already known to attackers. Composition rules mostly push
// people towards predictable mutations — Password1! satisfies every
// checkbox and appears in every cracking dictionary — so this deliberately
// does not impose them.
//
// Three checks instead:
//
//   1. Length. 8 minimum (the pre-existing floor), 256 maximum. The cap
//      exists because Argon2id hashes whatever it is given and an
//      unbounded input is a free way to burn 64 MiB of server memory.
//
//   2. Context. A password equal to (or containing) the username or the
//      local part of the email is the first thing any targeted attempt
//      tries.
//
//   3. Breach. Optionally checked against Have I Been Pwned. A password
//      that appears in a public breach corpus is not secret regardless of
//      how strong it looks.
namespace password_policy {

// Returns an empty string when the password is acceptable, otherwise a
// message suitable for showing to the user.
//
// `username` and `email` may be empty when the caller does not have them
// (the reset flow only knows the token). The breach check is only
// performed when enabled — see breachCheckEnabled().
std::string validate(const std::string& password,
                     const std::string& username,
                     const std::string& email);

// True when BLOG_HIBP_ENABLED=1. Off by default: the check makes an
// outbound HTTPS request on the registration path, which not every
// deployment wants, and an operator should opt into that explicitly.
bool breachCheckEnabled();

// Queries Have I Been Pwned for `password` using k-anonymity: SHA-1 the
// password, send only the FIRST FIVE hex characters of the digest, and
// match the remainder locally against the returned suffix list. The
// service therefore never learns the password, nor its full hash, nor
// which of the ~800 returned candidates was ours.
//
// Fails OPEN. If the request times out or the service is unreachable, this
// returns false and the password is accepted. That is deliberate: a
// third-party outage must not take registration and password resets down
// with it, and the check is a hardening measure rather than an
// authorization decision.
//
// Blocking — call only from a worker thread, never an IO loop.
bool isBreached(const std::string& password);

// Exposed for tests: the k-anonymity split of a password's SHA-1, as
// (first five hex chars, remaining 35).
struct HashSplit {
    std::string prefix;   // 5 chars, sent to the service
    std::string suffix;   // 35 chars, never leaves the process
};
HashSplit splitHash(const std::string& password);

// Exposed for tests: scans a HIBP range response body for `suffix`.
// The body is `SUFFIX:COUNT` lines with CRLF endings.
bool suffixInRangeResponse(const std::string& body, const std::string& suffix);

} // namespace password_policy
