# ADR 0004 — The Helm chart does not bundle PostgreSQL

## Context

A common Helm pattern is to ship a chart that installs the app + its
database in one shot: `helm install blog ./chart`, get a Postgres
StatefulSet plus the app talking to it. Convenient for `kind` /
local dev, sometimes also for early-stage production.

Counter-pressure: production-grade Postgres needs

- a real backup + restore story (PITR, off-site copies),
- monitoring + alerting on lag, slow queries, connection saturation,
- well-tuned `postgresql.conf` for the workload's CPU / memory,
- a known-good upgrade story across PG major versions,
- a connection pooler (PgBouncer) sized to the app's worker pool.

A chart that bundles Postgres typically does NONE of those. The
result is a "looks like prod, isn't prod" deploy that gets sticky
fast: users put real data in it, the chart can't grow into a real
operator without rewriting half itself, and migrating off it later
is painful.

## Decision

The chart's default state is **no bundled Postgres**. The values
file's `database.*` block points the app at an external host
(`postgres.db.svc.cluster.local`, an RDS endpoint, etc.).

For the "I just want to try it on a kind cluster" use case, there's
an opt-in `cnpg.enabled` knob that templates a
`postgresql.cnpg.io/v1 Cluster` CR. That delegates to CNPG (a real
operator with backup primitives, replication, monitoring hooks).
The chart only DECLARES the Cluster — the CNPG operator itself is
the user's job to install, on the assumption that anyone who's
ready to run a stateful service is ready to install one operator.

## Consequences

- The default `helm install` will fail without `database.password`
  (or `database.existingSecret`). The chart's `secret.yaml` uses
  `{{ required "..." .Values.database.password }}` to make the
  error message clear.
- Setting up local-dev with this chart is two helm releases
  (operator + app) instead of one. Documented in
  `chart/drogon-blog/README.md`.
- A future change could add a `bitnami/postgresql` dependency as a
  second-tier opt-in. So far we haven't needed it: every reviewer
  who tried the chart was happy to point it at an existing PG.
