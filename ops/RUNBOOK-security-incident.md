# Security incident response

Owner: VPS/site operator. Last reviewed: 2026-11-16. Tabletop/restore: not yet
recorded. Store real contact details outside the public repository.

1. Record UTC time, observed symptoms, affected route and request IDs. Preserve
   relevant nginx, application, audit and systemd logs in restricted storage.
   Do not paste credentials, message bodies or session cookies into tickets.
2. Bound the incident: account abuse, content abuse, data disclosure, or host
   compromise. For host compromise isolate the host at the provider/firewall;
   use a clean administrative device. Avoid destroying evidence.
3. For an account incident use the existing session revocation/ban controls.
   For a release regression restore the previously running binary and check
   `/readyz`. For a disclosure shut off the affected public surface until fixed.
4. Determine which secrets could have been read. Rotate those secrets at the
   provider and application, revoke affected sessions, and verify old access
   fails. Preserve TOTP key recovery materials; losing the key breaks enrolled
   users' factors. Do not blindly delete the key or database.
5. Restore from a known-good release/backup in an isolated environment using
   RUNBOOK-backup-restore.md; confirm integrity and visibility controls before
   reopening. Document any unverified restore steps.
6. Assess affected people/data and notification obligations with the operator's
   qualified adviser. Keep an incident timeline and the basis for decisions.
7. After recovery add a regression test, review alert coverage, and run a short
   tabletop: stolen session, leaked draft, failed deployment, lost host.

Communication template (fill facts only):

> We detected [event] at [UTC time]. [Known data/services] were affected between
> [times]. We have [containment actions]. Users should [specific action, if any].
> The next update will be at [UTC time], via [operator-controlled channel].

Operational checks: readiness alerts must reach the operator; the operator must
be able to revoke sessions and locate the rollback binary; an isolated restore
must actually succeed. A written runbook alone does not verify these checks.
