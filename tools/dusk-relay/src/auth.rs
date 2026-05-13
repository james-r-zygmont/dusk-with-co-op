//! Session-code + token generation.
//!
//! Session codes are 6 chars from an ambiguity-free alphabet (no 0/O, 1/I/L,
//! 8/B, U) — `30^6 ≈ 7.29e8` codes is plenty for M2 where collisions are
//! resolved by retrying the SQL insert.
//!
//! Tokens are 32 random bytes hex-encoded (64 chars). Opaque to clients;
//! presented in the WS `?token=` query string at connect time.

use rand::distributions::{Distribution, Uniform};
use rand::Rng;

const SESSION_CODE_ALPHABET: &[u8] = b"ABCDEFGHJKMNPQRSTVWXYZ23456789";
pub const SESSION_CODE_LEN: usize = 6;
pub const TOKEN_BYTES: usize = 32;

pub fn gen_session_code() -> String {
    let mut rng = rand::thread_rng();
    let dist = Uniform::from(0..SESSION_CODE_ALPHABET.len());
    let bytes: Vec<u8> = (0..SESSION_CODE_LEN)
        .map(|_| SESSION_CODE_ALPHABET[dist.sample(&mut rng)])
        .collect();
    // All bytes come from an ASCII alphabet so this is always valid UTF-8.
    String::from_utf8(bytes).expect("alphabet is ascii")
}

pub fn gen_token() -> String {
    let mut bytes = [0u8; TOKEN_BYTES];
    rand::thread_rng().fill(&mut bytes);
    hex::encode(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn session_codes_are_alphabet_only_and_correct_length() {
        for _ in 0..100 {
            let code = gen_session_code();
            assert_eq!(code.len(), SESSION_CODE_LEN);
            assert!(code
                .as_bytes()
                .iter()
                .all(|b| SESSION_CODE_ALPHABET.contains(b)));
        }
    }

    #[test]
    fn tokens_are_64_hex_chars() {
        let token = gen_token();
        assert_eq!(token.len(), TOKEN_BYTES * 2);
        assert!(token.chars().all(|c| c.is_ascii_hexdigit()));
    }
}
