-- TRACE schema version 2 — local account scaffolding.
--
-- Phase 0 does not authenticate anyone: it records the workstation operator so
-- audit rows name a real person. The table exists now so that agency
-- deployments can add credentials, roles and account lifecycle without
-- rewriting audit history or evidence rows.

CREATE TABLE users (
    id            TEXT PRIMARY KEY NOT NULL,
    username      TEXT NOT NULL UNIQUE,
    display_name  TEXT NOT NULL DEFAULT '',
    role          TEXT NOT NULL DEFAULT 'analyst',
    active        INTEGER NOT NULL DEFAULT 1,
    -- Reserved for the authentication phase. Phase 0 never writes these.
    password_hash TEXT,
    password_salt TEXT,
    created_at    TEXT NOT NULL,
    last_seen_at  TEXT
);

CREATE INDEX idx_users_active ON users(active);

-- Audit rows can now name the local account that produced them while keeping
-- the denormalised actor string for records written before accounts existed.
ALTER TABLE audit_events ADD COLUMN actor_user_id TEXT;
