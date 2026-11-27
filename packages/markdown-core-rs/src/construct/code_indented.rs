//! Code (indented) occurs in the [flow][] content type.
//!
//! ## Grammar
//!
//! Code (indented) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! code_indented ::= filled_line *( eol *( blank_line eol ) filled_line )
//!
//! ; Restriction: at least one `line` byte must be `text`.
//! filled_line ::= 4(space_or_tab) *line
//! blank_line ::= *space_or_tab
//! ```
//!
//! As this construct occurs in flow, like all flow constructs, it must be
//! followed by an eol (line ending) or eof (end of file).
//!
//! In markdown, it is also possible to use [code (text)][raw_text] in the
//! [text][] content type.
//! It is also possible to create code with the [code (fenced)][raw_flow]
//! construct.
//!
//! ## HTML
//!
//! Code (indented) relates to both the `<pre>` and the `<code>` elements in
//! HTML.
//! See [*§ 4.4.3 The `pre` element*][html_pre] and the [*§ 4.5.15 The `code`
//! element*][html_code] in the HTML spec for more info.
//!
//! ## Recommendation
//!
//! It is recommended to use code (fenced) instead of code (indented).
//! Code (fenced) is more explicit, similar to code (text), and has support
//! for specifying the programming language.
//!
//! ## Tokens
//!
//! * [`CodeIndented`][Name::CodeIndented]
//! * [`CodeFlowChunk`][Name::CodeFlowChunk]
//! * [`LineEnding`][Name::LineEnding]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! ## References
//!
//! * [`code-indented.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/code-indented.js)
//! * [*§ 4.4 Indented code blocks* in `CommonMark`](https://spec.commonmark.org/0.31/#indented-code-blocks)
//!
//! [flow]: crate::construct::flow
//! [text]: crate::construct::text
//! [raw_flow]: crate::construct::raw_flow
//! [raw_text]: crate::construct::raw_text
//! [html_code]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-code-element
//! [html_pre]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-pre-element

use crate::choice::Starts;
use crate::construct::partial_space_or_tab::{space_or_tab, space_or_tab_min_max};
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::constant::TAB_SIZE;

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b"\t ");

/// Start of code (indented).
///
/// > **Parsing note**: it is not needed to check if this first line is a
/// > filled line (that it has a non-whitespace character), because blank lines
/// > are parsed already, so we never run into that.
///
/// ```markdown
/// > |     aaa
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    // Do not interrupt paragraphs.
    if !tokenizer.interrupt && matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.enter(Name::CodeIndented);
        tokenizer.attempt(
            State::Next(StateName::CodeIndented(Step::AtBreak)),
            State::Nok,
        );
        State::Retry(space_or_tab_min_max(TAB_SIZE, TAB_SIZE))
    } else {
        State::Nok
    }
}

/// At a break.
///
/// ```markdown
/// > |     aaa
///         ^  ^
/// ```
fn at_break(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Retry(StateName::CodeIndented(Step::After)),
        Some(b'\n') => {
            tokenizer.attempt(
                State::Next(StateName::CodeIndented(Step::AtBreak)),
                State::Next(StateName::CodeIndented(Step::After)),
            );
            State::Retry(StateName::CodeIndented(Step::FurtherStart))
        }
        _ => {
            tokenizer.enter(Name::CodeFlowChunk);
            State::Retry(StateName::CodeIndented(Step::Inside))
        }
    }
}

/// In code content.
///
/// ```markdown
/// > |     aaa
///         ^^^^
/// ```
fn inside(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::CodeFlowChunk);
            State::Retry(StateName::CodeIndented(Step::AtBreak))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::CodeIndented(Step::Inside))
        }
    }
}

/// After indented code.
///
/// ```markdown
/// > |     aaa
///            ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::CodeIndented);
    // Feel free to interrupt.
    tokenizer.interrupt = false;
    State::Ok
}

/// At eol, trying to parse another indent.
///
/// ```markdown
/// > |     aaa
///            ^
///   |     bbb
/// ```
fn further_start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.lazy || tokenizer.pierce {
        return State::Nok;
    }

    if tokenizer.current == Some(b'\n') {
        tokenizer.enter(Name::LineEnding);
        tokenizer.consume();
        tokenizer.exit(Name::LineEnding);
        State::Next(StateName::CodeIndented(Step::FurtherStart))
    } else {
        tokenizer.attempt(
            State::Ok,
            State::Next(StateName::CodeIndented(Step::FurtherBegin)),
        );
        State::Retry(space_or_tab_min_max(TAB_SIZE, TAB_SIZE))
    }
}

/// At the beginning of a line that is not indented enough.
///
/// ```markdown
///   |     aaa
/// > |   bbb
///     ^
/// ```
fn further_begin(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::CodeIndented(Step::FurtherAfter)),
            State::Nok,
        );
        State::Retry(space_or_tab())
    } else {
        State::Nok
    }
}

/// After whitespace, not indented enough.
///
/// ```markdown
///   |     aaa
/// > |   bbb
///       ^
/// ```
fn further_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\n') => State::Retry(StateName::CodeIndented(Step::FurtherStart)),
        _ => State::Nok,
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    AtBreak,
    After,
    FurtherStart,
    Inside,
    FurtherBegin,
    FurtherAfter,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::CodeIndented(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::AtBreak => at_break(tokenizer),
        Step::After => after(tokenizer),
        Step::FurtherStart => further_start(tokenizer),
        Step::Inside => inside(tokenizer),
        Step::FurtherBegin => further_begin(tokenizer),
        Step::FurtherAfter => further_after(tokenizer),
    }
}
