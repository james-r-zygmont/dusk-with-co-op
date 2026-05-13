//! In-memory live-session state.
//!
//! While the SQLite tables hold the canonical save and session metadata,
//! routing of realtime WS frames between the two paired clients lives in
//! memory. Each session has two slots (host + guest); each connected client
//! owns an mpsc channel whose `Sender` lives in the slot. A client's reader
//! task pushes incoming WS frames onto the *other* slot's sender, and a
//! writer task drains its own slot's receiver back to its WS sink.

use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::{mpsc, Mutex};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Slot {
    Host,
    Guest,
}

impl Slot {
    pub fn player_index(self) -> u8 {
        match self {
            Slot::Host => 0,
            Slot::Guest => 1,
        }
    }
}

pub type FrameTx = mpsc::UnboundedSender<Vec<u8>>;
pub type FrameRx = mpsc::UnboundedReceiver<Vec<u8>>;

#[derive(Debug, Default)]
struct SessionLive {
    host: Option<FrameTx>,
    guest: Option<FrameTx>,
}

#[derive(Clone, Default)]
pub struct SessionMap(Arc<Mutex<HashMap<String, SessionLive>>>);

#[derive(Debug, PartialEq, Eq)]
pub enum RegisterError {
    /// That slot is already claimed by another live connection.
    SlotTaken,
}

impl SessionMap {
    pub fn new() -> Self {
        Self::default()
    }

    /// Claim a slot in the live session map. On success, returns the peer's
    /// `FrameTx` (or `None` if the peer hasn't joined yet) so the caller can
    /// route inbound frames to them.
    pub async fn register(
        &self,
        code: &str,
        slot: Slot,
        tx: FrameTx,
    ) -> Result<Option<FrameTx>, RegisterError> {
        let mut map = self.0.lock().await;
        let live = map.entry(code.to_string()).or_default();
        let taken = match slot {
            Slot::Host => live.host.is_some(),
            Slot::Guest => live.guest.is_some(),
        };
        if taken {
            return Err(RegisterError::SlotTaken);
        }
        let peer = match slot {
            Slot::Host => live.guest.clone(),
            Slot::Guest => live.host.clone(),
        };
        match slot {
            Slot::Host => live.host = Some(tx),
            Slot::Guest => live.guest = Some(tx),
        }
        Ok(peer)
    }

    /// Resolve the peer's sender at the moment of the call. The peer may join
    /// after registration, so the reader loop calls this each time it needs
    /// to forward a frame.
    pub async fn peer(&self, code: &str, my_slot: Slot) -> Option<FrameTx> {
        let map = self.0.lock().await;
        map.get(code).and_then(|live| match my_slot {
            Slot::Host => live.guest.clone(),
            Slot::Guest => live.host.clone(),
        })
    }

    /// Release a slot. If both slots are now empty, drop the session entry
    /// entirely.
    pub async fn deregister(&self, code: &str, slot: Slot) {
        let mut map = self.0.lock().await;
        if let Some(live) = map.get_mut(code) {
            match slot {
                Slot::Host => live.host = None,
                Slot::Guest => live.guest = None,
            }
            if live.host.is_none() && live.guest.is_none() {
                map.remove(code);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn register_and_peer_resolve() {
        let map = SessionMap::new();
        let (host_tx, _host_rx) = mpsc::unbounded_channel();
        let (guest_tx, _guest_rx) = mpsc::unbounded_channel();

        // Host arrives first; no peer to return yet.
        assert!(matches!(
            map.register("AAAAAA", Slot::Host, host_tx.clone()).await,
            Ok(None)
        ));
        // Guest arrives; should see the host's sender.
        assert!(matches!(
            map.register("AAAAAA", Slot::Guest, guest_tx.clone()).await,
            Ok(Some(_))
        ));

        assert!(map.peer("AAAAAA", Slot::Guest).await.is_some());
        assert!(map.peer("AAAAAA", Slot::Host).await.is_some());
    }

    #[tokio::test]
    async fn slot_taken_rejects_second_claim() {
        let map = SessionMap::new();
        let (tx1, _r1) = mpsc::unbounded_channel();
        let (tx2, _r2) = mpsc::unbounded_channel();
        map.register("AAAAAA", Slot::Host, tx1).await.unwrap();
        assert!(matches!(
            map.register("AAAAAA", Slot::Host, tx2).await,
            Err(RegisterError::SlotTaken)
        ));
    }

    #[tokio::test]
    async fn deregister_clears_slot_and_gc_empty_session() {
        let map = SessionMap::new();
        let (tx, _r) = mpsc::unbounded_channel();
        map.register("AAAAAA", Slot::Host, tx).await.unwrap();
        map.deregister("AAAAAA", Slot::Host).await;
        // No peer, no live entry.
        assert!(map.peer("AAAAAA", Slot::Guest).await.is_none());
    }
}
