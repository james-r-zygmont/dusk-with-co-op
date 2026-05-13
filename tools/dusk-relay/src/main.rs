//! `dusk-relay` — co-op multiplayer relay for Dusk.
//!
//! M2 scaffolding: starts an HTTP/WS server on a configurable port, opens
//! and migrates an SQLite database, and exposes `GET /v1/version` so the
//! client can confirm the protocol-version handshake before we wire up
//! sessions, saves, and WS routing.

mod auth;
mod db;
mod error;
mod routes;
mod save_codec;
mod sessions;
mod wire;
mod ws;

use anyhow::Result;
use axum::{
    extract::State,
    routing::{get, post},
    Json, Router,
};
use clap::Parser;
use serde::Serialize;
use std::net::SocketAddr;
use std::path::PathBuf;
use tower_http::trace::TraceLayer;
use tracing_subscriber::EnvFilter;

use crate::db::Db;
use crate::sessions::SessionMap;

/// Wire-protocol version. Bump whenever a non-backward-compatible change
/// lands so mismatched clients/servers surface a typed error rather than
/// silently miscommunicating.
pub const PROTOCOL_VERSION: u16 = 1;

#[derive(Parser, Debug)]
#[command(name = "dusk-relay", about, version)]
struct Args {
    /// Address to bind the HTTP/WS server to.
    #[arg(long, default_value = "0.0.0.0:7777")]
    bind: SocketAddr,

    /// Path to the SQLite database file (created on first run).
    #[arg(long, default_value = "dusk-relay.db")]
    db: PathBuf,
}

#[derive(Clone)]
pub struct AppState {
    pub db: Db,
    pub sessions: SessionMap,
}

#[derive(Serialize)]
struct VersionResponse {
    protocol_version: u16,
    server_version: &'static str,
}

async fn get_version(State(_): State<AppState>) -> Json<VersionResponse> {
    Json(VersionResponse {
        protocol_version: PROTOCOL_VERSION,
        server_version: env!("CARGO_PKG_VERSION"),
    })
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("dusk_relay=info,tower_http=info")),
        )
        .init();

    let args = Args::parse();
    tracing::info!(bind = %args.bind, db = %args.db.display(), "starting dusk-relay");

    let db = Db::open(&args.db)?;
    let state = AppState {
        db,
        sessions: SessionMap::new(),
    };

    let app = Router::new()
        .route("/v1/version", get(get_version))
        .route("/v1/sessions", post(routes::create_session))
        .route("/v1/sessions/:code", get(routes::get_session))
        .route("/v1/sessions/:code/join", post(routes::join_session))
        .route("/v1/session/:code/ws", get(ws::ws_handler))
        .with_state(state)
        .layer(TraceLayer::new_for_http());

    let listener = tokio::net::TcpListener::bind(args.bind).await?;
    tracing::info!(addr = %listener.local_addr()?, "listening");
    axum::serve(listener, app).await?;
    Ok(())
}
