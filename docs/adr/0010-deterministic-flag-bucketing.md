# ADR 0010 — `sha256(key:user_id) % 100` for feature flag bucketing

## Context

The feature-flag system in `helpers/Flags` needs to answer "is this
flag on for this user?" for a rollout percentage between 0 and 100.
Three constraints matter:

- **Deterministic.** The same `(flag, user)` pair must land in the
  same bucket forever — flipping a flag from 30 % to 50 % should
  GROW the cohort, not reshuffle it. If user U is in the 30 % at
  rollout=30, U must still be in at rollout=50.
- **Uniform.** Across all users, exactly N% must fall into bucket
  `< N` for a healthy A/B sample.
- **Cheap.** The check runs on hot paths; it must not allocate or
  syscall.

Possible approaches:

| Approach | Why not |
|----------|---------|
| `random() < N/100` | Not deterministic — every call re-rolls. |
| `user_id % 100` | Deterministic + cheap but NOT uniform — modulus depends on the distribution of user IDs (often sequential, so the first 30 % of users would always get every flag). |
| `hash(user_id) % 100` | Uniform but the SAME user lands in the same bucket for ALL flags. Two flags at 30 % would target identical cohorts, defeating the point of independent rollouts. |
| `hash(flag_key, user_id) % 100` | Uniform AND per-flag — the cohorts for `new_feed @ 30 %` and `new_search @ 30 %` are independent samples. **Chosen.** |
| MurmurHash3 / xxHash | Faster than SHA-256 but adds a dependency (we already pull libsodium for password hashing, so SHA-256 is free). |

## Decision

`bucket(flag, user) = (sha256(flag_key + ":" + user_id_decimal)[0..7] as hex) % 100`

A flag with `rollout_percent = N` enables exactly buckets `[0, N)`.

Properties:

- **Deterministic.** SHA-256 is, well, SHA-256.
- **Per-flag.** The separator `:` between key and user id makes
  `(key="a1", uid="1")` hash distinctly from `(key="a", uid="11")`.
- **Monotone.** Raising rollout from N to N+k only enables new
  users (those whose bucket landed in `[N, N+k)`) without
  re-shuffling who was already in.
- **Anonymous-safe.** `user_id = 0` (no session) buckets against
  its own value. A 50 % rollout still serves the variant to 50 %
  of guests — not 0 %, not 100 %.

Implementation uses the first 8 hex chars (32 bits) modulo 100;
bias is negligible at this width.

## Consequences

- **Hashing cost is non-trivial.** SHA-256 over a ~30-byte input
  on modern CPUs is ~1 µs. For a request that evaluates 5 flags,
  that's 5 µs total — well below request budget — but a request
  that walked thousands of flags would care. We bound the table
  size in practice (currently a few flags) and the snapshot cache
  in `helpers/Flags` avoids re-hashing within a process.
- **Verification is trivial in a bash one-liner**, which is what
  we used to smoke-test in production:
  ```bash
  hex=$(echo -n "new_feed:0" | sha256sum | head -c 8)
  bucket=$(( 0x$hex % 100 ))
  ```
  Same bucket the C++ code computes.
- **Bucketing keys are NOT salt-stable across rename.** Renaming
  `new_feed` to `redesigned_feed` reshuffles every user. That's
  the right behaviour — a renamed flag is a different flag — but
  worth knowing if you ever want "rename the key without
  disturbing the cohort", which would require a separate
  `bucketing_key` column.
