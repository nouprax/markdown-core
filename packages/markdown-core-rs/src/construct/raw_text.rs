//! Raw (text) occurs in the [text][] content type.
//! It forms code (text).
//!
//! ## Grammar
//!
//! Raw (text) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: the number of markers in the closing sequence must be equal
//! ; to the number of markers in the opening sequence.
//! raw_text ::= sequence 1*byte sequence
//!
//! ; Restriction: not preceded or followed by the same marker.
//! sequence ::= 1*'`'
//! ```
//!
//! The above grammar shows that it is not possible to create empty raw (text).
//! It is possible to include the sequence marker (grave accent) in raw
//! (text), by wrapping it in bigger or smaller sequences:
//!
//! ```markdown
//! Include more: `a``b` or include less: ``a`b``.
//! ```
//!
//! It is also possible to include just one marker:
//!
//! ```markdown
//! Include just one: `` ` ``.
//! ```
//!
//! Sequences are “gready”, in that they cannot be preceded or followed by
//! more markers.
//! To illustrate:
//!
//! ```markdown
//! Not code: ``x`.
//!
//! Not code: `x``.
//!
//! Escapes work, this is code: \``x`.
//!
//! Escapes work, this is code: `x`\`.
//! ```
//!
//! Yields:
//!
//! ```html
//! <p>Not code: ``x`.</p>
//! <p>Not code: `x``.</p>
//! <p>Escapes work, this is code: `<code>x</code>.</p>
//! <p>Escapes work, this is code: <code>x</code>`.</p>
//! ```
//!
//! That is because, when turning markdown into HTML, the first and last space,
//! if both exist and there is also a non-space in the code, are removed.
//! Line endings, at that stage, are considered as spaces.
//!
//! In markdown, it is possible to create code with the
//! [raw (flow)][raw_flow] (or [code (indented)][code_indented]) constructs
//! in the [flow][] content type.
//!
//! ## HTML
//!
//! Code (text) relates to the `<code>` element in HTML.
//! See [*§ 4.5.15 The `code` element*][html_code] in the HTML spec for more
//! info.
//!
//! When turning markdown into HTML, each line ending in raw (text) is turned
//! into a space.
//!
//! ## Tokens
//!
//! * [`CodeText`][Name::CodeText]
//! * [`CodeTextData`][Name::CodeTextData]
//! * [`CodeTextSequence`][Name::CodeTextSequence]
//! * [`LineEnding`][Name::LineEnding]
//!
//! ## References
//!
//! * [`code-text.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/code-text.js)
//! * [*§ 6.1 Code spans* in `CommonMark`](https://spec.commonmark.org/0.31/#code-spans)
//!
//! [flow]: crate::construct::flow
//! [text]: crate::construct::text
//! [code_indented]: crate::construct::code_indented
//! [raw_flow]: crate::construct::raw_flow
//! [html_code]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-code-element

use crate::choice::Starts;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b"`");

/// Start of raw (text).
///
/// ```markdown
/// > | `a`
///     ^
/// > | \`a`
///      ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    // Code (text):
    if tokenizer.current == Some(b'`')
        // Not the same marker (except when escaped).
        && (tokenizer.previous != tokenizer.current
            || (!tokenizer.events.is_empty()
                && tokenizer.events[tokenizer.events.len() - 1].name == Name::CharacterEscape))
    {
        tokenizer.enter(Name::CodeText);
        tokenizer.enter(Name::CodeTextSequence);
        State::Redo(StateName::RawText(Step::SequenceOpen { size: 0 }))
    } else {
        State::Nok
    }
}

/// In opening sequence.
///
/// ```markdown
/// > | `a`
///     ^
/// ```
fn sequence_open(tokenizer: &mut Tokenizer, size: usize) -> State {
    if tokenizer.current == Some(b'`') {
        tokenizer.consume();
        State::Next(StateName::RawText(Step::SequenceOpen { size: size + 1 }))
    } else {
        tokenizer.exit(Name::CodeTextSequence);
        State::Redo(StateName::RawText(Step::Between { size }))
    }
}

/// Between something and something else.
///
/// ```markdown
/// > | `a`
///      ^^
/// ```
fn between(tokenizer: &mut Tokenizer, size: usize) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            State::Next(StateName::RawText(Step::Between { size }))
        }
        _ => {
            if tokenizer.current == Some(b'`') {
                tokenizer.enter(Name::CodeTextSequence);
                State::Redo(StateName::RawText(Step::SequenceClose { size, close: 0 }))
            } else {
                tokenizer.enter(Name::CodeTextData);
                State::Redo(StateName::RawText(Step::Data { size }))
            }
        }
    }
}

/// In data.
///
/// ```markdown
/// > | `a`
///      ^
/// ```
fn data(tokenizer: &mut Tokenizer, size: usize) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) || tokenizer.current == Some(b'`') {
        tokenizer.exit(Name::CodeTextData);
        State::Redo(StateName::RawText(Step::Between { size }))
    } else {
        tokenizer.consume();
        State::Next(StateName::RawText(Step::Data { size }))
    }
}

/// In closing sequence.
///
/// ```markdown
/// > | `a`
///       ^
/// ```
fn sequence_close(tokenizer: &mut Tokenizer, size: usize, close: usize) -> State {
    if tokenizer.current == Some(b'`') {
        tokenizer.consume();
        State::Next(StateName::RawText(Step::SequenceClose {
            size,
            close: close + 1,
        }))
    } else {
        tokenizer.exit(Name::CodeTextSequence);
        if size == close {
            tokenizer.exit(Name::CodeText);
            State::Ok
        } else {
            // More or less accents: mark as data.
            let len = tokenizer.events.len();
            tokenizer.events[len - 2].name = Name::CodeTextData;
            tokenizer.events[len - 1].name = Name::CodeTextData;
            State::Redo(StateName::RawText(Step::Between { size }))
        }
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    SequenceOpen { size: usize },
    Between { size: usize },
    Data { size: usize },
    SequenceClose { size: usize, close: usize },
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::RawText(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::SequenceOpen { size } => sequence_open(tokenizer, size),
        Step::Between { size } => between(tokenizer, size),
        Step::Data { size } => data(tokenizer, size),
        Step::SequenceClose { size, close } => sequence_close(tokenizer, size, close),
    }
}
