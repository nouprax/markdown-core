//! Turn bytes of markdown into events.

use crate::construct::document;
use crate::event::{Event, Point};
use crate::state::{Name as StateName, State};
use crate::subtokenize::subtokenize;
use crate::tokenizer::{Definitions, Tokenizer};
use alloc::vec::Vec;

/// Info needed, in all content types, when parsing markdown.
///
/// Importantly, this contains a set of known definitions.
/// It also references the input value as bytes (`u8`).
#[derive(Debug)]
pub struct ParseState<'a> {
    /// List of chars.
    pub source: &'a [u8],
    /// Identifiers defined in the whole document.
    pub definitions: Definitions,
}

/// Turn a string of markdown into events.
///
/// Passes the bytes back so the compiler can read the source.
pub fn parse(value: &str) -> (Vec<Event>, &[u8]) {
    let source = value.as_bytes();

    let mut parse_state = ParseState {
        source,
        definitions: Definitions::default(),
    };

    let start = Point {
        line: 1,
        column: 1,
        index: 0,
        vs: 0,
    };
    let mut tokenizer = Tokenizer::new(start, &parse_state);

    let state = tokenizer.push(
        (0, 0),
        (parse_state.source.len(), 0),
        State::Next(StateName::Document(document::Step::Start)),
    );
    let mut result = tokenizer.flush(state, true);
    let mut events = tokenizer.events;

    loop {
        parse_state.definitions.append(&mut result.definitions);

        if result.done {
            return (events, parse_state.source);
        }

        result = subtokenize(&mut events, &parse_state, None);
    }
}
