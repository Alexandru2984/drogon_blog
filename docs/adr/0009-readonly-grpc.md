# ADR 0009 — gRPC surface stays read-only

## Context

The blog gained a parallel gRPC service (`blog.v1.BlogReader`) on
port 50051 alongside the REST listener on 8092. The service exposes
two RPCs:

- `GetPost(GetPostRequest) → Post`
- `ListPosts(ListPostsRequest) → ListPostsResponse`

It's a natural question: why not put writes there too? `CreatePost`,
`UpdatePost`, `DeleteComment`, `Login` would all be plausible RPCs.

## Decision

The gRPC surface is intentionally read-only. Writes stay on REST.

The reasoning:

- **Auth / CSRF / rate-limit / audit are wired once on REST.** All
  four are implemented via Drogon advices that bind to HTTP
  request shapes. Reproducing them for gRPC means writing
  interceptor equivalents AND keeping them in lockstep with REST.
  That's a lot of code in service of a feature (gRPC writes) we
  don't actually need.
- **Auth tokens on gRPC are different.** REST uses an `HttpOnly`
  session cookie + CSRF double-submit. gRPC uses metadata headers
  with bearer tokens. Reusing the cookie would require a gRPC
  interceptor that re-implements the SPA's CSRF flow on top of
  metadata — and now the cookie has to be flagged
  `Secure;HttpOnly;Same-Site=Lax` for browsers AND somehow be
  reachable from a non-browser gRPC client, which is a security
  smell.
- **The read use case is concrete.** Service-to-service callers
  (analytics ingestion, mirror jobs, future feeds) want efficient
  binary reads. The write use case is hypothetical for a personal
  blog.
- **Failure modes diverge.** A REST 400 has a stable JSON shape
  read by the SPA. A gRPC `INVALID_ARGUMENT` has a different shape
  with different translation rules. Two surfaces for the same
  error semantics means two surfaces to maintain.

## Consequences

- The gRPC server reuses Drogon's PG connection pool via
  `app().getDbClient()` from a non-Drogon thread. That's safe
  because Drogon's pool keys off SQL, not the calling thread.
- `controllers/grpc/BlogReaderService.cc` deliberately mirrors the
  REST `PostController::getAllPosts` SQL shape verbatim — row format
  is identical at the SQL level so the two surfaces never drift on
  what a `Post` actually contains.
- A future "write some-thing" request from a service-to-service
  caller should answer "use REST with an API key". If the answer
  ever has to be "we need gRPC writes for X", revisit this ADR.
- Bidirectional streaming RPCs (server-push for new messages) are
  also off the table for now — the WebSocket hub already covers
  that use case and a duplicate streaming surface is more code
  than it's worth.
