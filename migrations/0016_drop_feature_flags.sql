-- Remove the feature-flags subsystem.
--
-- Nothing consumed it: the frontend useFlag() composable was never called and
-- gated no UI, and no server code read a flag on any request path. The table,
-- its update/notify triggers and the flags-only notify function are dropped
-- here. set_updated_at() is shared with other tables and is left in place.
--
-- Idempotent: DROP TABLE ... CASCADE also removes the two triggers, and both
-- statements use IF EXISTS so the test harness (which re-applies every
-- migration on a fresh schema) can run this after 0006/0007 without error.
DROP TABLE IF EXISTS feature_flags CASCADE;
DROP FUNCTION IF EXISTS notify_flag_change();
