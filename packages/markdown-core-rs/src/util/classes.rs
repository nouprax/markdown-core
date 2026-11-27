//! Unicode character classes, anchor images and the reference-label normal
//! form, read from the Unicode 17 tables (`tables.rs`).

use super::tables::{
    ANCHOR_CLASSES, ANCHOR_DELTAS, ANCHOR_LEAVES, ANCHOR_SHEETS, BLOCKS, FOLDS, FOLD_LEAVES,
    FOLD_PICTURES, FOLD_SHEETS, SHEETS,
};
use alloc::string::String;

pub const WHITESPACE: u8 = 1 << 0;
pub const PUNCTUATION: u8 = 1 << 1;
pub const MARK: u8 = 1 << 2;

/// The classes of `char`.
pub fn classes(char: char) -> u8 {
    let scalar = char as usize;
    BLOCKS[SHEETS[scalar >> 8] as usize + (scalar & 255)]
}

/// The anchor image of `char`, or `None` when an anchor drops it.
pub fn anchor_picture(char: char) -> Option<char> {
    let scalar = char as usize;
    let sheet = ANCHOR_SHEETS[scalar >> 10] as usize;
    let leaf = ANCHOR_LEAVES[sheet * 64 + ((scalar >> 4) & 63)] as usize;
    let class = ANCHOR_CLASSES[leaf * 16 + (scalar & 15)] as usize;
    if class == 0 {
        None
    } else {
        let picture = i64::try_from(scalar).ok()? + i64::from(ANCHOR_DELTAS[class]);
        char::from_u32(u32::try_from(picture).ok()?)
    }
}

/// The anchor image of `text`: every scalar mapped, dropped ones removed.
pub fn anchor(text: &str) -> String {
    text.chars().filter_map(anchor_picture).collect()
}

/// The case fold of `char`, when it has one.
fn fold(char: char) -> Option<&'static str> {
    let scalar = char as usize;
    if scalar >= 1 << 17 {
        return None;
    }
    let number = FOLD_LEAVES[FOLD_SHEETS[scalar >> 6] as usize * 64 + (scalar & 63)] as usize;
    if number == 0 {
        return None;
    }
    let item = FOLDS[number - 1];
    let offset = (((item >> 17) & 0xFFF) * 2) as usize;
    let size = (item >> 29) as usize;
    core::str::from_utf8(&FOLD_PICTURES[offset..offset + size]).ok()
}

/// Whether `code` is whitespace for label normalization.
fn label_space(char: char) -> bool {
    matches!(char, ' ' | '\t' | '\n' | '\r')
}

/// The reference-label normal form: case fold, trim whitespace, and collapse
/// each interior whitespace run to one space.
pub fn normalize_label(value: &str) -> String {
    let mut result = String::new();
    let mut space = false;
    for char in value.chars() {
        if label_space(char) {
            space = !result.is_empty();
            continue;
        }
        if space {
            result.push(' ');
            space = false;
        }
        if char.is_ascii() {
            result.push(char.to_ascii_lowercase());
        } else if let Some(picture) = fold(char) {
            result.push_str(picture);
        } else {
            result.push(char);
        }
    }
    result
}
