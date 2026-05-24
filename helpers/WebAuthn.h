#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Minimal server-side WebAuthn (FIDO2) implementation focused on the
// "passkey as a second factor" use case. Trade-offs:
//   * We accept ONLY the `none` attestation format. Verifying an
//     authenticator's attestation cert chain is the right thing for a
//     bank and the wrong thing for a personal blog (every browser ships
//     the same "none" path; users want enrolment to work on Android,
//     iOS, security keys and platform passkeys with no fuss).
//   * Public keys supported: COSE ES256 (alg -7) and EdDSA (alg -8 / Ed25519).
//     Together they cover ~all hardware keys and platform authenticators
//     in 2026. RS256 is excluded — the few authenticators that produce
//     RSA keys also support ES256 negotiation.
//   * Challenge management lives in the session (one challenge at a
//     time) — clean and bounded. No external state store.
//
// We deliberately do not pull in libfido2: its server-side surface is
// minimal and the parts we need (CBOR for COSE keys, ECDSA-P256/Ed25519
// signature verification) are already available through OpenSSL +
// libsodium. The implementation is ~250 lines; readability > a heavy
// dependency for code I want a reviewer to be able to audit end-to-end.
namespace webauthn {

struct RegistrationResult {
    std::string credential_id_b64u;   // base64url, suitable for DB
    std::vector<unsigned char> cose_public_key;
    std::uint32_t sign_count = 0;
};

struct AuthenticationResult {
    std::uint32_t new_sign_count = 0;
};

// ---- Begin handlers ----

// Returns a fresh 32-byte challenge (base64url) the client should pass
// into `navigator.credentials.create({ publicKey: { challenge, ... } })`.
std::string makeChallenge();

// ---- Register-finish ----
//
// Verifies the client-side registration response against the challenge
// the server originally sent (and the configured RP ID + origin), then
// returns the credential ID and the COSE public key bytes that should
// be persisted alongside the user row.
//
// rpId / origin are usually pulled from `BLOG_WEBAUTHN_RP_ID` and
// `BLOG_SITE_ORIGIN` env vars. The challenge is the base64url string
// returned by `makeChallenge()`.
//
// `clientDataJSON_b64u` and `attestationObject_b64u` are the two
// fields the browser produces. They arrive base64url-encoded over the
// wire to stay JSON-safe; we decode here.
std::optional<RegistrationResult> finishRegistration(
    const std::string& clientDataJSON_b64u,
    const std::string& attestationObject_b64u,
    const std::string& expected_challenge_b64u,
    const std::string& rpId,
    const std::string& origin,
    std::string&       error_out);

// ---- Authenticate-finish ----
//
// Verifies the assertion (signature) produced by the authenticator
// against the stored COSE public key. On success returns the new sign
// counter the caller should write back to the row.
//
// A sign-counter regression (new_sign_count <= stored_sign_count) is a
// hard reject: it indicates the credential may have been cloned.
std::optional<AuthenticationResult> finishAuthentication(
    const std::string&                  clientDataJSON_b64u,
    const std::string&                  authenticatorData_b64u,
    const std::string&                  signature_b64u,
    const std::string&                  expected_challenge_b64u,
    const std::string&                  rpId,
    const std::string&                  origin,
    const std::vector<unsigned char>&   stored_cose_public_key,
    std::uint32_t                       stored_sign_count,
    std::string&                        error_out);

// Helpers exposed for tests.
std::string base64UrlEncode(const unsigned char* data, std::size_t len);
bool        base64UrlDecode(const std::string& in, std::vector<unsigned char>& out);

} // namespace webauthn
