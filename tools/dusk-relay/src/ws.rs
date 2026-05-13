//! WebSocket endpoint with Hello handshake and binary-frame routing.
//!
//! Flow:
//! 1. Client connects to `/v1/session/{code}/ws?token=...`.
//! 2. We look the session up in SQLite and resolve which slot the token
//!    grants access to (host or guest).
//! 3. Client sends a `Frame::Hello`. We validate the protocol version and
//!    ISO hash; mismatches reply with a typed error frame and close.
//! 4. We claim the slot in the live `SessionMap`, send a `Frame::HelloAck`,
//!    and split the socket into a writer task (mpsc → ws.send) and a reader
//!    loop (ws.recv → peer's mpsc).
//! 5. On disconnect (either direction), the slot is released; if both slots
//!    are empty the session entry is GC'd.

use axum::extract::{
    ws::{Message, WebSocket, WebSocketUpgrade},
    Path, Query, State,
};
use axum::response::IntoResponse;
use futures_util::{SinkExt, StreamExt};
use rusqlite::params;
use serde::Deserialize;
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::sync::mpsc;

use crate::sessions::{RegisterError, Slot};
use crate::wire::{Frame, Hello, HelloAck, HelloIntent, ISO_HASH_LEN};
use crate::{AppState, PROTOCOL_VERSION};

#[derive(Deserialize)]
pub struct WsQuery {
    token: String,
}

pub async fn ws_handler(
    ws: WebSocketUpgrade,
    Path(code): Path<String>,
    Query(q): Query<WsQuery>,
    State(state): State<AppState>,
) -> impl IntoResponse {
    ws.on_upgrade(move |socket| handle_socket(socket, state, code, q.token))
}

/// What we resolved from the session row + the token in the query string.
struct AuthorizedSlot {
    slot: Slot,
    iso_hash: Vec<u8>,
}

async fn handle_socket(mut socket: WebSocket, state: AppState, code: String, token: String) {
    let authorized = match authorize(&state, &code, &token).await {
        Ok(a) => a,
        Err(err_frame) => {
            let _ = socket.send(Message::Binary(err_frame.encode())).await;
            return;
        }
    };

    // Read the first binary frame; it must be Hello.
    let hello = match socket.recv().await {
        Some(Ok(Message::Binary(bytes))) => match Frame::decode(&bytes) {
            Ok(Frame::Hello(h)) => h,
            _ => return, // Client sent something other than Hello as first frame.
        },
        _ => return,
    };

    if let Err(err_frame) = validate_hello(&hello, &authorized) {
        let _ = socket.send(Message::Binary(err_frame.encode())).await;
        return;
    }

    // Claim our slot in the live session map.
    let (my_tx, mut my_rx) = mpsc::unbounded_channel::<Vec<u8>>();
    match state.sessions.register(&code, authorized.slot, my_tx).await {
        Ok(_) => {}
        Err(RegisterError::SlotTaken) => {
            let _ = socket
                .send(Message::Binary(Frame::ErrorSessionFull.encode()))
                .await;
            return;
        }
    }

    // Send HelloAck. If the socket has already died, just unwind.
    let ack = Frame::HelloAck(HelloAck {
        protocol_version: PROTOCOL_VERSION,
        player_index: authorized.slot.player_index(),
        server_time_ms: now_ms(),
    });
    if socket.send(Message::Binary(ack.encode())).await.is_err() {
        state.sessions.deregister(&code, authorized.slot).await;
        return;
    }

    tracing::info!(code = %code, slot = ?authorized.slot, "client joined live session");

    let (mut ws_sink, mut ws_stream) = socket.split();

    // Writer task: drain my_rx into the socket.
    let mut writer = tokio::spawn(async move {
        while let Some(bytes) = my_rx.recv().await {
            if ws_sink.send(Message::Binary(bytes)).await.is_err() {
                break;
            }
        }
    });

    // Reader loop: forward to whoever currently holds the peer slot. The peer
    // may not be connected yet; we just drop the frame in that case. Resolving
    // per-message avoids a stale cached `peer_tx` after the peer reconnects.
    let sessions = state.sessions.clone();
    let reader_code = code.clone();
    let reader_slot = authorized.slot;
    let mut reader = tokio::spawn(async move {
        while let Some(Ok(msg)) = ws_stream.next().await {
            if let Message::Binary(bytes) = msg {
                if let Some(peer_tx) = sessions.peer(&reader_code, reader_slot).await {
                    let _ = peer_tx.send(bytes);
                }
            }
        }
    });

    // Wait for either side to terminate.
    tokio::select! {
        _ = &mut writer => reader.abort(),
        _ = &mut reader => writer.abort(),
    }

    state.sessions.deregister(&code, authorized.slot).await;
    tracing::info!(code = %code, slot = ?authorized.slot, "client left live session");
}

async fn authorize(state: &AppState, code: &str, token: &str) -> Result<AuthorizedSlot, Frame> {
    let db = state.db.clone();
    let code_owned = code.to_string();
    let token_owned = token.to_string();
    tokio::task::spawn_blocking(move || -> Result<AuthorizedSlot, Frame> {
        db.with_conn(|conn| -> anyhow::Result<Result<AuthorizedSlot, Frame>> {
            let row = conn.query_row(
                "SELECT host_token, guest_token, iso_hash FROM sessions WHERE code = ?1",
                params![&code_owned],
                |r| {
                    Ok((
                        r.get::<_, String>(0)?,
                        r.get::<_, String>(1)?,
                        r.get::<_, Vec<u8>>(2)?,
                    ))
                },
            );
            match row {
                Ok((host_token, guest_token, iso_hash)) => {
                    let slot = if token_owned == host_token {
                        Slot::Host
                    } else if token_owned == guest_token {
                        Slot::Guest
                    } else {
                        return Ok(Err(Frame::ErrorSessionNotFound));
                    };
                    Ok(Ok(AuthorizedSlot { slot, iso_hash }))
                }
                Err(rusqlite::Error::QueryReturnedNoRows) => Ok(Err(Frame::ErrorSessionNotFound)),
                Err(e) => Err(e.into()),
            }
        })
        .unwrap_or_else(|e| {
            tracing::error!(error = ?e, "ws authorize: db error");
            Err(Frame::ErrorSessionNotFound)
        })
    })
    .await
    .unwrap_or(Err(Frame::ErrorSessionNotFound))
}

fn validate_hello(hello: &Hello, authorized: &AuthorizedSlot) -> Result<(), Frame> {
    if hello.protocol_version != PROTOCOL_VERSION {
        return Err(Frame::ErrorProtocolVersion {
            server_version: PROTOCOL_VERSION,
        });
    }
    if hello.iso_hash[..] != authorized.iso_hash[..] {
        let mut server_hash = [0u8; ISO_HASH_LEN];
        if authorized.iso_hash.len() == ISO_HASH_LEN {
            server_hash.copy_from_slice(&authorized.iso_hash);
        }
        return Err(Frame::ErrorIsoMismatch { server_hash });
    }
    let expected = match authorized.slot {
        Slot::Host => HelloIntent::Host,
        Slot::Guest => HelloIntent::Guest,
    };
    if hello.intent != expected {
        // Token + intent disagree — treat as session-not-found to avoid
        // leaking which slot the token belongs to.
        return Err(Frame::ErrorSessionNotFound);
    }
    Ok(())
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}
