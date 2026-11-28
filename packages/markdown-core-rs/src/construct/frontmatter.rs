//! Frontmatter occurs at the start of the document.
//!
//! ## Grammar
//!
//! Frontmatter forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! frontmatter ::= fence_open *( eol *byte ) eol fence_close
//! fence_open ::= sequence
//! fence_close ::= sequence
//! sequence ::= 3'-'
//! ```
//!
//! Frontmatter can only occur once, at the document’s first byte.
//! It cannot occur in a container.
//! It must have a closing fence: the first later line that is exactly
//! `---`, which can end the file.
//! The fences hold no spaces or tabs.
//!
//! ## Extension
//!
//! > 👉 **Note**: frontmatter is not part of `CommonMark`.
//!
//! The dialect reads its payload as properties
//! ([`properties`][crate::util::properties]).
//!
//! ## Tokens
//!
//! * [`Frontmatter`][Name::Frontmatter]
//! * [`FrontmatterFence`][Name::FrontmatterFence]
//! * [`FrontmatterSequence`][Name::FrontmatterSequence]
//! * [`FrontmatterChunk`][Name::FrontmatterChunk]
//! * [`LineEnding`][Name::LineEnding]
//!
//! ## References
//!
//! * [`micromark-extension-frontmatter`](https://github.com/micromark/micromark-extension-frontmatter)
//!

use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::constant::FRONTMATTER_SEQUENCE_SIZE;

/// Start of frontmatter.
///
/// ```markdown
/// > | ---
///     ^
///   | title: "Venus"
///   | ---
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    // Indent not allowed.
    if tokenizer.current == Some(b'-') {
        let marker = tokenizer.current.unwrap();
        tokenizer.enter(Name::Frontmatter);
        tokenizer.enter(Name::FrontmatterFence);
        tokenizer.enter(Name::FrontmatterSequence);
        State::Redo(StateName::Frontmatter(Step::OpenSequence {
            marker,
            size: 0,
        }))
    } else {
        State::Nok
    }
}

/// In open sequence.
///
/// ```markdown
/// > | ---
///     ^
///   | title: "Venus"
///   | ---
/// ```
fn open_sequence(tokenizer: &mut Tokenizer, marker: u8, size: usize) -> State {
    if tokenizer.current == Some(marker) {
        tokenizer.consume();
        State::Next(StateName::Frontmatter(Step::OpenSequence {
            marker,
            size: size + 1,
        }))
    } else if size == FRONTMATTER_SEQUENCE_SIZE {
        tokenizer.exit(Name::FrontmatterSequence);
        State::Redo(StateName::Frontmatter(Step::OpenAfter { marker }))
    } else {
        State::Nok
    }
}

/// After open sequence.
///
/// ```markdown
/// > | ---
///        ^
///   | title: "Venus"
///   | ---
/// ```
fn open_after(tokenizer: &mut Tokenizer, marker: u8) -> State {
    if let Some(b'\n') = tokenizer.current {
        tokenizer.exit(Name::FrontmatterFence);
        tokenizer.enter(Name::LineEnding);
        tokenizer.consume();
        tokenizer.exit(Name::LineEnding);
        tokenizer.attempt(
            State::Next(StateName::Frontmatter(Step::After)),
            State::Next(StateName::Frontmatter(Step::ContentStart { marker })),
        );
        State::Next(StateName::Frontmatter(Step::CloseStart { marker }))
    } else {
        State::Nok
    }
}

/// Start of close sequence.
///
/// ```markdown
///   | ---
///   | title: "Venus"
/// > | ---
///     ^
/// ```
fn close_start(tokenizer: &mut Tokenizer, marker: u8) -> State {
    if tokenizer.current == Some(marker) {
        tokenizer.enter(Name::FrontmatterFence);
        tokenizer.enter(Name::FrontmatterSequence);
        State::Redo(StateName::Frontmatter(Step::CloseSequence {
            marker,
            size: 0,
        }))
    } else {
        State::Nok
    }
}

/// In close sequence.
///
/// ```markdown
///   | ---
///   | title: "Venus"
/// > | ---
///     ^
/// ```
fn close_sequence(tokenizer: &mut Tokenizer, marker: u8, size: usize) -> State {
    if tokenizer.current == Some(marker) {
        tokenizer.consume();
        State::Next(StateName::Frontmatter(Step::CloseSequence {
            marker,
            size: size + 1,
        }))
    } else if size == FRONTMATTER_SEQUENCE_SIZE {
        tokenizer.exit(Name::FrontmatterSequence);
        State::Redo(StateName::Frontmatter(Step::CloseAfter))
    } else {
        State::Nok
    }
}

/// After close sequence.
///
/// ```markdown
///   | ---
///   | title: "Venus"
/// > | ---
///        ^
/// ```
fn close_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::FrontmatterFence);
            State::Ok
        }
        _ => State::Nok,
    }
}

/// Start of content chunk.
///
/// ```markdown
///   | ---
/// > | title: "Venus"
///     ^
///   | ---
/// ```
fn content_start(tokenizer: &mut Tokenizer, marker: u8) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Redo(StateName::Frontmatter(Step::ContentEnd { marker })),
        Some(_) => {
            tokenizer.enter(Name::FrontmatterChunk);
            State::Redo(StateName::Frontmatter(Step::ContentInside { marker }))
        }
    }
}

/// In content chunk.
///
/// ```markdown
///   | ---
/// > | title: "Venus"
///     ^
///   | ---
/// ```
fn content_inside(tokenizer: &mut Tokenizer, marker: u8) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::FrontmatterChunk);
            State::Redo(StateName::Frontmatter(Step::ContentEnd { marker }))
        }
        Some(_) => {
            tokenizer.consume();
            State::Next(StateName::Frontmatter(Step::ContentInside { marker }))
        }
    }
}

/// End of content chunk.
///
/// ```markdown
///   | ---
/// > | title: "Venus"
///                   ^
///   | ---
/// ```
fn content_end(tokenizer: &mut Tokenizer, marker: u8) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            tokenizer.attempt(
                State::Next(StateName::Frontmatter(Step::After)),
                State::Next(StateName::Frontmatter(Step::ContentStart { marker })),
            );
            State::Next(StateName::Frontmatter(Step::CloseStart { marker }))
        }
        Some(_) => unreachable!("expected eof/eol"),
    }
}

/// After frontmatter.
///
/// ```markdown
///   | ---
///   | title: "Venus"
/// > | ---
///        ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    debug_assert!(
        matches!(tokenizer.current, None | Some(b'\n')),
        "expected eol/eof after closing fence"
    );
    tokenizer.exit(Name::Frontmatter);
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    OpenSequence { marker: u8, size: usize },
    OpenAfter { marker: u8 },
    After,
    ContentStart { marker: u8 },
    ContentInside { marker: u8 },
    ContentEnd { marker: u8 },
    CloseStart { marker: u8 },
    CloseSequence { marker: u8, size: usize },
    CloseAfter,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Frontmatter(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::OpenSequence { marker, size } => open_sequence(tokenizer, marker, size),
        Step::OpenAfter { marker } => open_after(tokenizer, marker),
        Step::After => after(tokenizer),
        Step::ContentStart { marker } => content_start(tokenizer, marker),
        Step::ContentInside { marker } => content_inside(tokenizer, marker),
        Step::ContentEnd { marker } => content_end(tokenizer, marker),
        Step::CloseStart { marker } => close_start(tokenizer, marker),
        Step::CloseSequence { marker, size } => close_sequence(tokenizer, marker, size),
        Step::CloseAfter => close_after(tokenizer),
    }
}
