#include "WebAuthn.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/sha.h>
#include <sodium.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace webauthn {

namespace {

// ============================ Base64URL ============================

const std::string kB64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64Index(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// ============================ Minimal CBOR ============================
//
// We only support the subset COSE keys + WebAuthn attestation objects use:
//   - unsigned int (major 0)
//   - negative int (major 1)
//   - byte string  (major 2)
//   - text  string (major 3)
//   - array        (major 4)
//   - map          (major 5)
// Plus indefinite-length is rejected (modern authenticators emit definite).

class CborReader {
public:
    CborReader(const unsigned char* data, std::size_t len) : p_(data), end_(data + len) {}

    bool eof() const { return p_ >= end_; }
    std::size_t remaining() const { return static_cast<std::size_t>(end_ - p_); }
    const unsigned char* ptr() const { return p_; }

    // Reads the next item header (major type + value). On byte/text
    // strings, `value` is the length and the payload sits at ptr().
    // On arrays/maps, `value` is the element count.
    bool readHead(int& major, std::uint64_t& value)
    {
        if (eof()) return false;
        const unsigned char b = *p_++;
        major = (b >> 5) & 0x07;
        const unsigned info = b & 0x1F;
        if (info < 24) { value = info; return true; }
        if (info == 24) { if (remaining() < 1) return false; value = *p_++; return true; }
        if (info == 25) { if (remaining() < 2) return false;
            value = (static_cast<std::uint64_t>(p_[0]) << 8) | p_[1]; p_ += 2; return true; }
        if (info == 26) { if (remaining() < 4) return false;
            value = (static_cast<std::uint64_t>(p_[0]) << 24) |
                    (static_cast<std::uint64_t>(p_[1]) << 16) |
                    (static_cast<std::uint64_t>(p_[2]) <<  8) |
                     static_cast<std::uint64_t>(p_[3]);
            p_ += 4; return true; }
        if (info == 27) { if (remaining() < 8) return false;
            value = 0;
            for (int i = 0; i < 8; ++i) value = (value << 8) | p_[i];
            p_ += 8; return true; }
        return false;
    }

    // Reads `len` bytes, advancing.
    bool readBytes(std::size_t len, std::vector<unsigned char>& out)
    {
        if (remaining() < len) return false;
        out.assign(p_, p_ + len);
        p_ += len;
        return true;
    }

    bool readText(std::size_t len, std::string& out)
    {
        if (remaining() < len) return false;
        out.assign(reinterpret_cast<const char*>(p_), len);
        p_ += len;
        return true;
    }

    // Cap recursion depth so a crafted attestationObject can't blow the
    // stack by nesting arrays / maps. 16 is generously above anything a
    // real authenticator emits (typical attestation is 1–2 levels deep).
    static constexpr int kMaxCborDepth = 16;
    bool skip(int depth = 0)
    {
        if (depth >= kMaxCborDepth) return false;
        int           major;
        std::uint64_t v;
        if (!readHead(major, v)) return false;
        switch (major) {
            case 0: case 1: case 7:
                return true;
            case 2: case 3:
                if (remaining() < v) return false;
                p_ += v;
                return true;
            case 4:
                for (std::uint64_t i = 0; i < v; ++i)
                    if (!skip(depth + 1)) return false;
                return true;
            case 5:
                for (std::uint64_t i = 0; i < v; ++i) {
                    if (!skip(depth + 1)) return false;  // key
                    if (!skip(depth + 1)) return false;  // value
                }
                return true;
            default:
                return false;
        }
    }

private:
    const unsigned char* p_;
    const unsigned char* end_;
};

// ============================ SHA-256 ============================
std::vector<unsigned char> sha256(const unsigned char* data, std::size_t len)
{
    std::vector<unsigned char> out(SHA256_DIGEST_LENGTH);
    SHA256(data, len, out.data());
    return out;
}

// ============================ Client data parsing ============================
//
// clientDataJSON is a UTF-8 JSON object the browser produced. The spec
// requires us to validate it against the original challenge / RP origin
// / ceremony type before doing any signature work. We parse it with
// a tiny key-finding scan rather than dragging Jsoncpp into a security-
// sensitive path (the format is fixed and JSON-encoded inside the
// browser, so we never see exotic content here).
bool extractJsonString(const std::string& s, const std::string& key, std::string& out)
{
    const std::string needle = std::string("\"") + key + "\":";
    auto pos = s.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        if (s[pos] == '\\') {
            if (pos + 1 >= s.size()) return false;
            const char esc = s[pos + 1];
            if (esc == 'n')      out.push_back('\n');
            else if (esc == 'r') out.push_back('\r');
            else if (esc == 't') out.push_back('\t');
            else if (esc == '"' || esc == '\\' || esc == '/') out.push_back(esc);
            else return false;  // \uXXXX etc. not needed for the fields we read
            pos += 2;
        } else if (s[pos] == '"') {
            return true;
        } else {
            out.push_back(s[pos++]);
        }
    }
    return false;
}

// ============================ COSE signature verify ============================
//
// COSE map keys we care about:
//   1  (kty)  : 2 = EC2, 1 = OKP
//   3  (alg)  : -7 = ES256, -8 = EdDSA
//  -1  (crv)  : EC2 → 1 = P-256; OKP → 6 = Ed25519
//  -2  (x)    : EC x-coordinate (32 bytes) or Ed25519 raw public key (32 bytes)
//  -3  (y)    : EC y-coordinate (32 bytes) — absent on OKP

struct CoseKey {
    int kty = 0;
    int alg = 0;
    int crv = 0;
    std::vector<unsigned char> x;
    std::vector<unsigned char> y;
};

bool parseCoseKey(const std::vector<unsigned char>& cose, CoseKey& out)
{
    CborReader r(cose.data(), cose.size());
    int major;
    std::uint64_t mapLen;
    if (!r.readHead(major, mapLen) || major != 5) return false;
    for (std::uint64_t i = 0; i < mapLen; ++i) {
        int km;
        std::uint64_t kv;
        if (!r.readHead(km, kv)) return false;
        long long key;
        if (km == 0)      key = static_cast<long long>(kv);
        else if (km == 1) key = -static_cast<long long>(kv) - 1;
        else return false;

        int vm;
        std::uint64_t vv;
        if (!r.readHead(vm, vv)) return false;

        auto signedValue = [&]() -> long long {
            if (vm == 0) return static_cast<long long>(vv);
            if (vm == 1) return -static_cast<long long>(vv) - 1;
            return 0;
        };

        if (key == 1) out.kty = static_cast<int>(signedValue());
        else if (key == 3) out.alg = static_cast<int>(signedValue());
        else if (key == -1) out.crv = static_cast<int>(signedValue());
        else if (key == -2 && vm == 2) { if (!r.readBytes(vv, out.x)) return false; }
        else if (key == -3 && vm == 2) { if (!r.readBytes(vv, out.y)) return false; }
        else {
            // Skip unknown values. The header was already consumed, so
            // for byte/text strings we still need to consume the body.
            if (vm == 2 || vm == 3) {
                std::vector<unsigned char> sink;
                if (!r.readBytes(vv, sink)) return false;
            }
        }
    }
    return true;
}

// Parses an authenticatorData buffer (variable length). Returns true and
// fills the fields if it is well-formed enough for our use.
//
// Layout:
//   [0..31]  rpIdHash       (SHA-256 of RP ID)
//   [32]     flags          (bit 0 = UP, bit 2 = UV, bit 6 = AT, bit 7 = ED)
//   [33..36] signCount      (big-endian)
//   [37..52] AAGUID         (only when AT flag set)
//   [53..54] credentialIdLen (BE u16, only when AT flag set)
//   [55..]   credentialId
//   [..]     credentialPublicKey (CBOR-encoded COSE_Key, only when AT flag)
//   [..]     extensions (CBOR, only when ED flag)
struct AuthData {
    std::vector<unsigned char> rpIdHash;
    unsigned char              flags = 0;
    std::uint32_t              signCount = 0;
    std::vector<unsigned char> credentialId;
    std::vector<unsigned char> cosePublicKey;
};

bool parseAuthData(const std::vector<unsigned char>& a, AuthData& out, bool needCred)
{
    if (a.size() < 37) return false;
    out.rpIdHash.assign(a.begin(), a.begin() + 32);
    out.flags     = a[32];
    out.signCount = (static_cast<std::uint32_t>(a[33]) << 24) |
                    (static_cast<std::uint32_t>(a[34]) << 16) |
                    (static_cast<std::uint32_t>(a[35]) <<  8) |
                     static_cast<std::uint32_t>(a[36]);
    if (!needCred) return true;

    if ((out.flags & 0x40) == 0) return false;       // AT flag must be set on register
    if (a.size() < 37 + 16 + 2)  return false;
    const auto credLen = (static_cast<std::size_t>(a[53]) << 8) | a[54];
    if (a.size() < 55 + credLen) return false;
    using diff_t = std::vector<unsigned char>::difference_type;
    out.credentialId.assign(a.begin() + static_cast<diff_t>(55),
                            a.begin() + static_cast<diff_t>(55 + credLen));

    // The remaining buffer is the COSE_Key (CBOR), possibly followed by
    // extensions when the ED flag is set. We hand the *rest* of the buffer
    // to the COSE parser, which stops at the natural end of its map.
    out.cosePublicKey.assign(a.begin() + static_cast<diff_t>(55 + credLen),
                             a.end());
    return true;
}

// Splits the attestationObject CBOR map and returns the authData by-ref.
// `fmt` is the attestation format name; `attStmt` is the (typically empty
// for `none`) statement map, which we skip cleanly via the generic
// reader so we don't fall out of sync on map ordering.
bool parseAttestationObject(const std::vector<unsigned char>& obj,
                            std::string& fmt,
                            std::vector<unsigned char>& authData)
{
    CborReader r(obj.data(), obj.size());
    int major;
    std::uint64_t mapLen;
    if (!r.readHead(major, mapLen) || major != 5) return false;
    for (std::uint64_t i = 0; i < mapLen; ++i) {
        int km;
        std::uint64_t kv;
        if (!r.readHead(km, kv) || km != 3) return false;   // key must be tstr
        std::string key;
        if (!r.readText(kv, key)) return false;

        if (key == "fmt") {
            int vm;
            std::uint64_t vv;
            if (!r.readHead(vm, vv) || vm != 3) return false;
            if (!r.readText(vv, fmt)) return false;
        } else if (key == "authData") {
            int vm;
            std::uint64_t vv;
            if (!r.readHead(vm, vv) || vm != 2) return false;
            if (!r.readBytes(vv, authData)) return false;
        } else {
            // attStmt / unknown — skip whatever shape it is. The reader's
            // skip() consumes one full CBOR item including nested maps.
            if (!r.skip()) return false;
        }
    }
    return true;
}

// Helper: build the signed data for an authentication assertion:
//   authenticatorData || SHA-256(clientDataJSON)
std::vector<unsigned char> assertionSignedData(
    const std::vector<unsigned char>& authData,
    const std::string&                clientDataJSON)
{
    auto cdHash = sha256(
        reinterpret_cast<const unsigned char*>(clientDataJSON.data()),
        clientDataJSON.size());
    std::vector<unsigned char> out;
    out.reserve(authData.size() + cdHash.size());
    out.insert(out.end(), authData.begin(), authData.end());
    out.insert(out.end(), cdHash.begin(),  cdHash.end());
    return out;
}

bool verifyEs256(const CoseKey& key,
                 const std::vector<unsigned char>& signedData,
                 const std::vector<unsigned char>& signatureDer)
{
    if (key.x.size() != 32 || key.y.size() != 32) return false;

    // OpenSSL 3.0+ way of building an EVP_PKEY from raw EC point bytes,
    // without touching the deprecated low-level EC_KEY API. The
    // uncompressed form is `04 || X || Y` (65 bytes).
    unsigned char point[65];
    point[0] = 0x04;
    std::memcpy(point + 1,      key.x.data(), 32);
    std::memcpy(point + 1 + 32, key.y.data(), 32);

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    OSSL_PARAM*     params = nullptr;
    EVP_PKEY_CTX*   pctx = nullptr;
    EVP_PKEY*       pkey = nullptr;
    EVP_MD_CTX*     mctx = nullptr;
    bool            verified = false;

    if (!bld) return false;
    if (OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME,
                                        const_cast<char*>("P-256"), 0) != 1) goto out;
    if (OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY,
                                         point, sizeof(point)) != 1) goto out;
    params = OSSL_PARAM_BLD_to_param(bld);
    if (!params) goto out;

    pctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    if (!pctx) goto out;
    if (EVP_PKEY_fromdata_init(pctx) != 1) goto out;
    if (EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) != 1) goto out;

    mctx = EVP_MD_CTX_new();
    if (!mctx) goto out;
    verified =
        EVP_DigestVerifyInit(mctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
        EVP_DigestVerifyUpdate(mctx, signedData.data(), signedData.size()) == 1 &&
        EVP_DigestVerifyFinal(mctx, signatureDer.data(), signatureDer.size()) == 1;

out:
    if (mctx)   EVP_MD_CTX_free(mctx);
    if (pkey)   EVP_PKEY_free(pkey);
    if (pctx)   EVP_PKEY_CTX_free(pctx);
    if (params) OSSL_PARAM_free(params);
    if (bld)    OSSL_PARAM_BLD_free(bld);
    return verified;
}

bool verifyEd25519(const CoseKey& key,
                   const std::vector<unsigned char>& signedData,
                   const std::vector<unsigned char>& signature)
{
    if (key.x.size() != 32 || signature.size() != 64) return false;

    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, key.x.data(), 32);
    if (!pkey) return false;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool verified =
        ctx &&
        EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
        EVP_DigestVerify(ctx, signature.data(), signature.size(),
                         signedData.data(), signedData.size()) == 1;
    if (ctx) EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return verified;
}

bool checkClientData(const std::vector<unsigned char>& clientDataJSONBytes,
                     const std::string& expectedType,
                     const std::string& expectedChallenge_b64u,
                     const std::string& expectedOrigin,
                     std::string& error_out)
{
    const std::string s(reinterpret_cast<const char*>(clientDataJSONBytes.data()),
                        clientDataJSONBytes.size());
    std::string typ, chal, orig;
    if (!extractJsonString(s, "type",      typ)  ||
        !extractJsonString(s, "challenge", chal) ||
        !extractJsonString(s, "origin",    orig))
    {
        error_out = "clientDataJSON missing required fields";
        return false;
    }
    if (typ != expectedType) {
        error_out = "clientDataJSON.type mismatch (got '" + typ + "')";
        return false;
    }
    // Constant-time compare on challenge to avoid leaking partial guesses.
    if (chal.size() != expectedChallenge_b64u.size() ||
        sodium_memcmp(chal.data(), expectedChallenge_b64u.data(), chal.size()) != 0)
    {
        error_out = "challenge mismatch";
        return false;
    }
    if (orig != expectedOrigin) {
        error_out = "origin mismatch (got '" + orig + "')";
        return false;
    }
    return true;
}

} // namespace

// ============================ Public API ============================

std::string base64UrlEncode(const unsigned char* data, std::size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= len) {
        unsigned a = data[i];
        unsigned b = data[i + 1];
        unsigned c = data[i + 2];
        out.push_back(kB64Alphabet[(a >> 2) & 0x3F]);
        out.push_back(kB64Alphabet[((a & 0x3) << 4) | ((b >> 4) & 0xF)]);
        out.push_back(kB64Alphabet[((b & 0xF) << 2) | ((c >> 6) & 0x3)]);
        out.push_back(kB64Alphabet[c & 0x3F]);
        i += 3;
    }
    if (i < len) {
        unsigned a = data[i];
        unsigned b = (i + 1 < len) ? data[i + 1] : 0u;
        out.push_back(kB64Alphabet[(a >> 2) & 0x3F]);
        out.push_back(kB64Alphabet[((a & 0x3) << 4) | ((b >> 4) & 0xF)]);
        if (i + 1 < len) out.push_back(kB64Alphabet[(b & 0xF) << 2]);
    }
    return out;
}

bool base64UrlDecode(const std::string& in, std::vector<unsigned char>& out)
{
    out.clear();
    out.reserve(in.size() * 3 / 4);
    std::uint32_t buf = 0;
    int           bits = 0;
    for (char c : in) {
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int v = b64Index(c);
        if (v < 0) return false;
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
        }
    }
    return true;
}

std::string makeChallenge()
{
    unsigned char raw[32];
    randombytes_buf(raw, sizeof(raw));
    return base64UrlEncode(raw, sizeof(raw));
}

std::optional<RegistrationResult> finishRegistration(
    const std::string& clientDataJSON_b64u,
    const std::string& attestationObject_b64u,
    const std::string& expected_challenge_b64u,
    const std::string& rpId,
    const std::string& origin,
    std::string&       error_out)
{
    std::vector<unsigned char> clientDataJSON, attObj;
    if (!base64UrlDecode(clientDataJSON_b64u, clientDataJSON)) {
        error_out = "clientDataJSON: invalid base64url"; return std::nullopt;
    }
    if (!base64UrlDecode(attestationObject_b64u, attObj)) {
        error_out = "attestationObject: invalid base64url"; return std::nullopt;
    }

    if (!checkClientData(clientDataJSON, "webauthn.create",
                         expected_challenge_b64u, origin, error_out)) {
        return std::nullopt;
    }

    std::string fmt;
    std::vector<unsigned char> authDataBytes;
    if (!parseAttestationObject(attObj, fmt, authDataBytes)) {
        error_out = "malformed attestation object"; return std::nullopt;
    }
    if (fmt != "none") {
        // We accept only `none` attestation by policy — see the header
        // comment. The browser sends `none` whenever no attestation is
        // requested, which is what `attestation: 'none'` triggers on
        // the client side.
        error_out = "unsupported attestation format '" + fmt + "'";
        return std::nullopt;
    }

    AuthData ad;
    if (!parseAuthData(authDataBytes, ad, /*needCred=*/true)) {
        error_out = "malformed authenticatorData"; return std::nullopt;
    }

    // rpIdHash must equal SHA-256(rpId). This binds the credential to
    // this exact RP and stops cross-origin replay.
    auto expected = sha256(
        reinterpret_cast<const unsigned char*>(rpId.data()), rpId.size());
    if (ad.rpIdHash.size() != expected.size() ||
        sodium_memcmp(ad.rpIdHash.data(), expected.data(), expected.size()) != 0)
    {
        error_out = "rpIdHash mismatch"; return std::nullopt;
    }
    if ((ad.flags & 0x01) == 0) {
        error_out = "user presence flag not set"; return std::nullopt;
    }

    // We sanity-check the COSE key is one of the algorithms we know how
    // to verify signatures against, so we don't accept credentials we
    // cannot later authenticate.
    CoseKey ck;
    if (!parseCoseKey(ad.cosePublicKey, ck)) {
        error_out = "COSE key parse error"; return std::nullopt;
    }
    if (!(ck.alg == -7 || ck.alg == -8)) {
        error_out = "unsupported COSE alg " + std::to_string(ck.alg);
        return std::nullopt;
    }

    RegistrationResult res;
    res.credential_id_b64u =
        base64UrlEncode(ad.credentialId.data(), ad.credentialId.size());
    res.cose_public_key = ad.cosePublicKey;
    res.sign_count      = ad.signCount;
    return res;
}

std::optional<AuthenticationResult> finishAuthentication(
    const std::string&                  clientDataJSON_b64u,
    const std::string&                  authenticatorData_b64u,
    const std::string&                  signature_b64u,
    const std::string&                  expected_challenge_b64u,
    const std::string&                  rpId,
    const std::string&                  origin,
    const std::vector<unsigned char>&   stored_cose_public_key,
    std::uint32_t                       stored_sign_count,
    std::string&                        error_out)
{
    std::vector<unsigned char> clientDataJSON, authData, signature;
    if (!base64UrlDecode(clientDataJSON_b64u, clientDataJSON)) {
        error_out = "clientDataJSON: invalid base64url"; return std::nullopt;
    }
    if (!base64UrlDecode(authenticatorData_b64u, authData)) {
        error_out = "authenticatorData: invalid base64url"; return std::nullopt;
    }
    if (!base64UrlDecode(signature_b64u, signature)) {
        error_out = "signature: invalid base64url"; return std::nullopt;
    }
    if (!checkClientData(clientDataJSON, "webauthn.get",
                         expected_challenge_b64u, origin, error_out)) {
        return std::nullopt;
    }

    AuthData ad;
    if (!parseAuthData(authData, ad, /*needCred=*/false)) {
        error_out = "malformed authenticatorData"; return std::nullopt;
    }
    auto expectedHash = sha256(
        reinterpret_cast<const unsigned char*>(rpId.data()), rpId.size());
    if (ad.rpIdHash.size() != expectedHash.size() ||
        sodium_memcmp(ad.rpIdHash.data(), expectedHash.data(), expectedHash.size()) != 0)
    {
        error_out = "rpIdHash mismatch"; return std::nullopt;
    }
    if ((ad.flags & 0x01) == 0) {
        error_out = "user presence flag not set"; return std::nullopt;
    }

    CoseKey ck;
    if (!parseCoseKey(stored_cose_public_key, ck)) {
        error_out = "stored COSE key parse error"; return std::nullopt;
    }

    auto signedData = assertionSignedData(authData, std::string(
        reinterpret_cast<const char*>(clientDataJSON.data()), clientDataJSON.size()));

    bool ok = false;
    if (ck.alg == -7)      ok = verifyEs256(ck, signedData, signature);
    else if (ck.alg == -8) ok = verifyEd25519(ck, signedData, signature);
    else { error_out = "unsupported alg"; return std::nullopt; }

    if (!ok) {
        error_out = "signature verification failed";
        return std::nullopt;
    }

    // Sign-counter regression check. The spec allows authenticators that
    // never increment (always 0). Reject only on genuine regression.
    if (ad.signCount != 0 && ad.signCount <= stored_sign_count) {
        error_out = "signCount regression — possible cloned authenticator";
        return std::nullopt;
    }
    return AuthenticationResult{ad.signCount};
}

} // namespace webauthn
