//! Directive (text) occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Directive (text) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! directive_text ::= ':' name (label [attributes] | attributes)
//!
//! ; Restriction: the `:` is not after another `:`.
//! ```
//!
//! See [`partial_directive`][crate::construct::partial_directive] for the
//! name and the label.
//! A name alone is text, so the colon in `12:30` stays text: a label or
//! attributes must follow.
//! See [`partial_attributes`][crate::construct::partial_attributes] for
//! attributes.
//!
//! ## Tokens
//!
//! * [`DirectiveText`][Name::DirectiveText]
//! * [`DirectiveTextMarker`][Name::DirectiveTextMarker]
//!
//! ## References
//!
//! * [*Generic directives/plugins syntax*](https://talk.commonmark.org/t/generic-directives-plugins-syntax/444)
//! * [`micromark-extension-directive`](https://github.com/micromark/micromark-extension-directive)
//!
//! [text]: crate::construct::text

use crate::choice::Starts;
use crate::construct::{partial_attributes, partial_directive};
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Bytes this construct may start at.
pub const STARTS: Starts = Starts::Bytes(b":");

/// Start of a directive (text).
///
/// ```markdown
/// > | a :b[c] d
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    // A run of colons stays whole.
    if tokenizer.current != Some(b':') || tokenizer.previous == Some(b':') {
        return State::Nok;
    }
    tokenizer.enter(Name::DirectiveText);
    tokenizer.enter(Name::DirectiveTextMarker);
    tokenizer.consume();
    tokenizer.exit(Name::DirectiveTextMarker);
    tokenizer.attempt(
        State::Next(StateName::DirectiveText(Step::NameAfter)),
        State::Nok,
    );
    State::Next(StateName::Directive(partial_directive::Step::NameStart))
}

/// After the name: a label or attributes must follow.
///
/// ```markdown
/// > | a :b[c] d
///         ^
/// ```
fn name_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'[') => {
            tokenizer.attempt(
                State::Next(StateName::Attributes(partial_attributes::Step::After(
                    Name::DirectiveText,
                ))),
                State::Nok,
            );
            State::Redo(StateName::Directive(partial_directive::Step::LabelStart {
                line: false,
            }))
        }
        Some(b'{') => {
            tokenizer.attempt(
                State::Next(StateName::Attributes(partial_attributes::Step::Close(
                    Name::DirectiveText,
                ))),
                State::Nok,
            );
            State::Redo(StateName::Attributes(partial_attributes::Step::Start {
                line: false,
            }))
        }
        _ => State::Nok,
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    NameAfter,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::DirectiveText(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::NameAfter => name_after(tokenizer),
    }
}
