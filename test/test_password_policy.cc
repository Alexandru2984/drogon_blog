#include <drogon/drogon_test.h>

#include "../helpers/PasswordPolicy.h"

#include <string>

// The policy follows NIST SP 800-63B: length is the control that matters,
// composition rules are not imposed, and what is actually checked is
// whether the password is already known to attackers. These tests pin the
// parts that are easy to break silently.

DROGON_TEST(PasswordPolicy_LengthBounds)
{
    CHECK(!password_policy::validate("short", "", "").empty());
    CHECK(!password_policy::validate("", "", "").empty());
    CHECK(!password_policy::validate(std::string(257, 'a'), "", "").empty());

    // 256 is the documented ceiling, not a rejection. The cap exists
    // because Argon2id hashes whatever it is handed and an unbounded input
    // is a free way to burn 64 MiB of server memory per request.
    CHECK(password_policy::validate(std::string(256, 'a'), "", "").empty());
}

// Composition rules are deliberately absent: requiring an uppercase, a
// digit and a symbol pushes people towards predictable mutations, and
// "Password1!" satisfies every such rule while sitting in every cracking
// dictionary. A long, plain passphrase must be accepted.
DROGON_TEST(PasswordPolicy_AcceptsPassphrasesWithoutComposition)
{
    // Deliberately not "correct horse battery staple": that one is famous
    // enough to appear in breach corpora, so it would start failing the
    // moment a deployment turned the breach check on.
    CHECK(password_policy::validate(
              "thistlebound riverwake lantern", "", "").empty());
    CHECK(password_policy::validate("thistlebound-riverwake", "", "").empty());
}

DROGON_TEST(PasswordPolicy_RejectsTheObviousOnes)
{
    CHECK(!password_policy::validate("password", "", "").empty());
    CHECK(!password_policy::validate("PASSWORD", "", "").empty());   // case-folded
    CHECK(!password_policy::validate("12345678", "", "").empty());
    CHECK(!password_policy::validate("qwertyuiop", "", "").empty());
    CHECK(!password_policy::validate("changeme", "", "").empty());
}

// A password containing the account's own name is the first guess any
// targeted attempt makes. Checked as a substring in both directions,
// because "micu" and "micu2026summer" are equally guessable for user micu.
DROGON_TEST(PasswordPolicy_RejectsContextualPasswords)
{
    CHECK(!password_policy::validate("micu2026summer", "micu", "").empty());
    CHECK(!password_policy::validate("xxMICUxxlongenough", "micu", "").empty());
    CHECK(!password_policy::validate(
              "alexmihai-is-here", "", "alexmihai@example.com").empty());

    // A short username must not turn into a substring match that rejects
    // half of all passwords — "abc" appearing anywhere would be absurd.
    CHECK(password_policy::validate("thistlebound-riverwake", "ab", "").empty());
}

// k-anonymity is the entire privacy argument for the breach check: only
// the first five hex characters of the SHA-1 ever leave the process, so
// the service learns neither the password nor its full hash.
DROGON_TEST(PasswordPolicy_HashSplitIsKAnonymous)
{
    // Known SHA-1 of "password", uppercase:
    // 5BAA61E4C9B93F3F0682250B6CF8331B7EE68FD8
    const auto s = password_policy::splitHash("password");
    CHECK(s.prefix == "5BAA6");
    CHECK(s.suffix == "1E4C9B93F3F0682250B6CF8331B7EE68FD8");
    CHECK(s.prefix.size() == 5);
    CHECK(s.suffix.size() == 35);
}

// The range response is `SUFFIX:COUNT` lines. Matching must be anchored to
// the suffix field: a naive substring search over the body would also hit
// the count column and any accidental overlap across a line boundary,
// which would reject perfectly good passwords.
DROGON_TEST(PasswordPolicy_RangeResponseParsingIsAnchored)
{
    const std::string body =
        "0018A45C4D1DEF81644B54AB7F969B88D65:1\r\n"
        "00D4F6E8FA6EECAD2A3AA415EEC418D38EC:2\r\n"
        "1E4C9B93F3F0682250B6CF8331B7EE68FD8:9659365\r\n";

    CHECK(password_policy::suffixInRangeResponse(
              body, "1E4C9B93F3F0682250B6CF8331B7EE68FD8"));
    CHECK(!password_policy::suffixInRangeResponse(
              body, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"));

    // A count value must never be mistaken for a suffix.
    CHECK(!password_policy::suffixInRangeResponse(body, "9659365"));

    // Nor a prefix of a real suffix.
    CHECK(!password_policy::suffixInRangeResponse(body, "1E4C9B93"));

    CHECK(!password_policy::suffixInRangeResponse("", "ANYTHING"));
}

// Off by default. The check makes an outbound request on the registration
// path, and an operator should opt into that rather than discover it.
DROGON_TEST(PasswordPolicy_BreachCheckIsOptIn)
{
    const char* v = std::getenv("BLOG_HIBP_ENABLED");
    const bool expected = v && std::string(v) == "1";
    CHECK(password_policy::breachCheckEnabled() == expected);
}

// ---------------------------------------------------------------------------
// Login throttle
// ---------------------------------------------------------------------------
// Exercised directly rather than over HTTP: the whole suite runs with
// BLOG_DISABLE_RATE_LIMIT=1 (every request originates from 127.0.0.1, so
// the production limits would trip mid-run), and clearing that variable
// for an HTTP test would switch the per-IP limiter back on for every other
// test running alongside it. Calling the functions lets the switch be
// flipped for exactly this test.

#include "../helpers/LoginThrottle.h"

#include <drogon/drogon.h>

namespace {

// Restores BLOG_DISABLE_RATE_LIMIT on the way out so the rest of the suite
// keeps the limiter off.
class ThrottleEnabled
{
  public:
    ThrottleEnabled()
    {
        const char* prev = std::getenv("BLOG_DISABLE_RATE_LIMIT");
        had_  = prev != nullptr;
        prev_ = had_ ? prev : "";
        unsetenv("BLOG_DISABLE_RATE_LIMIT");
    }
    ~ThrottleEnabled()
    {
        if (had_) setenv("BLOG_DISABLE_RATE_LIMIT", prev_.c_str(), 1);
    }
    ThrottleEnabled(const ThrottleEnabled&)            = delete;
    ThrottleEnabled& operator=(const ThrottleEnabled&) = delete;

  private:
    bool        had_;
    std::string prev_;
};

int makeThrottleUser()
{
    auto db = drogon::app().getDbClient();
    static int seq = 0;
    const std::string suffix =
        std::to_string(++seq) + "_" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch().count());
    const auto r = db->execSqlSync(
        "INSERT INTO users (username, email, password_hash) "
        "VALUES ($1, $2, 'x') RETURNING id",
        "thr_" + suffix, "thr_" + suffix + "@example.test");
    return r[0]["id"].as<int>();
}

} // namespace

DROGON_TEST(LoginThrottle_EngagesAtThresholdAndResetsOnSuccess)
{
    ThrottleEnabled on;
    const int uid = makeThrottleUser();

    // Below the threshold the account is untouched. A user who mistypes a
    // few times must not be made to wait.
    for (int i = 0; i < login_throttle::kThreshold - 1; ++i) {
        login_throttle::recordFailure(uid, "thr@example.test", "thr");
        CHECK(!login_throttle::check(uid).throttled);
    }

    // Crossing it engages the throttle, with a bounded wait — the cap is
    // what keeps this from becoming a way to lock a victim out
    // indefinitely by failing logins on their behalf.
    login_throttle::recordFailure(uid, "thr@example.test", "thr");
    const auto d = login_throttle::check(uid);
    CHECK(d.throttled);
    CHECK(d.retryAfterSeconds > 0);
    CHECK(d.retryAfterSeconds <= login_throttle::kWindowSeconds);

    // A correct password clears the episode immediately.
    login_throttle::recordSuccess(uid);
    CHECK(!login_throttle::check(uid).throttled);

    auto db = drogon::app().getDbClient();
    db->execSqlSync("DELETE FROM users WHERE id = $1", uid);
}

// Failures older than the window are not consecutive, so they must not
// accumulate. Otherwise someone who mistypes once a month would eventually
// throttle themselves out of their own account.
DROGON_TEST(LoginThrottle_StaleFailuresDoNotAccumulate)
{
    ThrottleEnabled on;
    const int uid = makeThrottleUser();
    auto db = drogon::app().getDbClient();

    // Park the account just under the threshold, then age the last failure
    // past the window.
    for (int i = 0; i < login_throttle::kThreshold - 1; ++i) {
        login_throttle::recordFailure(uid, "thr@example.test", "thr");
    }
    db->execSqlSync(
        "UPDATE users SET last_failed_login = NOW() - make_interval(secs => $2::int) "
        " WHERE id = $1",
        uid, login_throttle::kWindowSeconds + 60);

    // The next failure starts a fresh count rather than tipping it over.
    login_throttle::recordFailure(uid, "thr@example.test", "thr");
    CHECK(!login_throttle::check(uid).throttled);

    const auto r = db->execSqlSync(
        "SELECT failed_login_count FROM users WHERE id = $1", uid);
    CHECK(r[0]["failed_login_count"].as<int>() == 1);

    db->execSqlSync("DELETE FROM users WHERE id = $1", uid);
}
