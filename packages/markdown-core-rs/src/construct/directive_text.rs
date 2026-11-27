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
//! name ::= 1*(byte - whitespace - '[' - '{' - ':')
//! ; Restriction: brackets in the label are balanced, at most 32 deep, and
//! ; a backslash escapes the byte after it.
//! label ::= '[' *byte ']'
//! ```
//!
//! A name is any string without whitespace: `:中文[a]` names `中文`.
//! A name alone is text, so the colon in `12:30` stays text: a label or
//! attributes must follow.
//! The label’s content is text.
//! See [`partial_attributes`][crate::construct::partial_attributes] for
//! attributes.
//!
//! ## Tokens
//!
//! * [`DirectiveText`][Name::DirectiveText]
//! * [`DirectiveTextMarker`][Name::DirectiveTextMarker]
//! * [`DirectiveNameText`][Name::DirectiveNameText]
//! * [`DirectiveLabel`][Name::DirectiveLabel]
//! * [`DirectiveLabelMarker`][Name::DirectiveLabelMarker]
//! * [`DirectiveLabelText`][Name::DirectiveLabelText]
//!
//! ## References
//!
//! * [*Generic directives/plugins syntax*](https://talk.commonmark.org/t/generic-directives-plugins-syntax/444)
//! * [`micromark-extension-directive`](https://github.com/micromark/micromark-extension-directive)
//!
//! [text]: crate::construct::text

use crate::choice::Starts;
use crate::construct::partial_attributes;
use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::subtokenize::link;
use crate::tokenizer::Tokenizer;

/// Bytes this construct may start at.
pub const STARTS: Starts = Starts::Bytes(b":");

/// The most brackets a label may hold open inside it.
const LABEL_DEPTH_MAX: usize = 32;

/// Whether `byte` ends a name.
fn name_end(byte: Option<u8>) -> bool {
    matches!(
        byte,
        None | Some(b'\t' | b'\n' | b'\x0b' | b'\x0c' | b'\r' | b' ' | b'[' | b'{' | b':')
    )
}

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
    State::Next(StateName::DirectiveText(Step::NameStart))
}

/// After the colon, at the name.
///
/// ```markdown
/// > | a :b[c] d
///        ^
/// ```
fn name_start(tokenizer: &mut Tokenizer) -> State {
    if name_end(tokenizer.current) {
        State::Nok
    } else {
        tokenizer.enter(Name::DirectiveNameText);
        tokenizer.consume();
        State::Next(StateName::DirectiveText(Step::Name))
    }
}

/// In the name.
///
/// ```markdown
/// > | a :bc[d] e
///         ^
/// ```
fn name(tokenizer: &mut Tokenizer) -> State {
    if name_end(tokenizer.current) {
        tokenizer.exit(Name::DirectiveNameText);
        State::Redo(StateName::DirectiveText(Step::NameAfter))
    } else {
        tokenizer.consume();
        State::Next(StateName::DirectiveText(Step::Name))
    }
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
            State::Redo(StateName::DirectiveText(Step::LabelStart))
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

/// At the label’s opening bracket.
///
/// ```markdown
/// > | a :b[c] d
///         ^
/// ```
fn label_start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::DirectiveLabel);
    tokenizer.enter(Name::DirectiveLabelMarker);
    tokenizer.consume();
    tokenizer.exit(Name::DirectiveLabelMarker);
    State::Next(StateName::DirectiveText(Step::LabelBefore {
        depth: 0,
        connect: false,
    }))
}

/// In the label, before a chunk of its text, or at its closing bracket;
/// `connect` is whether a chunk on an earlier line goes on in it.
///
/// ```markdown
/// > | a :b[c] d
///          ^
/// ```
fn label_before(tokenizer: &mut Tokenizer, depth: usize, connect: bool) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b']') if depth == 0 => State::Redo(StateName::DirectiveText(Step::LabelClose)),
        _ => {
            tokenizer.enter_link(
                Name::DirectiveLabelText,
                Link {
                    previous: None,
                    next: None,
                    content: Content::Text,
                },
            );
            if connect {
                let index = tokenizer.events.len() - 1;
                link(&mut tokenizer.events, index);
            }
            State::Redo(StateName::DirectiveText(Step::LabelInside { depth }))
        }
    }
}

/// In a chunk of the label’s text.
///
/// ```markdown
/// > | a :b[c] d
///          ^
/// ```
fn label_inside(tokenizer: &mut Tokenizer, depth: usize) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b']') if depth == 0 => {
            tokenizer.exit(Name::DirectiveLabelText);
            State::Redo(StateName::DirectiveText(Step::LabelClose))
        }
        Some(b'\n') => {
            tokenizer.consume();
            tokenizer.exit(Name::DirectiveLabelText);
            State::Next(StateName::DirectiveText(Step::LabelBefore {
                depth,
                connect: true,
            }))
        }
        Some(b'[') if depth == LABEL_DEPTH_MAX => State::Nok,
        Some(byte) => {
            tokenizer.consume();
            State::Next(StateName::DirectiveText(match byte {
                b'\\' => Step::LabelEscape { depth },
                b'[' => Step::LabelInside { depth: depth + 1 },
                b']' => Step::LabelInside { depth: depth - 1 },
                _ => Step::LabelInside { depth },
            }))
        }
    }
}

/// In the label, after a backslash, which escapes the byte after it.
///
/// ```markdown
/// > | a :b[c\]] d
///            ^
/// ```
fn label_escape(tokenizer: &mut Tokenizer, depth: usize) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Redo(StateName::DirectiveText(Step::LabelInside { depth })),
        _ => {
            tokenizer.consume();
            State::Next(StateName::DirectiveText(Step::LabelInside { depth }))
        }
    }
}

/// At the label’s closing bracket.
///
/// ```markdown
/// > | a :b[c] d
///           ^
/// ```
fn label_close(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::DirectiveLabelMarker);
    tokenizer.consume();
    tokenizer.exit(Name::DirectiveLabelMarker);
    tokenizer.exit(Name::DirectiveLabel);
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    NameStart,
    Name,
    NameAfter,
    LabelStart,
    LabelBefore { depth: usize, connect: bool },
    LabelInside { depth: usize },
    LabelEscape { depth: usize },
    LabelClose,
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
        Step::NameStart => name_start(tokenizer),
        Step::Name => name(tokenizer),
        Step::NameAfter => name_after(tokenizer),
        Step::LabelStart => label_start(tokenizer),
        Step::LabelBefore { depth, connect } => label_before(tokenizer, depth, connect),
        Step::LabelInside { depth } => label_inside(tokenizer, depth),
        Step::LabelEscape { depth } => label_escape(tokenizer, depth),
        Step::LabelClose => label_close(tokenizer),
    }
}
