//! Comment occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Comment forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: the body ends at the first `%%` after the opener.
//! comment ::= '%%' *byte '%%'
//! ```
//!
//! The opener is `%%` at the cursor, so `%%%a%%%` is the comment `%a`
//! followed by the text `%`, and `%%%%` is an empty comment.
//! Every byte of the body is the comment's, as written: no construct is
//! recognized inside it.
//! The body may span the lines of its text; the whitespace that starts a
//! continuation line is not part of it.
//! An opener without a closer is not a comment.
//!
//! ## Tokens
//!
//! * [`Comment`][Name::Comment]
//! * [`CommentSequence`][Name::CommentSequence]
//! * [`CommentText`][Name::CommentText]
//! * [`LineEnd`][Name::LineEnd]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! [text]: crate::construct::text

use crate::choice::Starts;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Codes(b"%");

/// Start of comment.
///
/// ```markdown
/// > | a %%b%% c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'%') {
        tokenizer.enter(Name::Comment);
        tokenizer.enter(Name::CommentSequence);
        tokenizer.consume();
        State::Next(StateName::Comment(Step::Open))
    } else {
        State::Nok
    }
}

/// After the first sign of the opener.
///
/// ```markdown
/// > | a %%b%% c
///        ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'%') {
        tokenizer.consume();
        tokenizer.exit(Name::CommentSequence);
        State::Next(StateName::Comment(Step::Between))
    } else {
        State::Nok
    }
}

/// In the body, between parts.
///
/// ```markdown
/// > | a %%b%% c
///         ^
/// ```
fn between(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnd);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnd);
            State::Next(StateName::Comment(Step::Line))
        }
        Some(b'%') => {
            tokenizer.enter(Name::CommentSequence);
            tokenizer.consume();
            State::Next(StateName::Comment(Step::Close))
        }
        _ => {
            tokenizer.enter(Name::CommentText);
            State::Redo(StateName::Comment(Step::Text))
        }
    }
}

/// At the start of a continuation line.
///
/// ```markdown
///   | a %%b
/// > |   c%% d
///     ^
/// ```
fn line(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.enter(Name::SpaceOrTab);
        State::Redo(StateName::Comment(Step::Indent))
    } else {
        State::Redo(StateName::Comment(Step::Between))
    }
}

/// In the whitespace that starts a continuation line.
///
/// ```markdown
///   | a %%b
/// > |   c%% d
///     ^
/// ```
fn indent(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.consume();
        State::Next(StateName::Comment(Step::Indent))
    } else {
        tokenizer.exit(Name::SpaceOrTab);
        State::Redo(StateName::Comment(Step::Between))
    }
}

/// In body text.
///
/// ```markdown
/// > | a %%b%% c
///         ^
/// ```
fn text(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n' | b'%')) {
        tokenizer.exit(Name::CommentText);
        State::Redo(StateName::Comment(Step::Between))
    } else {
        tokenizer.consume();
        State::Next(StateName::Comment(Step::Text))
    }
}

/// After the first sign of a closer.
///
/// ```markdown
/// > | a %%b%% c
///          ^
/// ```
fn close(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'%') {
        tokenizer.consume();
        tokenizer.exit(Name::CommentSequence);
        tokenizer.exit(Name::Comment);
        State::Ok
    } else {
        // A lone sign is body text.
        tokenizer.exit(Name::CommentSequence);
        let len = tokenizer.events.len();
        tokenizer.events[len - 2].name = Name::CommentText;
        tokenizer.events[len - 1].name = Name::CommentText;
        State::Redo(StateName::Comment(Step::Between))
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Open,
    Between,
    Line,
    Indent,
    Text,
    Close,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Comment(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Open => open(tokenizer),
        Step::Between => between(tokenizer),
        Step::Line => line(tokenizer),
        Step::Indent => indent(tokenizer),
        Step::Text => text(tokenizer),
        Step::Close => close(tokenizer),
    }
}
