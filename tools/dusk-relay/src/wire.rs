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
    // Reserved for M3+: PlayerPose=16, PlayerAnim=17, SceneAnnounce=18,
    // FlagSet=32, KeyItemPickup=33, ConsumableChange=34, SaveSnapshot=48,
    // SaveCommit=49, SaveCommitAck=50, SaveCommitConflict=51, Keepalive=64.
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

/// All packet variants we currently understand. Decode produces one of
/// these; encode round-trips back to the same bytes (modulo `display_name`
/// length, which is varying).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Frame {
    Hello(Hello),
    HelloAck(HelloAck),
    ErrorIsoMismatch { server_hash: [u8; ISO_HASH_LEN] },
    ErrorProtocolVersion { server_version: u16 },
    ErrorSessionNotFound,
    ErrorSessionFull,
    ErrorSaveCommitConflict,
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
}
