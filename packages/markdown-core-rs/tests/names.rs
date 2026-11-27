//! Identifiers and file names of this crate contain none of the letters
//! `g`, `j` and `y`, in either case.
//!
//! Names that come from outside the crate (the standard library, lints,
//! other crates) are listed in `OUTSIDE`. Comments and literals are prose
//! and are not checked.

use std::fs;
use std::path::{Path, PathBuf};

/// Names defined outside this crate.
const OUTSIDE: &[&str] = &[
    "Copy",
    "Debug",
    "String",
    "ToString",
    "any",
    "args",
    "as_bytes",
    "binary_search",
    "bytes",
    "cfg",
    "clippy",
    "debug_assert",
    "debug_assert_eq",
    "debug_assert_ne",
    "deny",
    "display",
    "from_utf8_lossy",
    "fuzz_target",
    "get",
    "get_or_insert_with",
    "honggfuzz",
    "into_string",
    "is_ascii_digit",
    "is_ascii_hexdigit",
    "is_empty",
    "join",
    "libfuzzer_sys",
    "missing_panics_doc",
    "read_to_string",
    "saturating_sub",
    "sort_by_key",
    "sort_unstable_by",
    "string",
    "to_string",
    "to_string_lossy",
    "too_many_lines",
    "try_from",
    "with_capacity",
];

fn banned(name: &str) -> bool {
    name.chars()
        .any(|c| matches!(c.to_ascii_lowercase(), 'g' | 'j' | 'y'))
}

fn files(folder: &Path, found: &mut Vec<PathBuf>) {
    for item in fs::read_dir(folder).expect("expected a folder") {
        let path = item.expect("expected a folder item").path();
        if path.is_dir() {
            if path.file_name().map_or(false, |name| name != "target") {
                files(&path, found);
            }
        } else if path.extension().map_or(false, |ext| ext == "rs") {
            found.push(path);
        }
    }
}

/// Code with comments, strings, and characters blanked out.
fn code(text: &str) -> String {
    let source = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut index = 0;
    while index < source.len() {
        let rest = &source[index..];
        if rest.starts_with(b"//") {
            while index < source.len() && source[index] != b'\n' {
                index += 1;
            }
        } else if rest[0] == b'"' {
            index += 1;
            while index < source.len() && source[index] != b'"' {
                index += if source[index] == b'\\' { 2 } else { 1 };
            }
            index += 1;
            out.push_str("\"\"");
        } else if rest[0] == b'\''
            && (rest.get(2) == Some(&b'\'')
                || (rest.get(1) == Some(&b'\\') && rest.get(3) == Some(&b'\'')))
        {
            index += if rest[1] == b'\\' { 4 } else { 3 };
            out.push_str("' '");
        } else {
            out.push(char::from(rest[0]));
            index += 1;
        }
    }
    out
}

#[test]
fn names() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let mut found = vec![];
    for folder in ["src", "tests", "fuzz"] {
        files(&root.join(folder), &mut found);
    }
    let mut bad = vec![];
    for path in &found {
        let relative = path.strip_prefix(root).expect("expected a crate file");
        for part in relative.iter() {
            let part = part.to_string_lossy();
            if banned(part.trim_end_matches(".rs")) {
                bad.push(format!("file {}", relative.display()));
            }
        }
        let text = fs::read_to_string(path).expect("expected a source file");
        let text = code(&text);
        let mut word = String::new();
        for c in text.chars().chain(Some(' ')) {
            if c.is_ascii_alphanumeric() || c == '_' {
                word.push(c);
            } else {
                if !word.is_empty()
                    && !word.starts_with(|c: char| c.is_ascii_digit())
                    && banned(&word)
                    && !OUTSIDE.contains(&word.as_str())
                {
                    bad.push(format!("{}: {}", relative.display(), word));
                }
                word.clear();
            }
        }
    }
    bad.sort();
    bad.dedup();
    assert!(bad.is_empty(), "names with g, j, or y:\n{}", bad.join("\n"));
}
