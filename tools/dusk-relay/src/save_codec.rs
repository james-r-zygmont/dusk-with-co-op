//! Portable save-blob codec.
//!
//! The canonical save is stored as an opaque byte blob in the `saves.blob`
//! column. The blob is a back-to-back sequence of tuples:
//!
//! ```text
//! repeat to EOF:
//!     [field_id: u16 big-endian]
//!     [length:   u16 big-endian]
//!     [bytes:    `length` bytes]
//! ```
//!
//! `saves.schema_version` tracks codec versions. Unknown `field_id` values
//! pass through encode/decode unchanged so an older client can round-trip a
//! save written by a newer client without losing data — this is the property
//! that lets us add fields without forcing a lock-step upgrade.
//!
//! A byte-identical C++ port lives at `src/dusk/net/save_codec.cpp`
//! (M2 task #18); the cross-language golden tests in
//! `tests/multiplayer/golden/` (M2 task #20) keep both implementations
//! honest.
//!
//! Field-id assignments for actual save data (event flags, key items, max HP,
//! wallet size, equipment, ...) are defined alongside the C++ save-mapping
//! work and aren't part of this module — the codec itself is just the
//! byte-shoveling layer.

use byteorder::{BigEndian, ReadBytesExt, WriteBytesExt};
use std::collections::BTreeMap;
use std::io::{self, Cursor, Read, Write};
use thiserror::Error;

/// Schema version baked into the `saves.schema_version` column. Bump when a
/// field's meaning changes (renames / re-encodings); not bumped when fields
/// are added (those carry through the unknown-id passthrough).
pub const SCHEMA_VERSION: u16 = 1;

/// A decoded save blob: a sorted-by-id bag of `(field_id, bytes)`. Iteration
/// order is deterministic (BTreeMap), which keeps encode output stable for
/// the golden tests.
pub type SaveFields = BTreeMap<u16, Vec<u8>>;

#[derive(Debug, Error)]
pub enum SaveCodecError {
    #[error("io: {0}")]
    Io(#[from] io::Error),
    #[error("duplicate field_id {0} in save blob")]
    DuplicateField(u16),
    #[error("truncated tuple: header reads past end of blob")]
    Truncated,
    #[error("field {field_id} body too long for u16 length ({len} bytes)")]
    FieldTooLong { field_id: u16, len: usize },
}

pub fn encode(fields: &SaveFields) -> Result<Vec<u8>, SaveCodecError> {
    let mut buf = Vec::with_capacity(fields.len() * 8);
    write_into(&mut buf, fields)?;
    Ok(buf)
}

fn write_into<W: Write>(w: &mut W, fields: &SaveFields) -> Result<(), SaveCodecError> {
    for (&field_id, bytes) in fields {
        if bytes.len() > u16::MAX as usize {
            return Err(SaveCodecError::FieldTooLong {
                field_id,
                len: bytes.len(),
            });
        }
        w.write_u16::<BigEndian>(field_id)?;
        w.write_u16::<BigEndian>(bytes.len() as u16)?;
        w.write_all(bytes)?;
    }
    Ok(())
}

pub fn decode(blob: &[u8]) -> Result<SaveFields, SaveCodecError> {
    let mut cur = Cursor::new(blob);
    let mut fields = SaveFields::new();
    let end = blob.len() as u64;
    while cur.position() < end {
        // Need at least 4 bytes for the header.
        if end - cur.position() < 4 {
            return Err(SaveCodecError::Truncated);
        }
        let field_id = cur.read_u16::<BigEndian>()?;
        let len = cur.read_u16::<BigEndian>()? as u64;
        if end - cur.position() < len {
            return Err(SaveCodecError::Truncated);
        }
        let mut body = vec![0u8; len as usize];
        cur.read_exact(&mut body)?;
        if fields.insert(field_id, body).is_some() {
            return Err(SaveCodecError::DuplicateField(field_id));
        }
    }
    Ok(fields)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fields_of<const N: usize>(items: [(u16, &[u8]); N]) -> SaveFields {
        items
            .into_iter()
            .map(|(id, bytes)| (id, bytes.to_vec()))
            .collect()
    }

    #[test]
    fn empty_roundtrip() {
        let bytes = encode(&SaveFields::new()).unwrap();
        assert!(bytes.is_empty());
        assert_eq!(decode(&bytes).unwrap(), SaveFields::new());
    }

    #[test]
    fn single_field_roundtrip() {
        let fields = fields_of([(0x0001, &[0xDE, 0xAD, 0xBE, 0xEF])]);
        let bytes = encode(&fields).unwrap();
        assert_eq!(decode(&bytes).unwrap(), fields);
    }

    #[test]
    fn multi_field_roundtrip_is_sorted_by_id() {
        // Insert out of order; encode emits sorted-by-id; decode reproduces
        // the same map (BTreeMap equality is unordered but the bytes are
        // deterministic).
        let mut fields = SaveFields::new();
        fields.insert(0x0042, vec![0xAA, 0xBB]);
        fields.insert(0x0001, vec![0x01]);
        fields.insert(0x00FF, vec![]);

        let bytes = encode(&fields).unwrap();
        assert_eq!(&bytes[0..4], &[0x00, 0x01, 0x00, 0x01]); // id=1, len=1
        assert_eq!(bytes[4], 0x01);
        assert_eq!(&bytes[5..9], &[0x00, 0x42, 0x00, 0x02]); // id=66, len=2
        assert_eq!(&bytes[9..11], &[0xAA, 0xBB]);
        assert_eq!(&bytes[11..15], &[0x00, 0xFF, 0x00, 0x00]); // id=255, len=0
        assert_eq!(bytes.len(), 15);

        assert_eq!(decode(&bytes).unwrap(), fields);
    }

    #[test]
    fn unknown_field_ids_pass_through() {
        // The whole point of the codec: a "future" field id we don't
        // recognise still round-trips its bytes intact.
        let fields = fields_of([
            (0x0001, &[0x10]),
            (0xFFFF, &[0x99, 0x99, 0x99, 0x99]),
        ]);
        let bytes = encode(&fields).unwrap();
        let decoded = decode(&bytes).unwrap();
        assert_eq!(decoded, fields);
        // Re-encoding the decoded map produces identical bytes.
        assert_eq!(encode(&decoded).unwrap(), bytes);
    }

    #[test]
    fn truncated_header_rejected() {
        // 3 bytes — not enough for the 4-byte header.
        assert!(matches!(
            decode(&[0x00, 0x01, 0x00]),
            Err(SaveCodecError::Truncated)
        ));
    }

    #[test]
    fn truncated_body_rejected() {
        // Header claims 4 bytes of body but only 2 are present.
        let bytes = [0x00, 0x01, 0x00, 0x04, 0xAA, 0xBB];
        assert!(matches!(decode(&bytes), Err(SaveCodecError::Truncated)));
    }

    #[test]
    fn duplicate_field_rejected() {
        // Two tuples with the same field_id — defensive guard against a
        // peer / disk-corrupt blob.
        let bytes = [
            0x00, 0x01, 0x00, 0x01, 0xAA, // field 1, 1 byte
            0x00, 0x01, 0x00, 0x01, 0xBB, // field 1 again
        ];
        assert!(matches!(
            decode(&bytes),
            Err(SaveCodecError::DuplicateField(1))
        ));
    }

    /// Byte-pinned fixture for the cross-language golden test (M2 task #20).
    /// If the C++ port produces different bytes for the same inputs, both
    /// this test and the C++ counterpart fail.
    #[test]
    fn byte_pinned_two_fields() {
        let fields = fields_of([
            (0x0001, &[0x10, 0x20, 0x30, 0x40]),
            (0x0042, &[0xCA, 0xFE]),
        ]);
        let bytes = encode(&fields).unwrap();
        let expected = [
            0x00, 0x01, 0x00, 0x04, 0x10, 0x20, 0x30, 0x40,
            0x00, 0x42, 0x00, 0x02, 0xCA, 0xFE,
        ];
        assert_eq!(&bytes[..], &expected[..]);
    }
}
