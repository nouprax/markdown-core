//! Thematic break occurs in the [flow][] content type.
//!
//! ## Grammar
//!
//! Thematic break forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: all markers must be identical.
//! ; Restriction: at least 3 markers must be used.
//! thematic_break ::= *space_or_tab 1*(1*marker *space_or_tab)
//!
//! marker ::= '*' | '-' | '_'
//! ```
//!
//! As this construct occurs in flow, like all flow constructs, it must be
//! followed by an eol (line ending) or eof (end of file).
//!
//! ## HTML
//!
//! Thematic breaks in markdown typically relate to the HTML element `<hr>`.
//! See [*§ 4.4.2 The `hr` element* in the HTML spec][html] for more info.
//!
//! ## Recommendation
//!
//! It is recommended to use exactly three asterisks without whitespace when
//! writing markdown.
//! As using more than three markers has no effect other than wasting space,
//! it is recommended to use exactly three markers.
//! Thematic breaks formed with asterisks or dashes can interfere with
//! [list][list-item]s if there is whitespace between them: `* * *` and `- - -`.
//! For these reasons, it is recommend to not use spaces or tabs between the
//! markers.
//! Thematic breaks formed with dashes (without whitespace) can also form
//! [heading (setext)][header_setext].
//! As dashes and underscores frequently occur in natural language and URLs, it
//! is recommended to use asterisks for thematic breaks to distinguish from
//! such use.
//! Because asterisks can be used to form the most markdown constructs, using
//! them has the added benefit of making it easier to gloss over markdown: you
//! can look for asterisks to find syntax while not worrying about other
//! characters.
//!
//! ## Tokens
//!
//! * [`ThematicBreak`][Name::ThematicBreak]
//! * [`ThematicBreakSequence`][Name::ThematicBreakSequence]
//!
//! ## References
//!
//! * [`thematic-break.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/thematic-break.js)
//! * [*§ 4.1 Thematic breaks* in `CommonMark`](https://spec.commonmark.org/0.31/#thematic-breaks)
//!
//! [flow]: crate::construct::flow
//! [header_setext]: crate::construct::header_setext
//! [list-item]: crate::construct::list_item
//! [html]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-hr-element

use crate::choice::Starts;
use crate::construct::partial_space_or_tab::{space_or_tab, space_or_tab_min_max};
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::constant::{TAB_SIZE, THEMATIC_BREAK_MARKER_COUNT_MIN};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Codes(b"\t *-_");

/// Start of thematic break.
///
/// ```markdown
/// > | ***
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::ThematicBreak);

    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::ThematicBreak(Step::Before)),
            State::Nok,
        );
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::ThematicBreak(Step::Before))
    }
}

/// After optional whitespace, at marker.
///
/// ```markdown
/// > | ***
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'*' | b'-' | b'_') => State::Redo(StateName::ThematicBreak(Step::AtBreak {
            marker: tokenizer.current.unwrap(),
            size: 0,
        })),
        _ => State::Nok,
    }
}

/// After something, before something else.
///
/// ```markdown
/// > | ***
///     ^
/// ```
fn at_break(tokenizer: &mut Tokenizer, marker: u8, size: usize) -> State {
    if tokenizer.current == Some(marker) {
        tokenizer.enter(Name::ThematicBreakSequence);
        State::Redo(StateName::ThematicBreak(Step::Sequence { marker, size }))
    } else if size >= THEMATIC_BREAK_MARKER_COUNT_MIN
        && matches!(tokenizer.current, None | Some(b'\n'))
    {
        tokenizer.exit(Name::ThematicBreak);
        // Feel free to interrupt.
        tokenizer.interrupt = false;
        State::Ok
    } else {
        State::Nok
    }
}

/// In sequence.
///
/// ```markdown
/// > | ***
///     ^
/// ```
fn sequence(tokenizer: &mut Tokenizer, marker: u8, size: usize) -> State {
    if tokenizer.current == Some(marker) {
        tokenizer.consume();
        State::Next(StateName::ThematicBreak(Step::Sequence {
            marker,
            size: size + 1,
        }))
    } else if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.exit(Name::ThematicBreakSequence);
        tokenizer.attempt(
            State::Next(StateName::ThematicBreak(Step::AtBreak { marker, size })),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        tokenizer.exit(Name::ThematicBreakSequence);
        State::Redo(StateName::ThematicBreak(Step::AtBreak { marker, size }))
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    Sequence { marker: u8, size: usize },
    AtBreak { marker: u8, size: usize },
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::ThematicBreak(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::Sequence { marker, size } => sequence(tokenizer, marker, size),
        Step::AtBreak { marker, size } => at_break(tokenizer, marker, size),
    }
}
