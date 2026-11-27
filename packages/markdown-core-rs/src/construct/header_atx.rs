//! Heading (atx) occurs in the [flow][] content type.
//!
//! ## Grammar
//!
//! Heading (atx) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! heading_atx ::= 1*6'#' [ 1*space_or_tab line [ 1*space_or_tab 1*'#' ] ] *space_or_tab
//! ```
//!
//! As this construct occurs in flow, like all flow constructs, it must be
//! followed by an eol (line ending) or eof (end of file).
//!
//! `CommonMark` introduced the requirement on whitespace existing after the
//! opening sequence and before text.
//! In older markdown versions, this was not required, and headings would form
//! without it.
//!
//! In markdown, it is also possible to create headings with a
//! [heading (setext)][header_setext] construct.
//! The benefit of setext headings is that their text can include line endings,
//! and by extensions also hard breaks (e.g., with
//! [hard break (escape)][hard_break_escape]).
//! However, their limit is that they cannot form `<h3>` through `<h6>`
//! headings.
//!
//! > 🏛 **Background**: the word *setext* originates from a small markup
//! > language by Ian Feldman from 1991.
//! > See [*§ Setext* on Wikipedia][wiki_setext] for more info.
//! > The word *atx* originates from a tiny markup language by Aaron Swartz
//! > from 2002.
//! > See [*§ atx, the true structured text format* on `aaronsw.com`][atx] for
//! > more info.
//!
//! ## HTML
//!
//! Headings in markdown relate to the `<h1>` through `<h6>` elements in HTML.
//! See [*§ 4.3.6 The `h1`, `h2`, `h3`, `h4`, `h5`, and `h6` elements* in the
//! HTML spec][html] for more info.
//!
//! ## Recommendation
//!
//! Always use heading (atx), never heading (setext).
//!
//! ## Tokens
//!
//! * [`HeaderAtx`][Name::HeaderAtx]
//! * [`HeaderAtxSequence`][Name::HeaderAtxSequence]
//! * [`HeaderAtxText`][Name::HeaderAtxText]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! ## References
//!
//! * [`heading-atx.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/heading-atx.js)
//! * [*§ 4.2 ATX headings* in `CommonMark`](https://spec.commonmark.org/0.31/#atx-headings)
//!
//! [flow]: crate::construct::flow
//! [header_setext]: crate::construct::header_setext
//! [hard_break_escape]: crate::construct::hard_break_escape
//! [html]: https://html.spec.whatwg.org/multipage/sections.html#the-h1,-h2,-h3,-h4,-h5,-and-h6-elements
//! [wiki_setext]: https://en.wikipedia.org/wiki/Setext
//! [atx]: http://www.aaronsw.com/2002/atx/

use crate::choice::Starts;
use crate::construct::partial_space_or_tab::{space_or_tab, space_or_tab_min_max};
use crate::event::{Content, Event, Kind, Link, Name};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::Tokenizer;
use crate::util::constant::{HEADER_ATX_OPEN_FENCE_SIZE_MAX, TAB_SIZE};
use alloc::vec;

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Codes(b"\t #");

/// Start of a heading (atx).
///
/// ```markdown
/// > | ## aa
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::HeaderAtx);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(State::Next(StateName::HeaderAtx(Step::Before)), State::Nok);
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::HeaderAtx(Step::Before))
    }
}

/// After optional whitespace, at `#`.
///
/// ```markdown
/// > | ## aa
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    if Some(b'#') == tokenizer.current {
        tokenizer.enter(Name::HeaderAtxSequence);
        State::Redo(StateName::HeaderAtx(Step::SequenceOpen { size: 0 }))
    } else {
        State::Nok
    }
}

/// In opening sequence.
///
/// ```markdown
/// > | ## aa
///     ^
/// ```
fn sequence_open(tokenizer: &mut Tokenizer, size: usize) -> State {
    if tokenizer.current == Some(b'#') && size < HEADER_ATX_OPEN_FENCE_SIZE_MAX {
        tokenizer.consume();
        State::Next(StateName::HeaderAtx(Step::SequenceOpen { size: size + 1 }))
    }
    // Always at least one `#`.
    else if matches!(tokenizer.current, None | Some(b'\t' | b'\n' | b' ')) {
        tokenizer.exit(Name::HeaderAtxSequence);
        State::Redo(StateName::HeaderAtx(Step::AtBreak))
    } else {
        State::Nok
    }
}

/// After something, before something else.
///
/// ```markdown
/// > | ## aa
///       ^
/// ```
fn at_break(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::HeaderAtx);
            tokenizer.add_resolver(ResolveName::HeaderAtx);
            // Feel free to interrupt.
            tokenizer.interrupt = false;
            State::Ok
        }
        Some(b'\t' | b' ') => {
            tokenizer.attempt(State::Next(StateName::HeaderAtx(Step::AtBreak)), State::Nok);
            State::Redo(space_or_tab())
        }
        Some(b'#') => {
            tokenizer.enter(Name::HeaderAtxSequence);
            State::Redo(StateName::HeaderAtx(Step::SequenceFurther))
        }
        Some(_) => {
            tokenizer.enter_link(
                Name::Data,
                Link {
                    previous: None,
                    next: None,
                    content: Content::Text,
                },
            );
            State::Redo(StateName::HeaderAtx(Step::Data))
        }
    }
}

/// In further sequence (after whitespace).
///
/// Could be normal “visible” hashes in the heading or a final sequence.
///
/// ```markdown
/// > | ## aa ##
///           ^
/// ```
fn sequence_further(tokenizer: &mut Tokenizer) -> State {
    if let Some(b'#') = tokenizer.current {
        tokenizer.consume();
        State::Next(StateName::HeaderAtx(Step::SequenceFurther))
    } else {
        tokenizer.exit(Name::HeaderAtxSequence);
        State::Redo(StateName::HeaderAtx(Step::AtBreak))
    }
}

/// In text.
///
/// ```markdown
/// > | ## aa
///        ^
/// ```
fn data(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // Note: `#` for closing sequence must be preceded by whitespace, otherwise it’s just text.
        None | Some(b'\t' | b'\n' | b' ') => {
            tokenizer.exit(Name::Data);
            State::Redo(StateName::HeaderAtx(Step::AtBreak))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::HeaderAtx(Step::Data))
        }
    }
}

/// Resolve heading (atx).
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    let mut index = 0;
    let mut header_inside = false;
    let mut data_start: Option<usize> = None;
    let mut data_end: Option<usize> = None;

    while index < tokenizer.events.len() {
        let event = &tokenizer.events[index];

        if event.name == Name::HeaderAtx {
            if event.kind == Kind::Enter {
                header_inside = true;
            } else {
                if let Some(start) = data_start {
                    // If `start` is some, `end` is too.
                    let end = data_end.unwrap();

                    tokenizer.map.add(
                        start,
                        0,
                        vec![Event {
                            kind: Kind::Enter,
                            name: Name::HeaderAtxText,
                            point: tokenizer.events[start].point.clone(),
                            link: None,
                        }],
                    );

                    // Remove everything between the start and the end.
                    tokenizer.map.add(start + 1, end - start - 1, vec![]);

                    tokenizer.map.add(
                        end + 1,
                        0,
                        vec![Event {
                            kind: Kind::Exit,
                            name: Name::HeaderAtxText,
                            point: tokenizer.events[end].point.clone(),
                            link: None,
                        }],
                    );
                }

                header_inside = false;
                data_start = None;
                data_end = None;
            }
        } else if header_inside && event.name == Name::Data {
            if event.kind == Kind::Enter {
                if data_start.is_none() {
                    data_start = Some(index);
                }
            } else {
                data_end = Some(index);
            }
        }

        index += 1;
    }

    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    SequenceOpen { size: usize },
    AtBreak,
    SequenceFurther,
    Data,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::HeaderAtx(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::SequenceOpen { size } => sequence_open(tokenizer, size),
        Step::AtBreak => at_break(tokenizer),
        Step::SequenceFurther => sequence_further(tokenizer),
        Step::Data => data(tokenizer),
    }
}
