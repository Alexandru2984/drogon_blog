#include "PasswordPolicy.h"

#include <curl/curl.h>
#include <openssl/sha.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_set>

namespace password_policy {

namespace {

constexpr std::size_t kMinLen = 8;
constexpr std::size_t kMaxLen = 256;

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// A small, deliberately un-clever blocklist.
//
// This is not a substitute for the breach check — it is what still works
// when the breach check is disabled or the service is unreachable, and it
// costs nothing. The entries are the passwords that dominate every leaked
// corpus, plus the ones this application specifically invites ("blog",
// "micutu"). A longer list belongs in a file, not a binary; the returns
// drop off sharply after the first few hundred.
const std::unordered_set<std::string>& commonPasswords()
{
    static const std::unordered_set<std::string> kSet = {
        "password", "password1", "password123", "passw0rd", "p@ssword",
        "123456", "1234567", "12345678", "123456789", "1234567890",
        "12345", "111111", "000000", "654321", "121212", "123123",
        "qwerty", "qwerty123", "qwertyuiop", "azerty", "qwertz",
        "abc123", "a1b2c3", "letmein", "welcome", "welcome1", "monkey",
        "dragon", "sunshine", "princess", "football", "baseball",
        "iloveyou", "admin", "administrator", "root", "toor", "guest",
        "login", "master", "superman", "batman", "trustno1", "starwars",
        "whatever", "freedom", "shadow", "michael", "jennifer", "jordan",
        "hunter", "harley", "ranger", "thomas", "robert", "matthew",
        "changeme", "change-me", "secret", "default", "test", "test123",
        "testing", "temp", "temporary", "asdfgh", "asdf1234", "zxcvbnm",
        "1q2w3e4r", "1qaz2wsx", "qazwsx", "photoshop", "computer",
        "internet", "samsung", "google", "facebook", "linkedin",
        "blog", "blogger", "micutu", "micublog", "drogon",
    };
    return kSet;
}

// Local part of an email address, lowercased. Empty when there is no '@'.
std::string emailLocalPart(const std::string& email)
{
    const auto at = email.find('@');
    if (at == std::string::npos || at == 0) return {};
    return toLower(email.substr(0, at));
}

std::size_t curlSink(void* contents, std::size_t size, std::size_t nmemb,
                     void* userp)
{
    const std::size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(
        static_cast<char*>(contents), total);
    return total;
}

} // namespace

HashSplit splitHash(const std::string& password)
{
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(password.data()),
         password.size(), digest);

    // Uppercase hex: the service's range endpoint compares case-sensitively
    // against uppercase suffixes.
    static const char* kHex = "0123456789ABCDEF";
    std::string hex;
    hex.resize(SHA_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        hex[2 * i]     = kHex[digest[i] >> 4];
        hex[2 * i + 1] = kHex[digest[i] & 0x0F];
    }
    return HashSplit{hex.substr(0, 5), hex.substr(5)};
}

bool suffixInRangeResponse(const std::string& body, const std::string& suffix)
{
    // Lines are `SUFFIX:COUNT`, CRLF-separated. Match on the suffix field
    // only — a naive `body.find(suffix)` would also hit the count column
    // and any partial overlap across a line boundary.
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t end = body.find('\n', pos);
        if (end == std::string::npos) end = body.size();

        std::size_t lineEnd = end;
        while (lineEnd > pos &&
               (body[lineEnd - 1] == '\r' || body[lineEnd - 1] == '\n')) {
            --lineEnd;
        }

        const std::size_t colon = body.find(':', pos);
        if (colon != std::string::npos && colon <= lineEnd) {
            if (colon - pos == suffix.size() &&
                body.compare(pos, suffix.size(), suffix) == 0) {
                return true;
            }
        }
        pos = end + 1;
    }
    return false;
}

bool breachCheckEnabled()
{
    const char* v = std::getenv("BLOG_HIBP_ENABLED");
    return v && std::string(v) == "1";
}

bool isBreached(const std::string& password)
{
    if (!breachCheckEnabled()) return false;

    const auto split = splitHash(password);
    const std::string url =
        "https://api.pwnedpasswords.com/range/" + split.prefix;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string body;
    struct curl_slist* headers = nullptr;
    // Ask the service to pad the response with random decoys, so an
    // observer cannot infer anything from the response size either.
    headers = curl_slist_append(headers, "Add-Padding: true");
    headers = curl_slist_append(headers, "User-Agent: drogon-blog");

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curlSink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    // Short timeouts. This sits on the registration path, and a slow
    // third party must not become our latency.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || status != 200) {
        // Fail open. A third-party outage must not stop people from
        // registering or resetting a password; this is hardening, not an
        // authorization decision.
        LOG_WARN << "HIBP lookup failed (curl=" << curl_easy_strerror(rc)
                 << ", status=" << status << "); allowing password";
        return false;
    }

    return suffixInRangeResponse(body, split.suffix);
}

std::string validate(const std::string& password,
                     const std::string& username,
                     const std::string& email)
{
    if (password.size() < kMinLen) {
        return "Password must be at least 8 characters";
    }
    if (password.size() > kMaxLen) {
        return "Password must be at most 256 characters";
    }

    const std::string lower = toLower(password);

    if (commonPasswords().count(lower)) {
        return "That password is one of the most common in use — pick another";
    }

    // Context: a password that is (or contains) the account's own name is
    // the first guess of any targeted attempt. Checked in both directions
    // because "micu" and "micu2026" are equally guessable for user micu.
    if (!username.empty()) {
        const std::string u = toLower(username);
        if (u.size() >= 3 && lower.find(u) != std::string::npos) {
            return "Password must not contain your username";
        }
    }
    if (!email.empty()) {
        const std::string local = emailLocalPart(email);
        if (local.size() >= 3 && lower.find(local) != std::string::npos) {
            return "Password must not contain your email address";
        }
    }

    if (isBreached(password)) {
        return "That password has appeared in a public data breach — "
               "pick one you have not used elsewhere";
    }

    return {};
}

} // namespace password_policy
