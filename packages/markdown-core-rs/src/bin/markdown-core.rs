//! Print the canonical AST dump of a Markdown file.
//!
//! `markdown-core FILE` prints the dump; `markdown-core --events FILE`
//! prints the event stream.

use std::{env, fs, process};

fn main() {
    let words: Vec<String> = env::args().skip(1).collect();
    let (events, path) = match words.as_slice() {
        [switch, path] if switch == "--events" => (true, path),
        [path] => (false, path),
        _ => {
            eprintln!("usage: markdown-core [--events] FILE");
            process::exit(2);
        }
    };
    let source = match fs::read_to_string(path) {
        Ok(source) => source,
        Err(error) => {
            eprintln!("markdown-core: {}: {}", path, error);
            process::exit(1);
        }
    };
    if events {
        print!("{}", markdown_core::events(&source));
    } else {
        let document = markdown_core::parse(&source);
        print!("{}", markdown_core::dump::dump(&document, &source));
    }
}
