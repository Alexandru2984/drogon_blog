#include <drogon/drogon_test.h>

#include "../helpers/RecoveryCodes.h"
#include "../helpers/Security.h"
#include "../helpers/Totp.h"
#include "../helpers/WebAuthn.h"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class ScopedTotpKey
{
  public:
    ScopedTotpKey()
    {
        const char* value = std::getenv("BLOG_TOTP_KEY");
        hadValue_ = value != nullptr;
        if (hadValue_) value_ = value;
    }

    ~ScopedTotpKey()
    {
        if (hadValue_) setenv("BLOG_TOTP_KEY", value_.c_str(), 1);
        else unsetenv("BLOG_TOTP_KEY");
    }

    ScopedTotpKey(const ScopedTotpKey&) = delete;
    ScopedTotpKey& operator=(const ScopedTotpKey&) = delete;

  private:
    bool        hadValue_ = false;
    std::string value_;
};

} // namespace

// ============================================================================
// TOTP — RFC 6238 Appendix B reference test vectors (HMAC-SHA1, 30s step,
// 8-digit truncation in the RFC; we generate 6 digits, so we compare the
// low 6 digits of each canonical vector). The vectors prove HOTP truncation
// and time-step math are correct; verifying both is what the test suite
// cares about.
//
// RFC value (8-digit): T=59  -> 94287082  -> 287082 (6-digit)
//                       T=1111111109 -> 07081804 -> 081804
//                       T=1111111111 -> 14050471 -> 050471
//                       T=1234567890 -> 89005924 -> 005924
//                       T=2000000000 -> 69279037 -> 279037
//
// The 20-byte RFC SHA1 seed is "12345678901234567890" ASCII. Base32 of those
// bytes is GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ.
// ============================================================================
DROGON_TEST(TotpRfc6238Vectors)
{
    const std::string seedB32 = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

    struct V { std::uint64_t t; unsigned int six; };
    const V vectors[] = {
        {        59ULL, 287082u},
        {1111111109ULL,  81804u},
        {1111111111ULL,  50471u},
        {1234567890ULL,   5924u},
        {2000000000ULL, 279037u},
    };
    for (const auto& v : vectors) {
        CHECK(totp::generateCode(seedB32, v.t) == v.six);
    }
}

DROGON_TEST(TotpBase32RoundTrip)
{
    // RFC 4648 §10 reference vectors with padding stripped (we emit no
    // padding). "f" -> "MY", "foobar" -> "MZXW6YTBOI".
    const unsigned char foobar[] = {'f','o','o','b','a','r'};
    CHECK(totp::base32Encode(foobar, sizeof(foobar)) == "MZXW6YTBOI");

    // 5-byte input "Hello" packs into exactly 40 bits -> 8 base32 chars.
    const unsigned char hello[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
    CHECK(totp::base32Encode(hello, sizeof(hello)) == "JBSWY3DP");

    std::string back;
    CHECK(totp::base32Decode("MZXW6YTBOI", back));
    CHECK(back == "foobar");
}

DROGON_TEST(TotpSecretGenerationIsFreshAndCorrectLength)
{
    auto a = totp::generateSecret();
    auto b = totp::generateSecret();
    CHECK(a.size() == 32);           // 20 bytes -> 32 base32 chars (no pad)
    CHECK(b.size() == 32);
    CHECK(a != b);
}

DROGON_TEST(TotpOtpAuthUrlEscapesAndIncludesIssuer)
{
    const auto url = totp::otpAuthUrl("ABC", "alice@example.com", "Blog Demo");
    // Encoded space + @ in the label / issuer (no raw '@' or ' ' allowed).
    CHECK(url.find("otpauth://totp/Blog%20Demo:alice%40example.com") == 0);
    CHECK(url.find("secret=ABC") != std::string::npos);
    CHECK(url.find("issuer=Blog%20Demo") != std::string::npos);
}

DROGON_TEST(TotpVerifyAcceptsCurrentCodeAndRejectsOthers)
{
    const auto secret = totp::generateSecret();
    // Generate the code for the current step manually, then verify it.
    const auto now = static_cast<std::uint64_t>(std::time(nullptr));
    const auto code = totp::generateCode(secret, now);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06u", code);
    CHECK(totp::verify(secret, buf));
    CHECK(!totp::verify(secret, "000000"));
    CHECK(!totp::verify(secret, "12345"));        // too short
    CHECK(!totp::verify(secret, "1234567"));      // too long
}

DROGON_TEST(TotpEncryptionConfigurationFailsClosed)
{
    ScopedTotpKey restore;

    setenv("BLOG_TOTP_KEY", "configured-but-not-a-valid-key", 1);
    bool rejected = false;
    try {
        security::validateTotpKeyConfiguration();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);

    // A malformed configured value must also fail at the write boundary even
    // if a future entry point forgets to call the startup validator.
    rejected = false;
    try {
        (void)security::wrapTotpSecret("JBSWY3DPEHPK3PXP");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);

    setenv("BLOG_TOTP_KEY",
           "0123456789abcdef0123456789abcdef"
           "0123456789abcdef0123456789abcdef",
           1);
    security::validateTotpKeyConfiguration();
    const std::string secret = "JBSWY3DPEHPK3PXP";
    const auto wrapped = security::wrapTotpSecret(secret);
    CHECK(wrapped.rfind("enc:v1:", 0) == 0);
    CHECK(wrapped != secret);
    CHECK(security::unwrapTotpSecret(wrapped) == secret);
}

// ============================================================================
// Recovery codes
// ============================================================================
DROGON_TEST(RecoveryCodesBatchHasDistinctFormattedEntries)
{
    auto batch = recovery_codes::generateBatch();
    CHECK(batch.size() == 10);
    // Every code is "XXXX-XXXX" (9 chars) and all are distinct.
    std::vector<std::string> seen;
    for (const auto& c : batch) {
        CHECK(c.size() == 9);
        CHECK(c[4] == '-');
        for (size_t i = 0; i < c.size(); ++i) {
            if (i == 4) continue;
            const char ch = c[i];
            CHECK(((ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '9')));
        }
        for (const auto& prev : seen) CHECK(prev != c);
        seen.push_back(c);
    }
}

DROGON_TEST(RecoveryCodeHashAndVerifyAreInverse)
{
    const auto code = recovery_codes::generateBatch()[0];
    const auto hash = recovery_codes::hashOne(code);
    CHECK(recovery_codes::verifyOne(hash, code));
    CHECK(!recovery_codes::verifyOne(hash, "AAAA-AAAA"));
}

DROGON_TEST(RecoveryCodeNormalizeAcceptsLooseFormats)
{
    CHECK(recovery_codes::normalize("abcd-efgh") == "ABCD-EFGH");
    CHECK(recovery_codes::normalize("ABCDEFGH")  == "ABCD-EFGH");
    CHECK(recovery_codes::normalize(" abcd efgh ") == "ABCD-EFGH");
    CHECK(recovery_codes::normalize("abc")  == "ABC");        // wrong length stays as-is
}

// ============================================================================
// WebAuthn — we can exercise the base64url + challenge + clientDataJSON
// validation paths without a real authenticator. The signature paths live
// in helpers/WebAuthn.cc and are exercised end-to-end by the browser when
// the user enrols a passkey.
// ============================================================================
DROGON_TEST(WebauthnBase64UrlRoundTrip)
{
    const unsigned char data[] = {0x00, 0x01, 0xFF, 0xAB, 0xCD};
    const auto enc = webauthn::base64UrlEncode(data, sizeof(data));
    std::vector<unsigned char> back;
    CHECK(webauthn::base64UrlDecode(enc, back));
    CHECK(back == std::vector<unsigned char>(data, data + sizeof(data)));
    // Neither '+' nor '/' show up in URL-safe base64.
    CHECK(enc.find('+') == std::string::npos);
    CHECK(enc.find('/') == std::string::npos);
}

DROGON_TEST(WebauthnChallengeIsFreshAndUrlSafe)
{
    const auto a = webauthn::makeChallenge();
    const auto b = webauthn::makeChallenge();
    CHECK(a != b);
    CHECK(!a.empty());
    CHECK(a.find('+') == std::string::npos);
    CHECK(a.find('/') == std::string::npos);
}

DROGON_TEST(WebauthnFinishRegistrationRejectsBadBase64)
{
    std::string err;
    auto res = webauthn::finishRegistration(
        "!!!not-base64!!!", "abc", "challenge",
        "blog.local", "https://blog.local", err);
    CHECK(!res.has_value());
    CHECK(err.find("base64") != std::string::npos);
}
