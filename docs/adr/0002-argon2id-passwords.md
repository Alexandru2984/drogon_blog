# ADR 0002 — Argon2id for password hashing

## Context

The auth path needs a hash that:

- Is **memory-hard** (resists GPU + ASIC attacks); plain bcrypt + a
  bigger work factor stops being enough as commodity ASIC mining
  rigs get cheap.
- Has a **constant-time verify** wrapper from a library that exists
  in apt — no rolling our own constant-time compare.
- Costs ~50–100 ms per verify on the production VPS, slow enough to
  cap online guessing at a few attempts per second per core.

Surveyed:

| Hash | Why not |
|------|---------|
| MD5 / SHA-1 / SHA-256 unsalted | Not a question; broken for password storage. |
| PBKDF2 | Not memory-hard; ASIC-vulnerable. NIST still allows it but argon2 is the modern pick. |
| bcrypt | Mature but capped at 72 bytes of input + memory cost can't be tuned independently of CPU cost. |
| scrypt | Also memory-hard but the parameters are harder to tune for a server profile vs the crypto-mining profile it was designed for. |
| Argon2d  | Optimised for crypto and resists GPU but is vulnerable to side-channel timing attacks because the access pattern depends on the password. |
| Argon2i  | Side-channel safe but weaker against TMTO attacks. |
| **Argon2id** | Hybrid of Argon2i (first half — side-channel safe) and Argon2d (second half — GPU resistant). RFC 9106's recommended default. |

## Decision

Argon2id via `crypto_pwhash_str{,_verify}` in libsodium.

- libsodium ships in Ubuntu / Debian / Alpine — no manual vendoring.
- The library's high-level wrapper picks parameters that hit roughly
  64 MiB memory + ~100 ms on a single CPU core, which is the right
  per-verify cost for a single-tenant blog.
- The encoded hash string carries the algorithm + params + salt, so
  rolling parameters later is just a comparison flag — no schema
  migration needed.

The wiring lives in `helpers/Security.cc::hashPassword` /
`verifyPassword`.

## Consequences

- Authentication is intentionally slow. Login latency is dominated
  by the ~80–120 ms Argon2id verify. We accept that; for a blog,
  login is rare-event traffic.
- Dummy-hash verify on missing users — `registerUser` runs a stub
  hash even on the "username already taken" path so the timing
  doesn't leak account existence. (CWE-208.)
- Rotating parameters means writing a "rehash on next successful
  verify" loop. The codebase doesn't do this yet — when libsodium
  bumps the recommended defaults, that's the trigger.
