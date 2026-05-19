#include <drogon/drogon_test.h>
#include <sodium.h>
#include <string>

DROGON_TEST(Argon2id_HashFormatAndVerify)
{
    char out[crypto_pwhash_STRBYTES];
    const std::string pw = "correct horse battery staple";

    REQUIRE(crypto_pwhash_str(out,
                              pw.c_str(),
                              pw.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0);

    const std::string hash(out);

    // Self-describing hash: algorithm + params + salt + tag.
    CHECK(hash.rfind("$argon2id$", 0) == 0);
    CHECK(hash.find("$v=19$") != std::string::npos);

    // Correct password verifies; wrong password does not.
    CHECK(crypto_pwhash_str_verify(hash.c_str(), pw.c_str(), pw.size()) == 0);
    const std::string wrong = "wrong";
    CHECK(crypto_pwhash_str_verify(hash.c_str(), wrong.c_str(), wrong.size()) != 0);
}

DROGON_TEST(Argon2id_DistinctSaltsForSamePassword)
{
    char a[crypto_pwhash_STRBYTES];
    char b[crypto_pwhash_STRBYTES];
    const std::string pw = "same input twice";

    REQUIRE(crypto_pwhash_str(a, pw.c_str(), pw.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0);
    REQUIRE(crypto_pwhash_str(b, pw.c_str(), pw.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0);

    // Same plaintext must produce different hashes (different salts).
    CHECK(std::string(a) != std::string(b));
}
