-- dusk-relay schema v1.
--
-- `saves` is the canonical co-op save shared between the two players.
-- `player_profiles` holds the per-player consumables overlay (rupees, bombs,
--    arrows, current HP/magic, transform state) — see plan §C.
-- `sessions` binds a 6-char session code to a save plus the two opaque
--    auth tokens used over WS.
--
-- Note: SQLite's FK enforcement is off by default; we enable it per-connection
-- with `PRAGMA foreign_keys = ON`.

CREATE TABLE IF NOT EXISTS saves (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    blob           BLOB NOT NULL,
    schema_version INTEGER NOT NULL,
    save_version   INTEGER NOT NULL DEFAULT 0,
    updated_at     INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS sessions (
    code             TEXT PRIMARY KEY,
    save_id          INTEGER NOT NULL,
    host_token       TEXT NOT NULL,
    guest_token      TEXT NOT NULL,
    iso_hash         BLOB NOT NULL,
    created_at       INTEGER NOT NULL,
    last_activity_at INTEGER NOT NULL,
    FOREIGN KEY (save_id) REFERENCES saves(id)
);

CREATE TABLE IF NOT EXISTS player_profiles (
    save_id          INTEGER NOT NULL,
    player_index     INTEGER NOT NULL,
    consumables_blob BLOB NOT NULL,
    PRIMARY KEY (save_id, player_index),
    FOREIGN KEY (save_id) REFERENCES saves(id)
);
