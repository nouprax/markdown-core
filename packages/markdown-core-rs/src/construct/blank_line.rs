//! Blank lines occur in the [flow][] content type.
//!
//! ## Grammar
//!
//! Blank lines form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! blank_line ::= *space_or_tab
//! ```
//!
//! As this construct occurs in flow, like all flow constructs, it must be
//! followed by an eol (line ending) or eof (end of file).
//!
//! Blank lines are sometimes needed, such as to differentiate a [paragraph][paragraph]
//! from a definition.
//! In several cases, blank lines are not needed between flow constructs,
//! such as between two [heading (atx)][heading_atx]s.
//! Sometimes, whether blank lines are present, changes the behavior of how
//! HTML is rendered, such as whether blank lines are present inside or between
//! [list items][list_item].
//! More than one blank line is never needed in `CommonMark`.
//!
//! Because blank lines can be empty (line endings are not considered part of
//! it), and events cannot be empty, blank lines are not present as an event.
//!
//! ## HTML
//!
//! Blank lines do not relate to an element in HTML, except for the role they
//! play when inside or between [list items][list_item].
//!
//! ## Recommendation
//!
//! It is recommended to always use a blank line between every flow construct,
//! to use blank lines (consistently) between list items as desired, and to
//! never use more than one blank line.
//!
//! ## Tokens
//!
//! * [`SpaceOrTab`][crate::event::Name::SpaceOrTab]
//!
//! ## References
//!
//! * [`blank-line.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/blank-line.js)
//! * [*§ 4.9 Blank lines* in `CommonMark`](https://spec.commonmark.org/0.31/#blank-lines)
//!
//! [header_atx]: crate::construct::header_atx
//! [list_item]: crate::construct::list_item
//! [prose]: crate::construct::prose
//! [flow]: crate::construct::flow

use crate::construct::partial_space_or_tab::space_or_tab;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Start of blank line.
///
/// > 👉 **Note**: `␠` represents a space character.
///
/// ```markdown
/// > | ␠␠␊
///     ^
/// > | ␊
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(State::Next(StateName::BlankLine(Step::After)), State::Nok);
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::BlankLine(Step::After))
    }
}

/// At eof/eol, after optional whitespace.
///
/// ```markdown
/// > | ␠␠␊
///       ^
/// > | ␊
///     ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Ok,
        _ => State::Nok,
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    After,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::BlankLine(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::After => after(tokenizer),
    }
}
