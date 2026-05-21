#include "RecoveryCodes.h"
#include "Security.h"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace recovery_codes {

namespace {

// Crockford-ish alphabet: skips visually ambiguous characters (0/O, 1/I/L)
// so users do not confuse a recovery code typed off paper.
constexpr char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";  // 31 chars

std::string randomBlock(int chars)
{
    std::string out(chars, '?');
    for (int i = 0; i < chars; ++i) {
        // We use randombytes_uniform so we don't introduce modulo bias.
        const auto idx = randombytes_uniform(sizeof(kAlphabet) - 1);
        out[i] = kAlphabet[idx];
    }
    return out;
}

} // namespace

std::vector<std::string> generateBatch()
{
    std::vector<std::string> out;
    out.reserve(kBatchSize);
    for (int i = 0; i < kBatchSize; ++i) {
        out.emplace_back(randomBlock(4) + "-" + randomBlock(4));
    }
    return out;
}

std::string hashOne(const std::string& code)
{
    return security::hashPassword(code);
}

bool verifyOne(const std::string& storedHash, const std::string& candidate)
{
    return security::verifyPassword(storedHash, candidate);
}

std::string normalize(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '-') continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (out.size() == 8) out = out.substr(0, 4) + "-" + out.substr(4);
    return out;
}

} // namespace recovery_codes
