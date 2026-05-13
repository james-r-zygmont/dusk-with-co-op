//! Typed HTTP errors. Each variant becomes a structured JSON response with a
//! stable `"error"` discriminator the client can match on (e.g. surface an
//! "iso mismatch" modal in the lobby UI).

use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde_json::json;

#[derive(Debug, thiserror::Error)]
pub enum AppError {
    #[error("session not found")]
    SessionNotFound,

    #[error("session full")]
    SessionFull,

    #[error("iso hash mismatch")]
    IsoMismatch { server_hash: Vec<u8> },

    #[error("bad request: {0}")]
    BadRequest(String),

    #[error(transparent)]
    Internal(#[from] anyhow::Error),
}

impl From<rusqlite::Error> for AppError {
    fn from(e: rusqlite::Error) -> Self {
        AppError::Internal(e.into())
    }
}

impl From<tokio::task::JoinError> for AppError {
    fn from(e: tokio::task::JoinError) -> Self {
        AppError::Internal(e.into())
    }
}

impl IntoResponse for AppError {
    fn into_response(self) -> Response {
        let (status, body) = match &self {
            AppError::SessionNotFound => (
                StatusCode::NOT_FOUND,
                json!({ "error": "session_not_found" }),
            ),
            AppError::SessionFull => (
                StatusCode::CONFLICT,
                json!({ "error": "session_full" }),
            ),
            AppError::IsoMismatch { server_hash } => (
                StatusCode::CONFLICT,
                json!({
                    "error": "iso_mismatch",
                    "server_hash": hex::encode(server_hash),
                }),
            ),
            AppError::BadRequest(msg) => (
                StatusCode::BAD_REQUEST,
                json!({ "error": "bad_request", "message": msg }),
            ),
            AppError::Internal(e) => {
                tracing::error!(error = ?e, "internal error");
                (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    json!({ "error": "internal" }),
                )
            }
        };
        (status, Json(body)).into_response()
    }
}
