//! Label start (note) occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Label start (note) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! label_start_note ::= '^' '['
//! ```
//!
//! Matched with a [label end][label_end], it forms an inline note: a
//! footnote whose content is the label, cited where it is written.
//! The label is not empty and not only spaces and tabs, and the end takes no
//! resource or reference after it.
//! Links inside the label do not stop links around the note.
//!
//! ## Tokens
//!
//! * [`LabelNote`][Name::LabelNote]
//! * [`LabelNoteMarker`][Name::LabelNoteMarker]
//! * [`LabelMarker`][Name::LabelMarker]
//!
//! [text]: crate::construct::text
//! [label_end]: crate::construct::label_end

use crate::choice::Starts;
use crate::event::Name;
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::tokenizer::{LabelKind, LabelStart, Tokenizer};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b"^");

/// Start of label (note) start.
///
/// ```markdown
/// > | a ^[b] c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'^') {
        tokenizer.enter(Name::LabelNote);
        tokenizer.enter(Name::LabelNoteMarker);
        tokenizer.consume();
        tokenizer.exit(Name::LabelNoteMarker);
        State::Next(StateName::LabelStartNote(Step::Open))
    } else {
        State::Nok
    }
}

/// After `^`, at `[`.
///
/// ```markdown
/// > | a ^[b] c
///        ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'[') {
        tokenizer.enter(Name::LabelMarker);
        tokenizer.consume();
        tokenizer.exit(Name::LabelMarker);
        tokenizer.exit(Name::LabelNote);
        tokenizer.labels.starts.push(LabelStart {
            kind: LabelKind::Note,
            start: (tokenizer.events.len() - 6, tokenizer.events.len() - 1),
            inactive: false,
        });
        tokenizer.register_resolver_before(ResolveName::Label);
        State::Ok
    } else {
        State::Nok
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Open,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::LabelStartNote(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Open => open(tokenizer),
    }
}
