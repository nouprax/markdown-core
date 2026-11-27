//! The string content type.
//!
//! **String** is a limited [text][] like content type which only allows
//! character escapes and character references.
//! It exists in things such as identifiers (media references, definitions),
//! titles, URLs, code (fenced) info and meta parts.
//!
//! The constructs found in string are:
//!
//! * [Character escape][crate::construct::character_escape]
//! * [Character reference][crate::construct::character_reference]
//!
//! [text]: crate::construct::text

use crate::choice::{self, Choice, Item, List};
use crate::construct::partial_whitespace::resolve_whitespace;
use crate::construct::{character_escape, character_reference, partial_data};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::Tokenizer;

/// Constructs in string, in the order they are tried.
///
/// Data when none of these matched.
pub const CHOICE: Choice = Choice::new(
    &[
        Item {
            starts: character_reference::STARTS,
            step: StateName::CharacterReference(character_reference::Step::Start),
        },
        Item {
            starts: character_escape::STARTS,
            step: StateName::CharacterEscape(character_escape::Step::Start),
        },
    ],
    StateName::Plain(Step::Before),
    StateName::Plain(Step::BeforeData),
);

/// Start of string.
///
/// ````markdown
/// > | ```js
///        ^
/// ````
fn start(_tokenizer: &mut Tokenizer) -> State {
    State::Redo(StateName::Plain(Step::Before))
}

/// Before string.
///
/// ````markdown
/// > | ```js
///        ^
/// ````
fn before(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current.is_none() {
        tokenizer.add_resolver(ResolveName::Data);
        tokenizer.add_resolver(ResolveName::Plain);
        State::Ok
    } else {
        State::Redo(choice::start(List::Plain))
    }
}

/// At data.
///
/// ````markdown
/// > | ```js
///        ^
/// ````
fn before_data(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(State::Next(StateName::Plain(Step::Before)), State::Nok);
    State::Redo(StateName::Data(partial_data::Step::Start {
        list: List::Plain,
    }))
}

/// Resolve whitespace in string.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    resolve_whitespace(tokenizer, false, false);
    None
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    BeforeData,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Plain(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::BeforeData => before_data(tokenizer),
    }
}
