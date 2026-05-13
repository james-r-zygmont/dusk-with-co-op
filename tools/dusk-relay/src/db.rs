//! SQLite storage for sessions, canonical saves, and per-player consumable
//! profiles. See `migrations/0001_initial.sql` for the schema.

use anyhow::{Context, Result};
use rusqlite::Connection;
use std::path::Path;
use std::sync::{Arc, Mutex};

const MIGRATION_0001: &str = include_str!("../migrations/0001_initial.sql");

/// Thread-safe handle to the relay's SQLite database.
///
/// Wraps a single connection behind `Arc<Mutex<_>>`. Axum handlers should hop
/// to a blocking pool (`tokio::task::spawn_blocking`) before grabbing the
/// lock — SQLite calls are synchronous and would otherwise starve the async
/// runtime. With only two clients per session this is plenty; revisit if we
/// ever take real traffic.
#[derive(Clone)]
pub struct Db {
    conn: Arc<Mutex<Connection>>,
}

impl Db {
    pub fn open(path: &Path) -> Result<Self> {
        let conn = Connection::open(path)
            .with_context(|| format!("opening sqlite database at {}", path.display()))?;

        // WAL gives us crash-safe reader-writer concurrency; NORMAL sync is
        // safe under WAL for everything except a single edge case (power
        // loss between commit and WAL checkpoint). Save commits are wrapped
        // in explicit transactions in the save-write path so the durability
        // tradeoff there is clearer.
        conn.pragma_update(None, "journal_mode", "WAL")?;
        conn.pragma_update(None, "synchronous", "NORMAL")?;
        conn.pragma_update(None, "foreign_keys", "ON")?;

        conn.execute_batch(MIGRATION_0001)
            .context("running 0001_initial migration")?;

        Ok(Self {
            conn: Arc::new(Mutex::new(conn)),
        })
    }

    /// Run a closure with exclusive access to the underlying connection.
    /// Intended to be called from inside `tokio::task::spawn_blocking`.
    pub fn with_conn<F, T>(&self, f: F) -> Result<T>
    where
        F: FnOnce(&mut Connection) -> Result<T>,
    {
        let mut guard = self.conn.lock().expect("dusk-relay: db mutex poisoned");
        f(&mut *guard)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn open_creates_expected_schema() {
        let tmp = std::env::temp_dir().join("dusk-relay-schema-test.db");
        for suffix in ["", "-wal", "-shm"] {
            let _ = fs::remove_file(format!("{}{}", tmp.display(), suffix));
        }

        let db = Db::open(&tmp).expect("open");

        let tables = db
            .with_conn(|conn| {
                let mut stmt = conn.prepare(
                    "SELECT name FROM sqlite_master WHERE type='table' \
                     AND name NOT LIKE 'sqlite_%' ORDER BY name",
                )?;
                let names: Vec<String> = stmt
                    .query_map([], |row| row.get::<_, String>(0))?
                    .collect::<rusqlite::Result<_>>()?;
                Ok(names)
            })
            .expect("query");

        assert_eq!(tables, vec!["player_profiles", "saves", "sessions"]);
    }
}
