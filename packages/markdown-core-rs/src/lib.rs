//! Markdown Core: the parser of the repository dialect.
//!
//! [`parse()`][] reads Markdown into the canonical AST
//! (`docs/specs/canonical-ast.json`), and [`dump::dump()`][] writes that AST
//! in the canonical dump format.

#![no_std]
#![deny(clippy::pedantic)]
#![allow(clippy::doc_link_with_quotes)]
#![allow(clippy::missing_panics_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::too_many_lines)]

extern crate alloc;
pub mod ast;
mod construct;
pub mod dump;
mod event;
mod parser;
mod resolve;
mod state;
mod subtokenize;
mod to_ast;
mod tokenizer;
mod util;

use alloc::string::String;

/// Parse `source` into the canonical AST.
pub fn parse(source: &str) -> ast::Node {
    let (events, bytes) = parser::parse(source);
    to_ast::compile(&events, bytes)
}

/// The event stream of `source`, one line per event, for debugging.
#[doc(hidden)]
pub fn events(source: &str) -> String {
    use core::fmt::Write;
    let (events, _) = parser::parse(source);
    let mut out = String::new();
    let mut depth = 0usize;
    for event in &events {
        if event.kind == event::Kind::Exit {
            depth -= 1;
        }
        let _ = writeln!(
            out,
            "{}{:?} {:?} {}:{}@{}{}",
            "  ".repeat(depth),
            event.kind,
            event.name,
            event.point.line,
            event.point.column,
            event.point.index,
            if event.point.vs > 0 {
                alloc::format!("+{}", event.point.vs)
            } else {
                String::new()
            }
        );
        if event.kind == event::Kind::Enter {
            depth += 1;
        }
    }
    out
}
