//! Print the canonical AST dump of a Markdown file.
//!
//! `markdown-core FILE` prints the dump; `markdown-core --events FILE`
//! prints the event stream; `markdown-core --batch FILE` reads records, each
//! a byte count, a line ending, and that many bytes of Markdown, and prints
//! the dump of each as a record too. Built with the `cover` feature, each
//! dump record is followed by a record of the transitions the parse ran, as
//! `markdown_core::cover::take` writes it.

use std::io::Write;
use std::{env, fs, io, process};

fn main() {
    let arguments: Vec<String> = env::args().skip(1).collect();
    let (mode, path) = match arguments.as_slice() {
        [flag, path] if flag == "--events" || flag == "--batch" => (flag.as_str(), path),
        [path] => ("", path),
        _ => {
            eprintln!("usage: markdown-core [--events | --batch] FILE");
            process::exit(2);
        }
    };
    let input = match fs::read(path) {
        Ok(input) => input,
        Err(error) => {
            eprintln!("markdown-core: {}: {}", path, error);
            process::exit(1);
        }
    };
    if mode == "--batch" {
        batch(&input);
        return;
    }
    let source = match String::from_utf8(input) {
        Ok(source) => source,
        Err(error) => {
            eprintln!("markdown-core: {}: {}", path, error);
            process::exit(1);
        }
    };
    if mode == "--events" {
        print!("{}", markdown_core::events(&source));
    } else {
        let document = markdown_core::parse(&source);
        print!("{}", markdown_core::dump::dump(&document, &source));
    }
}

/// Parse each record of `input` and print its dump as a record.
fn batch(input: &[u8]) {
    let stdout = io::stdout();
    let mut out = io::BufWriter::new(stdout.lock());
    let mut at = 0;
    #[cfg(feature = "cover")]
    let mut hits = String::new();
    while at < input.len() {
        let line = input[at..]
            .iter()
            .position(|byte| *byte == b'\n')
            .map_or(input.len(), |end| at + end);
        let size: usize = match std::str::from_utf8(&input[at..line])
            .ok()
            .and_then(|text| text.parse().ok())
        {
            Some(size) => size,
            None => {
                eprintln!("markdown-core: bad record at byte {}", at);
                process::exit(1);
            }
        };
        let start = line + 1;
        let end = start + size;
        let source = match input.get(start..end).map(std::str::from_utf8) {
            Some(Ok(source)) => source,
            _ => {
                eprintln!("markdown-core: bad record at byte {}", at);
                process::exit(1);
            }
        };
        let dump = markdown_core::dump::dump(&markdown_core::parse(source), source);
        record(&mut out, &dump);
        #[cfg(feature = "cover")]
        {
            hits.clear();
            markdown_core::cover::take(&mut hits);
            record(&mut out, &hits);
        }
        at = end;
    }
}

fn record(out: &mut impl Write, text: &str) {
    if write!(out, "{}\n{}", text.len(), text).is_err() {
        process::exit(1);
    }
}
