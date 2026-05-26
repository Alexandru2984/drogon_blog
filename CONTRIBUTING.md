# Contributing

Thanks for opening the file — that already puts you ahead of most
readers. This document is short on purpose; the goal is "I can get
the project building and a small change shipped in 30 minutes."

## What's in the repo

| Area | Lives in |
|------|----------|
| Drogon HTTP backend (C++20) | `controllers/`, `helpers/`, `models/`, `main.cc` |
| gRPC service (read-only) | `controllers/grpc/`, `proto/blog.proto` |
| SPA (Vue 3 + Vite + Pinia) | `frontend_app/`, output goes to `public/` |
| Database migrations | `migrations/0001_*.sql` … `0007_*.sql` |
| Helm chart | `chart/drogon-blog/` |
| OpenAPI 3.1 spec + Redoc viewer | `openapi/blog.openapi.yaml` |
| Playwright E2E | `e2e/tests/*.spec.ts` |
| k6 load + perf regression guard | `bench/scenarios.js` |
| Storybook (Vue components) | `frontend_app/.storybook/`, `src/components/*.stories.ts` |
| Backup / restore scripts | `scripts/backup.sh`, `scripts/restore.sh` |
| Architecture Decision Records | [`docs/adr/`](docs/adr/README.md) |

## Build prerequisites

Ubuntu 24.04 / Debian / Alpine names below. Drogon's official Docker
image (`drogonframework/drogon:latest`) carries libdrogon + its
direct deps; everything else is an apt install away.

```bash
sudo apt-get install -y --no-install-recommends \
    build-essential pkg-config cmake \
    libsodium-dev libcurl4-openssl-dev libssl-dev libbrotli-dev \
    libvips-dev libcmark-gfm-dev libcmark-gfm-extensions-dev \
    libpq-dev libhiredis-dev \
    libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
    postgresql-client
```

`libhiredis-dev` + the grpc stack are optional — CMake feature-detects
them and ships the binary either way (the affected features compile
to no-ops without them). See ADRs [0007](docs/adr/0007-hiredis-sync-presence.md)
and [0009](docs/adr/0009-readonly-grpc.md) for the rationale.

For the SPA: Node 24+ and a recent npm.

## Running locally

The fastest loop is docker-compose:

```bash
# Brings up Postgres + the C++ app + serves the SPA.
DB_PASSWORD=local_dev_pw docker compose up -d --build
curl http://127.0.0.1:8092/healthz
```

The compose file uses the Dockerfile in repo root (multi-stage:
frontend build → backend build → slim runtime image). First build is
slow (Drogon-from-source); rebuilds reuse layers.

To run the binary directly against an externally-managed Postgres:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
DB_HOST=… DB_PORT=… DB_NAME=… DB_USER=… DB_PASSWORD=… ./build/blog
```

`migrations/apply.sh` is idempotent and refuses to silently re-run a
migration whose SHA changed — never edit an already-applied file;
write a new one.

For the SPA dev server (HMR, proxies API to localhost:8092):

```bash
cd frontend_app
npm ci
npm run dev          # localhost:5173
```

## Tests

| Suite | Run with | Covers |
|-------|----------|--------|
| Integration (C++) | `./build/test/blog_test` | Controllers + helpers against a `blog_test_db` DB |
| Frontend lint / typecheck / build | `cd frontend_app && npm run lint && npm run build` | ESLint + vue-tsc + Vite build |
| Storybook bundle | `cd frontend_app && npm run build-storybook` | Stories under `src/components/*.stories.ts` |
| E2E (Playwright) | `cd e2e && npm ci && npx playwright test` | Full SPA against the docker-compose stack |
| Perf regression | `cd bench && K6_STRICT=1 k6 run -e SCENARIO=feed_read scenarios.js` | Defends D16/D17 baselines |
| Static analysis | `clang-tidy -p build controllers/foo.cc`<br>`cppcheck --enable=warning,style,performance,portability …` | CI runs this on every push |
| OpenAPI lint | `npx @stoplight/spectral-cli lint openapi/blog.openapi.yaml --ruleset .spectral.yaml` | Schema validity |
| Helm chart | `helm lint chart/drogon-blog`<br>`helm template ci chart/drogon-blog --set database.password=ci-only \| kubeconform -strict` | Template + Kubernetes schema |
| Backup → restore round-trip | `bash scripts/backup.sh && bash scripts/restore.sh /var/backups/drogon-blog/blog-*.dump --force` | Re-exercised in CI on every push |

The full CI matrix is at [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Code style

C++:

- C++20, four-space indent. `.clang-format` is in repo root —
  formatting is automatic, no debates.
- `helpers::camelCase` for functions, `g_camelCase` for file-static
  globals, `ALL_CAPS` reserved for macros (which we mostly avoid).
- Comments lean toward "why", not "what". Code shows what; comments
  should explain the constraint, the alternative we rejected, or the
  invariant a reader couldn't reconstruct from the AST.

TypeScript / Vue:

- Composition API, `<script setup lang="ts">`. No Options API.
- Strict ESLint via `npm run lint` (`--max-warnings 0` in CI).

Bash:

- `set -euo pipefail` and `#!/bin/sh` POSIX where possible.

SQL:

- One statement per line. No mixed `SELECT` + DML. Backticks never;
  PostgreSQL only. Tables and columns are `snake_case`.

## Commit messages

```
Short imperative subject under 70 chars

Optional paragraph explaining the WHY. The `git diff` already shows
the what — the message belongs in commit history because of the
context that won't survive in code comments.

- Bullet points are fine.
- Reference issue IDs as `Fixes #123` when the link adds context.
```

Multi-paragraph descriptions are encouraged for non-trivial changes
— search the repo's `git log` for examples (every direction has a
detailed commit explaining tradeoffs and which alternatives were
considered).

## Pull request flow

1. Branch from `main` (`git switch -c <topic>`).
2. Tests pass locally; if you touched a backend path, run the C++
   integration tests; if you touched the SPA, run lint + build.
3. Push, open a PR, link any related issue.
4. CI must be green. The matrix runs in parallel: backend, frontend,
   static-analysis, helm, e2e, perf-regression, openapi lint, docker
   build (amd64 + arm64), Trivy.
5. Squash-merge by default. The squashed message should follow the
   commit conventions above; reword if the branch had noisy WIP
   commits.

## Adding a new direction

When the change is large enough to span multiple files / commits:

1. Open the PR with a short proposal in the description.
2. If the decision space has plausible alternatives a reader would
   second-guess, write an ADR in `docs/adr/`. See
   [the ADR README](docs/adr/README.md) for the format.
3. Bump `TODO.md` if the change closes a backlog item.

## Where to ask

- Open an issue on GitHub with the `question` label.
- Or reach the author at the contact on the [project README](README.md).
