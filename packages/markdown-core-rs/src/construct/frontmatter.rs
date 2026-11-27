//! Frontmatter occurs at the start of the document.
//!
//! ## Grammar
//!
//! Frontmatter forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! frontmatter ::= fence_open *( eol *byte ) eol fence_close
//! fence_open ::= sequence *space_or_tab
//! ; Restriction: markers in `sequence` must match markers in opening sequence.
//! fence_close ::= sequence *space_or_tab
//! sequence ::= 3'+' | 3'-'
//! ```
//!
//! Frontmatter can only occur once.
//! It cannot occur in a container.
//! It must have a closing fence.
//! Like flow constructs, it must be followed by an eol (line ending) or
//! eof (end of file).
//!
//! ## Extension
//!
//! > 👉 **Note**: frontmatter is not part of `CommonMark`.
//!
//! As there is no spec for frontmatter in markdown, this extension follows how
//! YAML frontmatter works on `github.com`.
//! It also parses TOML frontmatter, just like YAML except that it uses a `+`.
//!
//! ## Recommendation
//!
//! When authoring markdown with frontmatter, it’s recommended to use YAML
//! frontmatter if possible.
//! While YAML has some warts, it works in the most places, so using it
//! guarantees the highest chance of portability.
//!
//! In certain ecosystems, other flavors are widely used.
//! For example, in the Rust ecosystem, TOML is often used.
//! In such cases, using TOML is an okay choice.
//!
//! ## Tokens
//!
//! * [`Frontmatter`][Name::Frontmatter]
//! * [`FrontmatterFence`][Name::FrontmatterFence]
//! * [`FrontmatterSequence`][Name::FrontmatterSequence]
//! * [`FrontmatterChunk`][Name::FrontmatterChunk]
//! * [`LineEnding`][Name::LineEnding]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! ## References
//!
//! * [`micromark-extension-frontmatter`](https://github.com/micromark/micromark-extension-frontmatter)
//!

use crate::construct::partial_space_or_tab::space_or_tab;
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
    if matches!(tokenizer.current, Some(b'+' | b'-')) {
        let marker = tokenizer.current.unwrap();
        tokenizer.enter(Name::Frontmatter);
        tokenizer.enter(Name::FrontmatterFence);
        tokenizer.enter(Name::FrontmatterSequence);
        State::Retry(StateName::Frontmatter(Step::OpenSequence {
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

        if matches!(tokenizer.current, Some(b'\t' | b' ')) {
            tokenizer.attempt(
                State::Next(StateName::Frontmatter(Step::OpenAfter { marker })),
                State::Nok,
            );
            State::Retry(space_or_tab())
        } else {
            State::Retry(StateName::Frontmatter(Step::OpenAfter { marker }))
        }
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
        State::Retry(StateName::Frontmatter(Step::CloseSequence {
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

        if matches!(tokenizer.current, Some(b'\t' | b' ')) {
            tokenizer.attempt(
                State::Next(StateName::Frontmatter(Step::CloseAfter)),
                State::Nok,
            );
            State::Retry(space_or_tab())
        } else {
            State::Retry(StateName::Frontmatter(Step::CloseAfter))
        }
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
        None | Some(b'\n') => State::Retry(StateName::Frontmatter(Step::ContentEnd { marker })),
        Some(_) => {
            tokenizer.enter(Name::FrontmatterChunk);
            State::Retry(StateName::Frontmatter(Step::ContentInside { marker }))
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
            State::Retry(StateName::Frontmatter(Step::ContentEnd { marker }))
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
