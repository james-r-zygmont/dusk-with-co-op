//! Binary wire protocol shared with the C++ client.
//!
//! Each WebSocket binary frame carries one logical packet, encoded as:
//!
//! ```text
//! [tag: u8] [body: tag-specific bytes (big-endian)]
//! ```
//!
//! WebSocket message framing supplies the outer length, so we do not add one.
//! Tags are single-byte for now; bumping to LEB128 varint stays
//! backward-compatible (tags 0..=127 encode identically) if we ever exceed
//! 127 tags.
//!
//! Byte order is big-endian, matching the existing `BE()` save-data
//! annotations in the game and giving us network byte order for the wire.
//!
//! A byte-identical port of this module lives at
//! `src/dusk/net/wire.{hpp,cpp}` on the client side; the golden tests in
//! `tests/multiplayer/golden/` (M2 task #20) keep them in sync.

use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
use std::io::{self, Cursor, Read, Write};
use thiserror::Error;

pub const ISO_HASH_LEN: usize = 16;
pub const SESSION_CODE_LEN: usize = 6;
pub const STAGE_NAME_LEN: usize = 8;

/// `mTransformStatus` from `dSv_player_status_a_c` (see plan §C — per-player
/// overlay). The on-the-wire representation matches the in-memory enum.
pub const TRANSFORM_HUMAN: u8 = 0;
pub const TRANSFORM_WOLF: u8 = 1;

/// Sentinel for "no item currently held" in `PlayerAnim.held_item`.
pub const HELD_ITEM_NONE: u8 = 0xFF;

/// Intent flag for `Hello` packets.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HelloIntent {
    Host = 0,
    Guest = 1,
}

impl HelloIntent {
    fn from_u8(b: u8) -> Result<Self, FrameError> {
        match b {
            0 => Ok(Self::Host),
            1 => Ok(Self::Guest),
            _ => Err(FrameError::InvalidEnum {
                name: "HelloIntent",
                value: b as u16,
            }),
        }
    }
}

/// Top-level packet tag. Single-byte; treat as a forward-compatible
/// varint (1..=127 only).
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tag {
    Hello = 1,
    HelloAck = 2,
    ErrorIsoMismatch = 3,
    ErrorProtocolVersion = 4,
    ErrorSessionNotFound = 5,
    ErrorSessionFull = 6,
    ErrorSaveCommitConflict = 7,

    // M3 replication packets — peer ↔ peer via server.
    PlayerPose = 16,
    PlayerAnim = 17,
    SceneAnnounce = 18,

    // Reserved for later: FlagSet=32, KeyItemPickup=33, ConsumableChange=34,
    // SaveSnapshot=48, SaveCommit=49, SaveCommitAck=50,
    // SaveCommitConflict=51, Keepalive=64.
}

impl Tag {
    fn from_u8(b: u8) -> Result<Self, FrameError> {
        match b {
            1 => Ok(Self::Hello),
            2 => Ok(Self::HelloAck),
            3 => Ok(Self::ErrorIsoMismatch),
            4 => Ok(Self::ErrorProtocolVersion),
            5 => Ok(Self::ErrorSessionNotFound),
            6 => Ok(Self::ErrorSessionFull),
            7 => Ok(Self::ErrorSaveCommitConflict),
            16 => Ok(Self::PlayerPose),
            17 => Ok(Self::PlayerAnim),
            18 => Ok(Self::SceneAnnounce),
            _ => Err(FrameError::UnknownTag(b)),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Hello {
    pub protocol_version: u16,
    pub iso_hash: [u8; ISO_HASH_LEN],
    pub session_code: [u8; SESSION_CODE_LEN],
    pub intent: HelloIntent,
    pub display_name: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HelloAck {
    pub protocol_version: u16,
    pub player_index: u8,
    pub server_time_ms: u64,
}

/// One pose snapshot from a peer's `daAlink_c`. Position, rotation, velocity,
/// and a monotonic per-sender tick so the receiver can drop reorderings.
/// Sent at ~20 Hz with send-on-delta gating on the sender. Source fields on
/// `fopAc_ac_c`: `current.pos @ 0x4D0`, `shape_angle @ 0x4E4`,
/// `speed @ 0x4F8`, `speedF @ 0x52C`.
#[derive(Debug, Clone, PartialEq)]
pub struct PlayerPose {
    pub tick: u32,
    pub stage: [u8; STAGE_NAME_LEN],
    pub room: u8,
    pub pos: [f32; 3],
    pub shape_angle: [i16; 3],
    pub speed: [f32; 3],
    pub speed_f: f32,
}

/// Animation snapshot. Sent on change; the receiver drives the puppet's
/// animation procedure from `anim_id` and steps `frame` itself between
/// updates so a dropped packet doesn't freeze the puppet.
#[derive(Debug, Clone, PartialEq)]
pub struct PlayerAnim {
    pub anim_id: u16,
    pub frame: f32,
    pub transform: u8,   // 0 = human, 1 = wolf
    pub held_item: u8,   // 0xFF = nothing held
}

/// Sent when a client's scene-request reaches the `Done` phase
/// (`f_op_scene_req.cpp`). The server caches the latest announce per slot
/// and flips co-location on when host's `stage`+`room` match the guest's.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SceneAnnounce {
    pub stage: [u8; STAGE_NAME_LEN],
    pub room: u8,
    pub spawn: u8,
}

/// All packet variants we currently understand. Decode produces one of
/// these; encode round-trips back to the same bytes (modulo `display_name`
/// length, which is varying). `PartialEq` not `Eq` because the M3 pose /
/// anim variants carry `f32` fields.
#[derive(Debug, Clone, PartialEq)]
pub enum Frame {
    Hello(Hello),
    HelloAck(HelloAck),
    ErrorIsoMismatch { server_hash: [u8; ISO_HASH_LEN] },
    ErrorProtocolVersion { server_version: u16 },
    ErrorSessionNotFound,
    ErrorSessionFull,
    ErrorSaveCommitConflict,
    PlayerPose(PlayerPose),
    PlayerAnim(PlayerAnim),
    SceneAnnounce(SceneAnnounce),
}

#[derive(Debug, Error)]
pub enum FrameError {
    #[error("io: {0}")]
    Io(#[from] io::Error),
    #[error("unknown packet tag: {0}")]
    UnknownTag(u8),
    #[error("invalid enum {name} value: {value}")]
    InvalidEnum { name: &'static str, value: u16 },
    #[error("invalid utf-8 in string field")]
    InvalidUtf8,
    #[error("trailing bytes after packet body")]
    TrailingBytes,
}

impl Frame {
    pub fn encode(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(64);
        self.write_into(&mut buf)
            .expect("Vec::write is infallible");
        buf
    }

    fn write_into<W: Write>(&self, w: &mut W) -> io::Result<()> {
        match self {
            Frame::Hello(p) => {
                w.write_u8(Tag::Hello as u8)?;
                w.write_u16::<BigEndian>(p.protocol_version)?;
                w.write_all(&p.iso_hash)?;
                w.write_all(&p.session_code)?;
                w.write_u8(p.intent as u8)?;
                write_str(w, &p.display_name)?;
            }
            Frame::HelloAck(p) => {
                w.write_u8(Tag::HelloAck as u8)?;
                w.write_u16::<BigEndian>(p.protocol_version)?;
                w.write_u8(p.player_index)?;
                w.write_u64::<BigEndian>(p.server_time_ms)?;
            }
            Frame::ErrorIsoMismatch { server_hash } => {
                w.write_u8(Tag::ErrorIsoMismatch as u8)?;
                w.write_all(server_hash)?;
            }
            Frame::ErrorProtocolVersion { server_version } => {
                w.write_u8(Tag::ErrorProtocolVersion as u8)?;
                w.write_u16::<BigEndian>(*server_version)?;
            }
            Frame::ErrorSessionNotFound => {
                w.write_u8(Tag::ErrorSessionNotFound as u8)?;
            }
            Frame::ErrorSessionFull => {
                w.write_u8(Tag::ErrorSessionFull as u8)?;
            }
            Frame::ErrorSaveCommitConflict => {
                w.write_u8(Tag::ErrorSaveCommitConflict as u8)?;
            }
            Frame::PlayerPose(p) => {
                w.write_u8(Tag::PlayerPose as u8)?;
                w.write_u32::<BigEndian>(p.tick)?;
                w.write_all(&p.stage)?;
                w.write_u8(p.room)?;
                for v in &p.pos { w.write_f32::<BigEndian>(*v)?; }
                for v in &p.shape_angle { w.write_i16::<BigEndian>(*v)?; }
                for v in &p.speed { w.write_f32::<BigEndian>(*v)?; }
                w.write_f32::<BigEndian>(p.speed_f)?;
            }
            Frame::PlayerAnim(p) => {
                w.write_u8(Tag::PlayerAnim as u8)?;
                w.write_u16::<BigEndian>(p.anim_id)?;
                w.write_f32::<BigEndian>(p.frame)?;
                w.write_u8(p.transform)?;
                w.write_u8(p.held_item)?;
            }
            Frame::SceneAnnounce(p) => {
                w.write_u8(Tag::SceneAnnounce as u8)?;
                w.write_all(&p.stage)?;
                w.write_u8(p.room)?;
                w.write_u8(p.spawn)?;
            }
        }
        Ok(())
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, FrameError> {
        let mut cur = Cursor::new(bytes);
        let tag = Tag::from_u8(cur.read_u8()?)?;
        let frame = match tag {
            Tag::Hello => {
                let protocol_version = cur.read_u16::<BigEndian>()?;
                let mut iso_hash = [0u8; ISO_HASH_LEN];
                cur.read_exact(&mut iso_hash)?;
                let mut session_code = [0u8; SESSION_CODE_LEN];
                cur.read_exact(&mut session_code)?;
                let intent = HelloIntent::from_u8(cur.read_u8()?)?;
                let display_name = read_str(&mut cur)?;
                Frame::Hello(Hello {
                    protocol_version,
                    iso_hash,
                    session_code,
                    intent,
                    display_name,
                })
            }
            Tag::HelloAck => {
                let protocol_version = cur.read_u16::<BigEndian>()?;
                let player_index = cur.read_u8()?;
                let server_time_ms = cur.read_u64::<BigEndian>()?;
                Frame::HelloAck(HelloAck {
                    protocol_version,
                    player_index,
                    server_time_ms,
                })
            }
            Tag::ErrorIsoMismatch => {
                let mut server_hash = [0u8; ISO_HASH_LEN];
                cur.read_exact(&mut server_hash)?;
                Frame::ErrorIsoMismatch { server_hash }
            }
            Tag::ErrorProtocolVersion => {
                let server_version = cur.read_u16::<BigEndian>()?;
                Frame::ErrorProtocolVersion { server_version }
            }
            Tag::ErrorSessionNotFound => Frame::ErrorSessionNotFound,
            Tag::ErrorSessionFull => Frame::ErrorSessionFull,
            Tag::ErrorSaveCommitConflict => Frame::ErrorSaveCommitConflict,
            Tag::PlayerPose => {
                let tick = cur.read_u32::<BigEndian>()?;
                let mut stage = [0u8; STAGE_NAME_LEN];
                cur.read_exact(&mut stage)?;
                let room = cur.read_u8()?;
                let pos = [
                    cur.read_f32::<BigEndian>()?,
                    cur.read_f32::<BigEndian>()?,
                    cur.read_f32::<BigEndian>()?,
                ];
                let shape_angle = [
                    cur.read_i16::<BigEndian>()?,
                    cur.read_i16::<BigEndian>()?,
                    cur.read_i16::<BigEndian>()?,
                ];
                let speed = [
                    cur.read_f32::<BigEndian>()?,
                    cur.read_f32::<BigEndian>()?,
                    cur.read_f32::<BigEndian>()?,
                ];
                let speed_f = cur.read_f32::<BigEndian>()?;
                Frame::PlayerPose(PlayerPose {
                    tick,
                    stage,
                    room,
                    pos,
                    shape_angle,
                    speed,
                    speed_f,
                })
            }
            Tag::PlayerAnim => {
                let anim_id = cur.read_u16::<BigEndian>()?;
                let frame = cur.read_f32::<BigEndian>()?;
                let transform = cur.read_u8()?;
                let held_item = cur.read_u8()?;
                Frame::PlayerAnim(PlayerAnim {
                    anim_id,
                    frame,
                    transform,
                    held_item,
                })
            }
            Tag::SceneAnnounce => {
                let mut stage = [0u8; STAGE_NAME_LEN];
                cur.read_exact(&mut stage)?;
                let room = cur.read_u8()?;
                let spawn = cur.read_u8()?;
                Frame::SceneAnnounce(SceneAnnounce {
                    stage,
                    room,
                    spawn,
                })
            }
        };

        if cur.position() as usize != bytes.len() {
            return Err(FrameError::TrailingBytes);
        }
        Ok(frame)
    }
}

fn write_str<W: Write>(w: &mut W, s: &str) -> io::Result<()> {
    let bytes = s.as_bytes();
    assert!(
        bytes.len() <= u16::MAX as usize,
        "wire: string field longer than 65535 bytes"
    );
    w.write_u16::<BigEndian>(bytes.len() as u16)?;
    w.write_all(bytes)
}

fn read_str<R: Read>(r: &mut R) -> Result<String, FrameError> {
    let len = r.read_u16::<BigEndian>()? as usize;
    let mut buf = vec![0u8; len];
    r.read_exact(&mut buf)?;
    String::from_utf8(buf).map_err(|_| FrameError::InvalidUtf8)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hash(seed: u8) -> [u8; ISO_HASH_LEN] {
        let mut h = [0u8; ISO_HASH_LEN];
        for (i, b) in h.iter_mut().enumerate() {
            *b = seed.wrapping_add(i as u8);
        }
        h
    }

    fn roundtrip(frame: Frame) {
        let bytes = frame.encode();
        let decoded = Frame::decode(&bytes).expect("decode");
        assert_eq!(frame, decoded);
    }

    #[test]
    fn hello_roundtrip() {
        roundtrip(Frame::Hello(Hello {
            protocol_version: 1,
            iso_hash: hash(0x42),
            session_code: *b"ABCDEF",
            intent: HelloIntent::Host,
            display_name: "Alice 🧝".to_string(),
        }));
    }

    #[test]
    fn hello_ack_roundtrip() {
        roundtrip(Frame::HelloAck(HelloAck {
            protocol_version: 1,
            player_index: 1,
            server_time_ms: 0x0123_4567_89AB_CDEF,
        }));
    }

    #[test]
    fn error_iso_mismatch_roundtrip() {
        roundtrip(Frame::ErrorIsoMismatch {
            server_hash: hash(0xFF),
        });
    }

    #[test]
    fn error_protocol_version_roundtrip() {
        roundtrip(Frame::ErrorProtocolVersion { server_version: 99 });
    }

    #[test]
    fn error_unit_variants_roundtrip() {
        roundtrip(Frame::ErrorSessionNotFound);
        roundtrip(Frame::ErrorSessionFull);
        roundtrip(Frame::ErrorSaveCommitConflict);
    }

    #[test]
    fn unknown_tag_rejected() {
        assert!(matches!(
            Frame::decode(&[99]),
            Err(FrameError::UnknownTag(99))
        ));
    }

    #[test]
    fn trailing_bytes_rejected() {
        let mut bytes = Frame::ErrorSessionNotFound.encode();
        bytes.push(0xAB);
        assert!(matches!(
            Frame::decode(&bytes),
            Err(FrameError::TrailingBytes)
        ));
    }

    /// A byte-pinned fixture for the Hello frame. The C++ client's golden
    /// test in M2 task #20 will assert the same expected bytes; if either
    /// side drifts, both tests fail.
    #[test]
    fn hello_byte_pinned() {
        let frame = Frame::Hello(Hello {
            protocol_version: 1,
            iso_hash: [0; ISO_HASH_LEN],
            session_code: *b"ABCDEF",
            intent: HelloIntent::Host,
            display_name: "x".to_string(),
        });
        let bytes = frame.encode();
        // 1 tag + 2 ver + 16 iso + 6 code + 1 intent + 2 namelen + 1 name = 29
        assert_eq!(bytes.len(), 29);
        assert_eq!(bytes[0], Tag::Hello as u8);
        assert_eq!(&bytes[1..3], &[0x00, 0x01]); // protocol version 1 BE
        assert_eq!(&bytes[3..19], &[0u8; 16]);
        assert_eq!(&bytes[19..25], b"ABCDEF");
        assert_eq!(bytes[25], HelloIntent::Host as u8);
        assert_eq!(&bytes[26..28], &[0x00, 0x01]); // name length 1 BE
        assert_eq!(bytes[28], b'x');
    }

    #[test]
    fn player_pose_roundtrip() {
        roundtrip(Frame::PlayerPose(PlayerPose {
            tick: 12345,
            stage: *b"F_SP103\0",
            room: 7,
            pos: [1.0, -2.5, 3.75],
            shape_angle: [0, 0x4000, -0x1234],
            speed: [0.1, 0.0, -0.25],
            speed_f: 4.5,
        }));
    }

    #[test]
    fn player_anim_roundtrip() {
        roundtrip(Frame::PlayerAnim(PlayerAnim {
            anim_id: 0x0042,
            frame: 17.5,
            transform: TRANSFORM_HUMAN,
            held_item: HELD_ITEM_NONE,
        }));
    }

    #[test]
    fn scene_announce_roundtrip() {
        roundtrip(Frame::SceneAnnounce(SceneAnnounce {
            stage: *b"F_SP103\0",
            room: 2,
            spawn: 1,
        }));
    }

    /// Byte-pinned PlayerPose fixture. C++ port's `wire::RunSelfTest` matches
    /// these bytes exactly.
    #[test]
    fn player_pose_byte_pinned() {
        let frame = Frame::PlayerPose(PlayerPose {
            tick: 1,
            stage: *b"F_SP103\0",
            room: 0,
            pos: [0.0, 0.0, 0.0],
            shape_angle: [0, 0, 0],
            speed: [0.0, 0.0, 0.0],
            speed_f: 0.0,
        });
        let bytes = frame.encode();
        // 1 tag + 4 tick + 8 stage + 1 room + 12 pos + 6 angle + 12 speed + 4 speed_f
        assert_eq!(bytes.len(), 48);
        assert_eq!(bytes[0], Tag::PlayerPose as u8);
        assert_eq!(&bytes[1..5], &[0x00, 0x00, 0x00, 0x01]); // tick = 1 BE
        assert_eq!(&bytes[5..13], b"F_SP103\0");
        assert_eq!(bytes[13], 0);                            // room
        assert_eq!(&bytes[14..26], &[0u8; 12]);              // pos[3] = 0.0
        assert_eq!(&bytes[26..32], &[0u8; 6]);               // shape_angle[3] = 0
        assert_eq!(&bytes[32..44], &[0u8; 12]);              // speed[3] = 0.0
        assert_eq!(&bytes[44..48], &[0u8; 4]);               // speed_f = 0.0
    }
}
