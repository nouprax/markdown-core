//! Cross link occurs in the [text][] content type.
//! It forms cross links (`[[a]]`) and cross embeds (`![[a]]`).
//!
//! ## Grammar
//!
//! Cross link forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! cross_link ::= ['!'] '[[' dest [separator label] ']]' [attributes]
//!
//! ; Restriction: not empty; no `[`, `]`, or line ending.
//! ; Restriction: no part between `#` signs is empty, and the last is not.
//! dest ::= path *('#' part)
//! separator ::= '|' | '\|'
//! ; Restriction: no `[`, `]`, or line ending.
//! label ::= *byte
//! ```
//!
//! The destination and label are raw: no construct is recognized in them.
//! The path is the destination up to its first `#`, the anchor what follows it;
//! an anchor `^id` (one part, letters, digits, and dashes) is a block
//! identifier.
//! An embed whose label ends in dimensions (`100`, `100x200`) after its last
//! separator has those dimensions.
//! [Attributes][parts::attributes] right after the closing brackets are
//! the node's.
//!
//! ## Tokens
//!
//! * [`CrossLink`][Name::CrossLink]
//! * [`CrossLinkMarker`][Name::CrossLinkMarker]
//! * [`CrossLinkDest`][Name::CrossLinkDest]
//! * [`CrossLinkSeparator`][Name::CrossLinkSeparator]
//! * [`CrossLinkLabel`][Name::CrossLinkLabel]
//!
//! [text]: crate::construct::text
//! [parts::attributes]: crate::construct::parts::attributes

use crate::choice::Starts;
use crate::construct::parts;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b"![");

/// Start of a cross link.
///
/// ```markdown
/// > | a ![[b]] c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'!') => {
            tokenizer.enter(Name::CrossLink);
            tokenizer.enter(Name::CrossLinkMarker);
            tokenizer.consume();
            State::Next(StateName::CrossLink(Step::Open))
        }
        Some(b'[') => {
            tokenizer.enter(Name::CrossLink);
            tokenizer.enter(Name::CrossLinkMarker);
            State::Redo(StateName::CrossLink(Step::Open))
        }
        _ => State::Nok,
    }
}

/// At the first `[`.
///
/// ```markdown
/// > | a [[b]] c
///       ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'[') {
        tokenizer.consume();
        State::Next(StateName::CrossLink(Step::OpenSecond))
    } else {
        State::Nok
    }
}

/// At the second `[`.
///
/// ```markdown
/// > | a [[b]] c
///        ^
/// ```
fn open_second(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'[') {
        tokenizer.consume();
        tokenizer.exit(Name::CrossLinkMarker);
        State::Next(StateName::CrossLink(Step::Dest {
            part: Part::Bare,
            open: false,
        }))
    } else {
        State::Nok
    }
}

/// What the destination has read so far.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Part {
    /// Nothing.
    Bare,
    /// Bytes, the last part not empty.
    Filled,
    /// Bytes, ending in a `#` that starts an empty part.
    Hash,
}

/// In the destination; `open` is whether a destination token is open.
///
/// ```markdown
/// > | a [[b#c]] c
///         ^^^
/// ```
fn dest(tokenizer: &mut Tokenizer, part: Part, open: bool) -> State {
    match tokenizer.current {
        None | Some(b'\n' | b'[') => State::Nok,
        Some(b']' | b'|') if part != Part::Filled => State::Nok,
        Some(b']') => {
            tokenizer.exit(Name::CrossLinkDest);
            State::Redo(StateName::CrossLink(Step::Close))
        }
        Some(b'|') => {
            tokenizer.exit(Name::CrossLinkDest);
            tokenizer.enter(Name::CrossLinkSeparator);
            tokenizer.consume();
            tokenizer.exit(Name::CrossLinkSeparator);
            State::Next(StateName::CrossLink(Step::LabelStart))
        }
        // `\|` is the separator; any other backslash is a byte of the
        // destination. In a cell, where `\\|` is `\|`, so is `\\\|`.
        Some(b'\\') if separator(tokenizer) => {
            if part != Part::Filled {
                return State::Nok;
            }
            if open {
                tokenizer.exit(Name::CrossLinkDest);
            }
            tokenizer.enter(Name::CrossLinkSeparator);
            tokenizer.consume();
            State::Next(StateName::CrossLink(Step::Pipe))
        }
        Some(b'#') if part == Part::Hash => State::Nok,
        Some(code) => {
            if !open {
                tokenizer.enter(Name::CrossLinkDest);
            }
            tokenizer.consume();
            State::Next(StateName::CrossLink(Step::Dest {
                part: if code == b'#' {
                    Part::Hash
                } else {
                    Part::Filled
                },
                open: true,
            }))
        }
    }
}

/// Whether the backslash at the current byte starts a separator: `\\|`, or
/// in a cell also `\\\\|`.
fn separator(tokenizer: &Tokenizer) -> bool {
    let bytes = tokenizer.parse_state.bytes;
    let at = tokenizer.point.index;
    match bytes.get(at + 1) {
        Some(b'|') => true,
        Some(b'\\') => tokenizer.cell && bytes.get(at + 2) == Some(&b'|'),
        _ => false,
    }
}

/// After a backslash in the destination, at the `|` that makes them a
/// separator.
///
/// ```markdown
/// > | a [[b\|c]] d
///           ^
/// ```
fn pipe(tokenizer: &mut Tokenizer) -> State {
    // The second backslash of `\\\|` in a cell.
    if tokenizer.current == Some(b'\\') {
        tokenizer.consume();
        return State::Next(StateName::CrossLink(Step::Pipe));
    }
    tokenizer.consume();
    tokenizer.exit(Name::CrossLinkSeparator);
    State::Next(StateName::CrossLink(Step::LabelStart))
}

/// After the separator, at the label.
///
/// ```markdown
/// > | a [[b|c]] d
///           ^
/// ```
fn label_start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b']') {
        State::Redo(StateName::CrossLink(Step::Close))
    } else {
        tokenizer.enter(Name::CrossLinkLabel);
        State::Redo(StateName::CrossLink(Step::Label))
    }
}

/// In the label.
///
/// ```markdown
/// > | a [[b|c]] d
///           ^
/// ```
fn label(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n' | b'[') => State::Nok,
        Some(b']') => {
            tokenizer.exit(Name::CrossLinkLabel);
            State::Redo(StateName::CrossLink(Step::Close))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::CrossLink(Step::Label))
        }
    }
}

/// At the first `]`.
///
/// ```markdown
/// > | a [[b]] c
///          ^
/// ```
fn close(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::CrossLinkMarker);
    tokenizer.consume();
    State::Next(StateName::CrossLink(Step::CloseSecond))
}

/// At the second `]`.
///
/// ```markdown
/// > | a [[b]] c
///           ^
/// ```
fn close_second(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b']') {
        tokenizer.consume();
        tokenizer.exit(Name::CrossLinkMarker);
        State::Next(StateName::Attributes(parts::attributes::Step::After(
            Name::CrossLink,
        )))
    } else {
        State::Nok
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Open,
    OpenSecond,
    Dest { part: Part, open: bool },
    Pipe,
    LabelStart,
    Label,
    Close,
    CloseSecond,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::CrossLink(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Open => open(tokenizer),
        Step::OpenSecond => open_second(tokenizer),
        Step::Dest { part, open } => dest(tokenizer, part, open),
        Step::Pipe => pipe(tokenizer),
        Step::LabelStart => label_start(tokenizer),
        Step::Label => label(tokenizer),
        Step::Close => close(tokenizer),
        Step::CloseSecond => close_second(tokenizer),
    }
}
