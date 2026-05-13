//! REST endpoints for session lifecycle.
//!
//! See the plan §D for the full surface; M2 ships the three handlers below
//! plus `GET /v1/version` from `main`. Saves CRUD lands in M4 (the
//! placeholder save row created here is an empty save_codec blob).

use axum::{
    extract::{Path, State},
    Json,
};
use rusqlite::params;
use serde::{Deserialize, Serialize};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::auth::{self, SESSION_CODE_LEN};
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

// --- helpers -----------------------------------------------------------------

#[derive(Debug, thiserror::Error)]
#[error("session not found")]
struct NotFoundMarker;

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

