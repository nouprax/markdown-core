//! Paragraph occurs in the [content][] content type.
//!
//! ## Grammar
//!
//! Paragraph forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: lines cannot start other flow constructs.
//! ; Restriction: lines cannot be blank.
//! paragraph ::= 1*line *(eol 1*line)
//! ```
//!
//! This construct must be followed by an eol (line ending) or eof (end of
//! file), like flow constructs.
//!
//! Paragraphs can contain line endings and whitespace, but they are not
//! allowed to contain blank lines, or to be blank themselves.
//!
//! The paragraph is interpreted as the [text][] content type.
//! That means that [autolinks][autolink], [code (text)][raw_text], etc are
//! allowed.
//!
//! ## HTML
//!
//! Paragraphs in markdown relate to the `<p>` element in HTML.
//! See [*§ 4.4.1 The `p` element* in the HTML spec][html] for more info.
//!
//! ## Tokens
//!
//! * [`Paragraph`][Name::Paragraph]
//!
//! ## References
//!
//! * [`content.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/content.js)
//! * [*§ 4.8 Paragraphs* in `CommonMark`](https://spec.commonmark.org/0.31/#paragraphs)
//!
//! [content]: crate::construct::content
//! [text]: crate::construct::text
//! [autolink]: crate::construct::autolink
//! [raw_text]: crate::construct::raw_text
//! [html]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-p-element

use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::subtokenize::link;
use crate::tokenizer::Tokenizer;

/// Paragraph start.
///
/// ```markdown
/// > | abc
///     ^
///   | def
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    debug_assert!(tokenizer.current.is_some());
    tokenizer.enter(Name::Paragraph);
    State::Redo(StateName::Paragraph(Step::LineStart { connect: false }))
}

/// Start of a line in a paragraph.
///
/// ```markdown
/// > | abc
///     ^
/// > | def
///     ^
/// ```
fn line_start(tokenizer: &mut Tokenizer, connect: bool) -> State {
    debug_assert!(tokenizer.current.is_some());
    tokenizer.enter_link(
        Name::Data,
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

    State::Redo(StateName::Paragraph(Step::Inside))
}

/// In paragraph.
///
/// ```markdown
/// > | abc
///     ^^^
/// ```
fn inside(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => {
            tokenizer.exit(Name::Data);
            tokenizer.exit(Name::Paragraph);
            State::Ok
        }
        Some(b'\n') => {
            tokenizer.consume();
            tokenizer.exit(Name::Data);
            State::Next(StateName::Paragraph(Step::LineStart { connect: true }))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::Paragraph(Step::Inside))
        }
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    LineStart { connect: bool },
    Inside,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Paragraph(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::LineStart { connect } => line_start(tokenizer, connect),
        Step::Inside => inside(tokenizer),
    }
}
