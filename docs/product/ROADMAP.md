# Product roadmap — 16 November 2026

The existing product uses C++20/Drogon, PostgreSQL and Vue 3/TypeScript.
Keep the existing identity and protect the live VPS throughout implementation.
Each completed stage gets a separate commit with the repository owner's existing
identity, without generated coauthor trailers. Test writes use disposable data.

## 1. Security and production foundations

- Audit HTTP/session/CSRF trust boundaries, private response caching, uploads,
  moderation/draft visibility, proxy configuration and dependencies.
- Apply response protections to static files and early errors as well as APIs.
- Reject browser writes from another origin, including pre-login/2FA routes.
- Explicitly prevent storage of authentication, messages and account responses.
- Verify native framework version and available fixes; align native and Docker
  dependencies after checking compatibility. Never replace host-wide libraries.
- Document evidence, severity, residual risks and an incident response runbook.

Acceptance: regression coverage for each security change; live reads only during
verification; readiness and rollback available before a service restart.

## 2. Discovery and retention

- Discovery feed with popular and discussed posts, bounded pagination, readable
  metrics, topic filters and shareable URLs.
- Introduce a clear welcome section and discovery controls; preserve the following
  feed. Provide explicit load-more and retry actions alongside automatic loading.
- Ensure hidden posts and drafts cannot enter discovery, counts or caches.

Acceptance: real database sorting and filters, predictable pagination, empty/error
states, keyboard controls, English/Romanian copy, 320px through desktop layouts.

## 3. Reader experience

- Reading preferences: text size, comfortable measure and distraction-free mode.
- Persist presentation settings only; keep private post content out of browser
  storage. Offer print layout and reliable back-to-top navigation.
- Harden navigation races so a slow previous article cannot replace the current
  one. Keep sharing, comments, TOC and live subscriptions working.

Acceptance: desktop/mobile browser checks, reduced-motion support, preferences
survive reload, blocked browser storage does not break the article.

## 4. SEO and CI, after product changes

- Validate sitemap/feed/canonical/structured-data visibility and timestamps.
- Extend the existing CI with focused regression checks for the new features.
- Audit development dependencies; pin changes rather than using blind upgrades.
- Build and validate the production frontend; retain old hashed assets for open
  tabs, deploy the verified binary with a rollback copy and check readiness.

## Next releases

Prioritize by observed usage and user feedback, rather than adding empty screens:

1. Author profiles with bio/social links and publication collections/series.
2. Bookmark collections, reading queue and optional completed-reading state.
3. Per-user notification preferences, digest email with unsubscribe and throttling.
4. Comment sorting, mention notifications, user blocking and mute controls.
5. Editor image alt-text guidance, unsaved-change recovery and revision history.
6. Accessible command palette and quick navigation.
7. Account email preferences and consent/retention information.
8. Moderation appeal workflow and actionable abuse monitoring.
9. Reproducible preview deployments and tested backup restore automation.

Gates: a newsletter needs a deliverability/unsubscribe design; revision history
needs a storage/retention budget; public uploads need ownership and deletion
semantics. These are product design tasks, not placeholder functionality.

## Release verification

Stages 1–4 are implemented: security and native dependency fixes, discovery,
reader controls, responsive public articles, API documentation and CI dependency
alignment. Local backend, browser, build, lint and supply-chain checks passed;
production rollout details are recorded in the security review. The next-release
items above remain a prioritized backlog, not claimed completed features.
