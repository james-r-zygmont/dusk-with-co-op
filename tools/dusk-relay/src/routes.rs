//! REST endpoints for session lifecycle.
//!
//! See the plan §D for the full surface; M2 ships the three handlers below
//! plus `GET /v1/version` from `main`. Saves CRUD lands in M4 (the
//! placeholder save row created here is an empty save_codec blob).

use axum::{
    extract::{Path, Query, State},
    Json,
};
use rusqlite::params;
use serde::{Deserialize, Serialize};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::auth::{self, SESSION_CODE_LEN};
#[allow(unused_imports)]
use crate::db::Db;
use crate::error::AppError;
use crate::save_codec;
use crate::AppState;

const ISO_HASH_LEN: usize = 16;

#[derive(Deserialize)]
pub struct CreateSessionRequest {
    pub display_name: String,
    pub iso_hash: String,
}

#[derive(Serialize)]
pub struct CreateSessionResponse {
    pub session_code: String,
    pub host_token: String,
    pub player_index: u8,
}

#[derive(Deserialize)]
pub struct JoinSessionRequest {
    pub display_name: String,
    pub iso_hash: String,
}

#[derive(Serialize)]
pub struct JoinSessionResponse {
    pub guest_token: String,
    pub player_index: u8,
}

#[derive(Serialize)]
pub struct SessionStatusResponse {
    pub code: String,
    pub iso_hash: String,
    pub created_at: i64,
    pub last_activity_at: i64,
}

pub async fn create_session(
    State(state): State<AppState>,
    Json(req): Json<CreateSessionRequest>,
) -> Result<Json<CreateSessionResponse>, AppError> {
    let iso_hash = parse_iso_hash(&req.iso_hash)?;
    let now = unix_now_ms();

    let session_code = auth::gen_session_code();
    let host_token = auth::gen_token();
    let guest_token = auth::gen_token();

    let db = state.db.clone();
    let code_for_blocking = session_code.clone();
    let host_token_for_blocking = host_token.clone();
    tokio::task::spawn_blocking(move || -> Result<(), AppError> {
        db.with_conn(|conn| {
            let tx = conn.transaction()?;
            tx.execute(
                "INSERT INTO saves (blob, schema_version, save_version, updated_at) \
                 VALUES (?1, ?2, 0, ?3)",
                params![empty_save_blob(), save_codec::SCHEMA_VERSION, now],
            )?;
            let save_id = tx.last_insert_rowid();
            tx.execute(
                "INSERT INTO sessions \
                 (code, save_id, host_token, guest_token, iso_hash, created_at, last_activity_at) \
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6)",
                params![
                    &code_for_blocking,
                    save_id,
                    &host_token_for_blocking,
                    &guest_token,
                    iso_hash,
                    now,
                ],
            )?;
            tx.commit()?;
            Ok::<_, anyhow::Error>(())
        })?;
        Ok(())
    })
    .await??;

    tracing::info!(code = %session_code, "session created");
    Ok(Json(CreateSessionResponse {
        session_code,
        host_token,
        player_index: 0,
    }))
}

pub async fn join_session(
    State(state): State<AppState>,
    Path(code): Path<String>,
    Json(req): Json<JoinSessionRequest>,
) -> Result<Json<JoinSessionResponse>, AppError> {
    validate_code_shape(&code)?;
    let iso_hash = parse_iso_hash(&req.iso_hash)?;
    let now = unix_now_ms();

    let db = state.db.clone();
    let row: SessionRow = tokio::task::spawn_blocking(move || -> Result<SessionRow, AppError> {
        db.with_conn(|conn| -> anyhow::Result<SessionRow> {
            let row = conn
                .query_row(
                    "SELECT save_id, host_token, guest_token, iso_hash FROM sessions \
                     WHERE code = ?1",
                    params![&code],
                    |r| {
                        Ok(SessionRow {
                            save_id: r.get::<_, i64>(0)?,
                            host_token: r.get::<_, String>(1)?,
                            guest_token: r.get::<_, String>(2)?,
                            iso_hash: r.get::<_, Vec<u8>>(3)?,
                            code: code.clone(),
                        })
                    },
                )
                .map_err(|e| match e {
                    rusqlite::Error::QueryReturnedNoRows => {
                        anyhow::Error::new(NotFoundMarker {})
                    }
                    other => other.into(),
                })?;
            // Bump last_activity_at on every join attempt that finds the row.
            conn.execute(
                "UPDATE sessions SET last_activity_at = ?1 WHERE code = ?2",
                params![now, &row.code],
            )?;
            Ok(row)
        })
        .map_err(|e| match e.downcast::<NotFoundMarker>() {
            Ok(_) => AppError::SessionNotFound,
            Err(orig) => AppError::Internal(orig),
        })
    })
    .await??;

    if row.iso_hash != iso_hash {
        return Err(AppError::IsoMismatch {
            server_hash: row.iso_hash,
        });
    }

    Ok(Json(JoinSessionResponse {
        guest_token: row.guest_token,
        player_index: 1,
    }))
}

pub async fn get_session(
    State(state): State<AppState>,
    Path(code): Path<String>,
) -> Result<Json<SessionStatusResponse>, AppError> {
    validate_code_shape(&code)?;

    let db = state.db.clone();
    let code_for_blocking = code.clone();
    let status = tokio::task::spawn_blocking(move || -> Result<SessionStatusResponse, AppError> {
        db.with_conn(|conn| -> anyhow::Result<SessionStatusResponse> {
            conn.query_row(
                "SELECT iso_hash, created_at, last_activity_at FROM sessions WHERE code = ?1",
                params![&code_for_blocking],
                |r| {
                    Ok(SessionStatusResponse {
                        code: code_for_blocking.clone(),
                        iso_hash: hex::encode(r.get::<_, Vec<u8>>(0)?),
                        created_at: r.get::<_, i64>(1)?,
                        last_activity_at: r.get::<_, i64>(2)?,
                    })
                },
            )
            .map_err(|e| match e {
                rusqlite::Error::QueryReturnedNoRows => anyhow::Error::new(NotFoundMarker {}),
                other => other.into(),
            })
        })
        .map_err(|e| match e.downcast::<NotFoundMarker>() {
            Ok(_) => AppError::SessionNotFound,
            Err(orig) => AppError::Internal(orig),
        })
    })
    .await??;

    Ok(Json(status))
}

// --- dev helpers --------------------------------------------------------------

#[derive(Serialize)]
pub struct LatestSessionResponse {
    pub code: String,
}

// GET /v1/dev/latest-session  — returns the most-recently-created session code,
// or 404 if there are none. Used by the dev auto-connect path so a second
// client can discover the first client's session without a lobby UI. (The
// relay wipes all sessions on startup, so "most recent" is unambiguous within
// one relay run.)
pub async fn get_latest_session(
    State(state): State<AppState>,
) -> Result<Json<LatestSessionResponse>, AppError> {
    let db = state.db.clone();
    let code = tokio::task::spawn_blocking(move || -> Result<String, AppError> {
        db.with_conn(|conn| -> anyhow::Result<String> {
            conn.query_row(
                "SELECT code FROM sessions ORDER BY created_at DESC, rowid DESC LIMIT 1",
                [],
                |r| r.get::<_, String>(0),
            )
            .map_err(|e| match e {
                rusqlite::Error::QueryReturnedNoRows => anyhow::Error::new(NotFoundMarker),
                other => anyhow::Error::new(other),
            })
        })
        .map_err(map_lookup_err)
    })
    .await??;
    Ok(Json(LatestSessionResponse { code }))
}

// --- canonical save (M4 chunk 1) ---------------------------------------------

#[derive(Deserialize)]
pub struct SaveTokenQuery {
    pub token: String,
}

#[derive(Deserialize)]
pub struct PutSaveRequest {
    pub blob: String, // hex-encoded save_codec blob
}

#[derive(Serialize)]
pub struct PutSaveResponse {
    pub save_version: i64,
}

#[derive(Serialize)]
pub struct GetSaveResponse {
    pub blob: String, // hex-encoded save_codec blob
    pub save_version: i64,
}

// PUT /v1/sessions/{code}/save?token=...  — overwrite the canonical save blob.
// No optimistic-concurrency check yet (M4 chunk 3 adds CAS on save_version).
pub async fn put_session_save(
    State(state): State<AppState>,
    Path(code): Path<String>,
    Query(q): Query<SaveTokenQuery>,
    Json(req): Json<PutSaveRequest>,
) -> Result<Json<PutSaveResponse>, AppError> {
    validate_code_shape(&code)?;
    let blob = hex::decode(&req.blob).map_err(|_| AppError::BadRequest("blob must be hex".into()))?;
    let now = unix_now_ms();

    let db = state.db.clone();
    let code_for_log = code.clone();
    let new_version = tokio::task::spawn_blocking(move || -> Result<i64, AppError> {
        db.with_conn(|conn| -> anyhow::Result<i64> {
            let save_id = lookup_save_id_checked(conn, &code, &q.token)?;
            conn.execute(
                "UPDATE saves SET blob = ?1, save_version = save_version + 1, updated_at = ?2 \
                 WHERE id = ?3",
                params![blob, now, save_id],
            )?;
            let v: i64 = conn.query_row(
                "SELECT save_version FROM saves WHERE id = ?1",
                params![save_id],
                |r| r.get(0),
            )?;
            Ok(v)
        })
        .map_err(map_lookup_err)
    })
    .await??;

    tracing::info!(code = %code_for_log, version = new_version, "canonical save updated");
    Ok(Json(PutSaveResponse { save_version: new_version }))
}

// GET /v1/sessions/{code}/save?token=...
pub async fn get_session_save(
    State(state): State<AppState>,
    Path(code): Path<String>,
    Query(q): Query<SaveTokenQuery>,
) -> Result<Json<GetSaveResponse>, AppError> {
    validate_code_shape(&code)?;

    let db = state.db.clone();
    let resp = tokio::task::spawn_blocking(move || -> Result<GetSaveResponse, AppError> {
        db.with_conn(|conn| -> anyhow::Result<GetSaveResponse> {
            let save_id = lookup_save_id_checked(conn, &code, &q.token)?;
            let (blob, save_version): (Vec<u8>, i64) = conn.query_row(
                "SELECT blob, save_version FROM saves WHERE id = ?1",
                params![save_id],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )?;
            Ok(GetSaveResponse {
                blob: hex::encode(blob),
                save_version,
            })
        })
        .map_err(map_lookup_err)
    })
    .await??;

    Ok(Json(resp))
}

// --- helpers -----------------------------------------------------------------

#[derive(Debug, thiserror::Error)]
#[error("session not found")]
struct NotFoundMarker;

#[derive(Debug, thiserror::Error)]
#[error("forbidden")]
struct ForbiddenMarker;

// Resolve a session's save_id, requiring `token` to match the host or guest
// token. Runs inside a with_conn closure; failures are anyhow-wrapped marker
// errors that map_lookup_err turns back into typed AppErrors.
fn lookup_save_id_checked(
    conn: &rusqlite::Connection,
    code: &str,
    token: &str,
) -> anyhow::Result<i64> {
    let (save_id, host_token, guest_token): (i64, String, String) = conn
        .query_row(
            "SELECT save_id, host_token, guest_token FROM sessions WHERE code = ?1",
            params![code],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .map_err(|e| match e {
            rusqlite::Error::QueryReturnedNoRows => anyhow::Error::new(NotFoundMarker),
            other => anyhow::Error::new(other),
        })?;
    if token != host_token && token != guest_token {
        return Err(anyhow::Error::new(ForbiddenMarker));
    }
    Ok(save_id)
}

fn map_lookup_err(e: anyhow::Error) -> AppError {
    if e.downcast_ref::<NotFoundMarker>().is_some() {
        AppError::SessionNotFound
    } else if e.downcast_ref::<ForbiddenMarker>().is_some() {
        AppError::Forbidden
    } else {
        AppError::Internal(e)
    }
}

#[allow(dead_code)] // save_id and host_token consumed by the WS handler (M2 #15).
struct SessionRow {
    save_id: i64,
    host_token: String,
    guest_token: String,
    iso_hash: Vec<u8>,
    code: String,
}

fn parse_iso_hash(s: &str) -> Result<Vec<u8>, AppError> {
    let bytes = hex::decode(s).map_err(|_| AppError::BadRequest("iso_hash must be hex".into()))?;
    if bytes.len() != ISO_HASH_LEN {
        return Err(AppError::BadRequest(format!(
            "iso_hash must be {} bytes ({} hex chars)",
            ISO_HASH_LEN,
            ISO_HASH_LEN * 2
        )));
    }
    Ok(bytes)
}

fn validate_code_shape(code: &str) -> Result<(), AppError> {
    if code.len() != SESSION_CODE_LEN {
        return Err(AppError::BadRequest(format!(
            "session code must be {} chars",
            SESSION_CODE_LEN
        )));
    }
    Ok(())
}

fn empty_save_blob() -> Vec<u8> {
    save_codec::encode(&save_codec::SaveFields::new()).expect("empty save_codec encode")
}

fn unix_now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

