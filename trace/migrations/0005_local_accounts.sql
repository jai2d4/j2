-- Migration 0005 — local accounts.
--
-- Until now TRACE trusted whoever opened it. The users table existed and named
-- the operator in the audit trail, but password_hash and password_salt were
-- reserved columns that nothing ever wrote: there was no login, and no way to
-- tell one operator from another beyond the name the operating system reported.
--
-- For software whose whole purpose is saying who did what to a piece of
-- evidence, that is a gap in the chain of custody, not just a missing feature.
--
-- The columns added here record how a credential was derived, not just the
-- derived value. Work factors have to rise over time, and an account whose hash
-- predates a raised factor must be identifiable so it can be upgraded on the
-- next successful login. Storing only the digest would make that impossible.

ALTER TABLE users ADD COLUMN password_algorithm TEXT;
ALTER TABLE users ADD COLUMN password_iterations INTEGER;
ALTER TABLE users ADD COLUMN password_changed_at TEXT;

-- Set when an administrator creates or resets an account. The operator must
-- choose their own secret before they can do anything, so that no credential an
-- administrator knows is ever the one attributed to another person's actions.
ALTER TABLE users ADD COLUMN must_change_password INTEGER NOT NULL DEFAULT 0;

-- Brute-force resistance. An offline attacker with the database file is slowed
-- by the work factor; an attacker at the keyboard is stopped by these.
ALTER TABLE users ADD COLUMN failed_attempts INTEGER NOT NULL DEFAULT 0;
ALTER TABLE users ADD COLUMN locked_until TEXT;
ALTER TABLE users ADD COLUMN last_login_at TEXT;

CREATE INDEX idx_users_username_active ON users(username, active);
