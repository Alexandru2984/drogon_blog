#include "Totp.h"

#include <openssl/hmac.h>
#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

namespace totp {

namespace {

constexpr std::uint64_t kStepSeconds = 30;
constexpr int           kCodeDigits  = 6;
constexpr int           kWindow      = 1;     // ±1 step (~60s clock skew)

// RFC 4648 Base32 alphabet, no padding.
constexpr char kB32Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

unsigned int powTen(int n)
{
    unsigned int v = 1;
    while (n--) v *= 10;
    return v;
}

// RFC 6238 §5.3 + RFC 4226 §5.3 dynamic truncation.
unsigned int truncate(const unsigned char* hmac20)
{
    const unsigned int offset  = hmac20[19] & 0x0F;
    const unsigned int binCode =
        (static_cast<unsigned int>(hmac20[offset]    & 0x7F) << 24) |
        (static_cast<unsigned int>(hmac20[offset+1]) << 16)         |
        (static_cast<unsigned int>(hmac20[offset+2]) <<  8)         |
        (static_cast<unsigned int>(hmac20[offset+3]));
    return binCode % powTen(kCodeDigits);
}

unsigned int hotp(const std::string& secretBin, std::uint64_t counter)
{
    unsigned char buf[8];
    for (int i = 7; i >= 0; --i) {
        buf[i] = static_cast<unsigned char>(counter & 0xFF);
        counter >>= 8;
    }
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int  outLen = 0;
    HMAC(EVP_sha1(),
         secretBin.data(), static_cast<int>(secretBin.size()),
         buf, sizeof(buf),
         out, &outLen);
    return truncate(out);
}

std::string padCode(unsigned int v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*u", kCodeDigits, v);
    return buf;
}

// Constant-time string comparison. We cannot use `==` because timing
// differences between matching prefixes leak codeword information to a
// remote attacker probing the verify endpoint.
bool constTimeEqual(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::uint64_t nowUnix()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

std::string base32Encode(const unsigned char* data, std::size_t len)
{
    std::string out;
    out.reserve(((len * 8) + 4) / 5);

    std::uint64_t buffer = 0;
    int           bits   = 0;
    for (std::size_t i = 0; i < len; ++i) {
        buffer = (buffer << 8) | data[i];
        bits  += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kB32Alphabet[(buffer >> bits) & 0x1F]);
        }
    }
    if (bits > 0) {
        out.push_back(kB32Alphabet[(buffer << (5 - bits)) & 0x1F]);
    }
    return out;
}

bool base32Decode(const std::string& in, std::string& out)
{
    out.clear();
    out.reserve(in.size() * 5 / 8);

    std::uint64_t buffer = 0;
    int           bits   = 0;
    for (char c : in) {
        if (c == '=' || c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        int v;
        if (c >= 'A' && c <= 'Z')      v = c - 'A';
        else if (c >= '2' && c <= '7') v = 26 + (c - '2');
        else return false;
        buffer = (buffer << 5) | static_cast<unsigned>(v);
        bits  += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

std::string generateSecret()
{
    unsigned char raw[20];
    randombytes_buf(raw, sizeof(raw));
    return base32Encode(raw, sizeof(raw));
}

std::string otpAuthUrl(const std::string& secret_b32,
                       const std::string& account_label,
                       const std::string& issuer)
{
    // RFC 7848-style otpauth URI. We percent-encode the label and issuer
    // because account_label is user-provided (username) and could contain
    // characters that break the URL.
    auto encode = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            const auto u = static_cast<unsigned char>(c);
            const bool safe = (u >= '0' && u <= '9') ||
                              (u >= 'A' && u <= 'Z') ||
                              (u >= 'a' && u <= 'z') ||
                              c == '-' || c == '_' || c == '.' || c == '~';
            if (safe) {
                out.push_back(c);
            } else {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%%%02X", u);
                out += buf;
            }
        }
        return out;
    };

    return "otpauth://totp/" + encode(issuer) + ":" + encode(account_label)
         + "?secret=" + secret_b32
         + "&issuer=" + encode(issuer)
         + "&algorithm=SHA1&digits=6&period=30";
}

unsigned int generateCode(const std::string& secret_b32,
                          std::uint64_t      unix_seconds)
{
    std::string secretBin;
    if (!base32Decode(secret_b32, secretBin) || secretBin.empty()) return 0;
    return hotp(secretBin, unix_seconds / kStepSeconds);
}

std::uint64_t verifyWithStep(const std::string& secret_b32,
                             const std::string& candidate_code)
{
    if (candidate_code.size() != static_cast<std::size_t>(kCodeDigits)) return 0;
    std::string secretBin;
    if (!base32Decode(secret_b32, secretBin) || secretBin.empty()) return 0;

    const std::uint64_t step = nowUnix() / kStepSeconds;
    // Iterate over [step - kWindow, step + kWindow]. Constant-time compare
    // each candidate so the loop's branch-predictor footprint does not
    // leak which step (if any) matched. We always run the full window
    // (no early break) and only record the matched step at the end, so the
    // accept/reject timing is independent of which step hit.
    std::uint64_t matchedStep = 0;
    for (int delta = -kWindow; delta <= kWindow; ++delta) {
        const std::uint64_t candStep = step + static_cast<std::uint64_t>(delta);
        const auto expected = padCode(hotp(secretBin, candStep));
        if (constTimeEqual(expected, candidate_code)) matchedStep = candStep;
    }
    return matchedStep;
}

bool verify(const std::string& secret_b32,
            const std::string& candidate_code)
{
    return verifyWithStep(secret_b32, candidate_code) != 0;
}

} // namespace totp
