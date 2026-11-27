//! Specimen reference occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Specimen references form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: the key is bare, and a specimen of the document has it as
//! ; its label.
//! specimen_reference ::= '(' citation_key ')'
//! ```
//!
//! A [citation key][citation] in parentheses names a [specimen][] when its
//! key is the specimen’s label, as written: the parentheses are part of the
//! reference.
//! Otherwise they stay text, and the key is a citation of its own.
//!
//! ## Tokens
//!
//! * [`CitationKey`][Name::CitationKey]
//! * [`SpecimenReference`][Name::SpecimenReference]
//! * [`SpecimenReferenceMarker`][Name::SpecimenReferenceMarker]
//!
//! [text]: crate::construct::text
//! [citation]: crate::construct::citation
//! [specimen]: crate::construct::specimen

use crate::choice::Starts;
use crate::construct::citation;
use crate::event::{Kind, Name};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Bytes this construct may start at.
pub const STARTS: Starts = Starts::Bytes(b"(");

/// Start of a specimen reference, at `(`.
///
/// ```markdown
/// > | a (@b) c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current != Some(b'(') || tokenizer.parse_state.definitions.specimens.is_empty() {
        return State::Nok;
    }
    tokenizer.enter(Name::SpecimenReference);
    tokenizer.enter(Name::SpecimenReferenceMarker);
    tokenizer.consume();
    tokenizer.exit(Name::SpecimenReferenceMarker);
    State::Next(StateName::SpecimenReference(Step::Key))
}

/// At the key.
///
/// ```markdown
/// > | a (@b) c
///        ^
/// ```
fn key(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current != Some(b'@') {
        return State::Nok;
    }
    tokenizer.attempt(
        State::Next(StateName::SpecimenReference(Step::Close)),
        State::Nok,
    );
    State::Redo(StateName::Citation(citation::Step::Start))
}

/// After the key, at `)`.
///
/// ```markdown
/// > | a (@b) c
///          ^
/// ```
fn close(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current != Some(b')') {
        return State::Nok;
    }
    let events = &tokenizer.events;
    let mut index = events.len() - 1;
    while !(events[index].name == Name::CitationKeyText && events[index].kind == Kind::Exit) {
        if events[index].name == Name::CitationBrace {
            return State::Nok;
        }
        index -= 1;
    }
    let text =
        &tokenizer.parse_state.bytes[events[index - 1].point.index..events[index].point.index];
    if !tokenizer
        .parse_state
        .definitions
        .specimens
        .iter()
        .any(|label| label.as_bytes() == text)
    {
        return State::Nok;
    }
    tokenizer.enter(Name::SpecimenReferenceMarker);
    tokenizer.consume();
    tokenizer.exit(Name::SpecimenReferenceMarker);
    tokenizer.exit(Name::SpecimenReference);
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Key,
    Close,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::SpecimenReference(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Key => key(tokenizer),
        Step::Close => close(tokenizer),
    }
}
