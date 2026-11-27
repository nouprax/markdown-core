//! Raw (text) occurs in the [text][] content type.
//! It forms code (text) and math (text).
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
//! sequence ::= 1*'`' | 1*'$'
//! ```
//!
//! The above grammar shows that it is not possible to create empty raw (text).
//! It is possible to include the sequence marker (grave accent for code,
//! dollar for math) in raw (text), by wrapping it in bigger or smaller
//! sequences:
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
//! In markdown, it is possible to create code or math with the
//! [raw (flow)][raw_flow] (or [code (indented)][code_indented]) constructs
//! in the [flow][] content type.
//!
//! ## HTML
//!
//! Code (text) relates to the `<code>` element in HTML.
//! See [*§ 4.5.15 The `code` element*][html_code] in the HTML spec for more
//! info.
//!
//! Math (text) does not relate to HTML elements.
//! `MathML`, which is sort of like SVG but for math, exists but it doesn’t work
//! well and isn’t widely supported.
//! Instead, it is recommended to use client side JavaScript with something like
//! `KaTeX` or `MathJax` to process the math
//! For that, the math is compiled as a `<code>` element with two classes:
//! `language-math` and `math-inline`.
//! Client side JavaScript can look for these classes to process them further.
//!
//! When turning markdown into HTML, each line ending in raw (text) is turned
//! into a space.
//!
//! ## Recommendations
//!
//! When authoring markdown with math, keep in mind that math doesn’t work in
//! most places.
//! Notably, GitHub currently has a really weird crappy client-side regex-based
//! thing.
//! But on your own (math-heavy?) site it can be great!
//!
//! ## Tokens
//!
//! * [`CodeText`][Name::CodeText]
//! * [`CodeTextData`][Name::CodeTextData]
//! * [`CodeTextSequence`][Name::CodeTextSequence]
//! * [`MathText`][Name::MathText]
//! * [`MathTextData`][Name::MathTextData]
//! * [`MathTextSequence`][Name::MathTextSequence]
//! * [`LineEnding`][Name::LineEnding]
//!
//! ## References
//!
//! * [`code-text.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/code-text.js)
//! * [`micromark-extension-math`](https://github.com/micromark/micromark-extension-math)
//! * [*§ 6.1 Code spans* in `CommonMark`](https://spec.commonmark.org/0.31/#code-spans)
//!
//! > 👉 **Note**: math is not specified anywhere.
//!
//! [flow]: crate::construct::flow
//! [text]: crate::construct::text
//! [code_indented]: crate::construct::code_indented
//! [raw_flow]: crate::construct::raw_flow
//! [html_code]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-code-element

use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

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
    if (tokenizer.current == Some(b'`')
        // Math (text):
        || tokenizer.current == Some(b'$'))
        // Not the same marker (except when escaped).
        && (tokenizer.previous != tokenizer.current
            || (!tokenizer.events.is_empty()
                && tokenizer.events[tokenizer.events.len() - 1].name == Name::CharacterEscape))
    {
        let marker = tokenizer.current.unwrap();
        let (whole, sequence, _) = names(marker);
        tokenizer.enter(whole);
        tokenizer.enter(sequence);
        State::Retry(StateName::RawText(Step::SequenceOpen { marker, size: 0 }))
    } else {
        State::Nok
    }
}

/// The token names of raw text with `marker`: the whole, its sequences and
/// its data.
fn names(marker: u8) -> (Name, Name, Name) {
    if marker == b'`' {
        (Name::CodeText, Name::CodeTextSequence, Name::CodeTextData)
    } else {
        (Name::MathText, Name::MathTextSequence, Name::MathTextData)
    }
}

/// In opening sequence.
///
/// ```markdown
/// > | `a`
///     ^
/// ```
fn sequence_open(tokenizer: &mut Tokenizer, marker: u8, size: usize) -> State {
    if tokenizer.current == Some(marker) {
        tokenizer.consume();
        State::Next(StateName::RawText(Step::SequenceOpen {
            marker,
            size: size + 1,
        }))
    } else {
        tokenizer.exit(names(marker).1);
        State::Retry(StateName::RawText(Step::Between { marker, size }))
    }
}

/// Between something and something else.
///
/// ```markdown
/// > | `a`
///      ^^
/// ```
fn between(tokenizer: &mut Tokenizer, marker: u8, size: usize) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            State::Next(StateName::RawText(Step::Between { marker, size }))
        }
        _ => {
            if tokenizer.current == Some(marker) {
                tokenizer.enter(names(marker).1);
                State::Retry(StateName::RawText(Step::SequenceClose {
                    marker,
                    size,
                    close: 0,
                }))
            } else {
                tokenizer.enter(names(marker).2);
                State::Retry(StateName::RawText(Step::Data { marker, size }))
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
fn data(tokenizer: &mut Tokenizer, marker: u8, size: usize) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) || tokenizer.current == Some(marker) {
        tokenizer.exit(names(marker).2);
        State::Retry(StateName::RawText(Step::Between { marker, size }))
    } else {
        tokenizer.consume();
        State::Next(StateName::RawText(Step::Data { marker, size }))
    }
}

/// In closing sequence.
///
/// ```markdown
/// > | `a`
///       ^
/// ```
fn sequence_close(tokenizer: &mut Tokenizer, marker: u8, size: usize, close: usize) -> State {
    let (whole, sequence, data) = names(marker);
    if tokenizer.current == Some(marker) {
        tokenizer.consume();
        State::Next(StateName::RawText(Step::SequenceClose {
            marker,
            size,
            close: close + 1,
        }))
    } else {
        tokenizer.exit(sequence);
        if size == close {
            tokenizer.exit(whole);
            State::Ok
        } else {
            // More or less accents: mark as data.
            let len = tokenizer.events.len();
            tokenizer.events[len - 2].name = data;
            tokenizer.events[len - 1].name = data;
            State::Retry(StateName::RawText(Step::Between { marker, size }))
        }
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    SequenceOpen {
        marker: u8,
        size: usize,
    },
    Between {
        marker: u8,
        size: usize,
    },
    Data {
        marker: u8,
        size: usize,
    },
    SequenceClose {
        marker: u8,
        size: usize,
        close: usize,
    },
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
        Step::SequenceOpen { marker, size } => sequence_open(tokenizer, marker, size),
        Step::Between { marker, size } => between(tokenizer, marker, size),
        Step::Data { marker, size } => data(tokenizer, marker, size),
        Step::SequenceClose {
            marker,
            size,
            close,
        } => sequence_close(tokenizer, marker, size, close),
    }
}
