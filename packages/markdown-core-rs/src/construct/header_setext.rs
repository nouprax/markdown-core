//! Heading (setext) occurs in the [flow][] content type.
//!
//! ## Grammar
//!
//! Heading (setext) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! heading_setext ::= paragraph eol *space_or_tab (1*'-' | 1*'=') *space_or_tab
//!
//! ; See the `prose` construct for the BNF of that part.
//! ```
//!
//! As this construct occurs in flow, like all flow constructs, it must be
//! followed by an eol (line ending) or eof (end of file).
//!
//! See [`prose`][prose] for grammar, notes, and recommendations on
//! that part.
//!
//! In markdown, it is also possible to create headings with a
//! [heading (atx)][header_atx] construct.
//! The benefit of setext headings is that their text can include line endings,
//! and by extensions also hard breaks (e.g., with
//! [hard break (escape)][hard_break_escape]).
//! However, their limit is that they cannot form `<h3>` through `<h6>`
//! headings.
//!
//! [Thematic breaks][thematic_break] formed with dashes and without whitespace
//! could be interpreted as a heading (setext).
//! Which one forms depends on whether there is text directly in fron of the
//! sequence.
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
//! Heading (setext) in markdown relates to the `<h1>` and `<h2>` elements in
//! HTML.
//! See [*§ 4.3.6 The `h1`, `h2`, `h3`, `h4`, `h5`, and `h6` elements* in the
//! HTML spec][html] for more info.
//!
//! ## Recommendation
//!
//! Always use heading (atx), never heading (setext).
//!
//! ## Tokens
//!
//! * [`HeaderSetext`][Name::HeaderSetext]
//! * [`HeaderSetextText`][Name::HeaderSetextText]
//! * [`HeaderSetextUnderline`][Name::HeaderSetextUnderline]
//! * [`HeaderSetextUnderlineSequence`][Name::HeaderSetextUnderlineSequence]
//!
//! ## References
//!
//! * [`setext-underline.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/setext-underline.js)
//! * [*§ 4.3 Setext headings* in `CommonMark`](https://spec.commonmark.org/0.31/#setext-headings)
//!
//! [flow]: crate::construct::flow
//! [prose]: crate::construct::prose
//! [header_atx]: crate::construct::header_atx
//! [thematic_break]: crate::construct::thematic_break
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
use crate::util::{constant::TAB_SIZE, skip};
use alloc::vec;

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Codes(b"\t -=");

/// At start of heading (setext) underline.
///
/// ```markdown
///   | aa
/// > | ==
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if !tokenizer.lax
        && !tokenizer.pierce
        // Require a paragraph before.
        && (!tokenizer.events.is_empty()
            && matches!(tokenizer.events[skip::opt_back(
                &tokenizer.events,
                tokenizer.events.len() - 1,
                &[Name::LineEnd, Name::SpaceOrTab],
            )]
            .name, Name::Content | Name::HeaderSetextUnderline))
    {
        tokenizer.enter(Name::HeaderSetextUnderline);

        if matches!(tokenizer.current, Some(b'\t' | b' ')) {
            tokenizer.attempt(
                State::Next(StateName::HeaderSetext(Step::Before)),
                State::Nok,
            );
            State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
        } else {
            State::Redo(StateName::HeaderSetext(Step::Before))
        }
    } else {
        State::Nok
    }
}

/// After optional whitespace, at `-` or `=`.
///
/// ```markdown
///   | aa
/// > | ==
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'-' | b'=') => {
            tokenizer.enter(Name::HeaderSetextUnderlineSequence);
            State::Redo(StateName::HeaderSetext(Step::Inside {
                marker: tokenizer.current.unwrap(),
            }))
        }
        _ => State::Nok,
    }
}

/// In sequence.
///
/// ```markdown
///   | aa
/// > | ==
///     ^
/// ```
fn inside(tokenizer: &mut Tokenizer, marker: u8) -> State {
    if tokenizer.current == Some(marker) {
        tokenizer.consume();
        State::Next(StateName::HeaderSetext(Step::Inside { marker }))
    } else {
        tokenizer.exit(Name::HeaderSetextUnderlineSequence);

        if matches!(tokenizer.current, Some(b'\t' | b' ')) {
            tokenizer.attempt(
                State::Next(StateName::HeaderSetext(Step::After)),
                State::Nok,
            );
            State::Redo(space_or_tab())
        } else {
            State::Redo(StateName::HeaderSetext(Step::After))
        }
    }
}

/// After sequence, after optional whitespace.
///
/// ```markdown
///   | aa
/// > | ==
///       ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            // Feel free to interrupt.
            tokenizer.interrupt = false;
            tokenizer.add_resolver(ResolveName::HeaderSetext);
            tokenizer.exit(Name::HeaderSetextUnderline);
            State::Ok
        }
        _ => State::Nok,
    }
}

/// Resolve heading (setext).
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    let mut enter = skip::to(&tokenizer.events, 0, &[Name::HeaderSetextUnderline]);

    while enter < tokenizer.events.len() {
        let exit = skip::to(&tokenizer.events, enter + 1, &[Name::HeaderSetextUnderline]);

        // Find paragraph before
        let prose_exit_before = skip::opt_back(
            &tokenizer.events,
            enter - 1,
            &[
                Name::SpaceOrTab,
                Name::ListItemIndent,
                Name::FootnoteDefinitionIndent,
                Name::LineEnd,
                Name::BlockQuotePrefix,
            ],
        );

        // There’s a paragraph before: this is a setext heading.
        if tokenizer.events[prose_exit_before].name == Name::Prose {
            let prose_enter =
                skip::to_back(&tokenizer.events, prose_exit_before - 1, &[Name::Prose]);

            // Change types of Enter:Paragraph, Exit:Paragraph.
            tokenizer.events[prose_enter].name = Name::HeaderSetextText;
            tokenizer.events[prose_exit_before].name = Name::HeaderSetextText;

            // Add Enter:HeadingSetext, Exit:HeadingSetext.
            let mut header_enter = tokenizer.events[prose_enter].clone();
            header_enter.name = Name::HeaderSetext;
            tokenizer.map.add(prose_enter, 0, vec![header_enter]);
            let mut header_exit = tokenizer.events[exit].clone();
            header_exit.name = Name::HeaderSetext;
            tokenizer.map.add(exit + 1, 0, vec![header_exit]);
        } else {
            // There’s a following paragraph, move this underline inside it.
            if exit + 3 < tokenizer.events.len()
                && tokenizer.events[exit + 1].name == Name::LineEnd
                && tokenizer.events[exit + 3].name == Name::Prose
            {
                // Swap type, HeadingSetextUnderline:Enter -> Paragraph:Enter.
                tokenizer.events[enter].name = Name::Prose;
                // Swap type, LineEnding -> Data.
                tokenizer.events[exit + 1].name = Name::Data;
                tokenizer.events[exit + 2].name = Name::Data;
                // Move new data (was line ending) back to include whole line,
                // and link data together.
                tokenizer.events[exit + 1].point = tokenizer.events[enter].point.clone();
                tokenizer.events[exit + 1].link = Some(Link {
                    previous: None,
                    next: Some(exit + 4),
                    content: Content::Text,
                });
                tokenizer.events[exit + 4].link.as_mut().unwrap().previous = Some(exit + 1);
                // Remove *including* HeadingSetextUnderline:Exit, until the line ending.
                tokenizer.map.add(enter + 1, exit - enter, vec![]);
                // Remove old Paragraph:Enter.
                tokenizer.map.add(exit + 3, 1, vec![]);
            } else {
                // Swap type.
                tokenizer.events[enter].name = Name::Prose;
                tokenizer.events[exit].name = Name::Prose;
                // Replace what’s inside the underline (whitespace, sequence).
                tokenizer.map.add(
                    enter + 1,
                    exit - enter - 1,
                    vec![
                        Event {
                            name: Name::Data,
                            kind: Kind::Enter,
                            point: tokenizer.events[enter].point.clone(),
                            link: Some(Link {
                                previous: None,
                                next: None,
                                content: Content::Text,
                            }),
                        },
                        Event {
                            name: Name::Data,
                            kind: Kind::Exit,
                            point: tokenizer.events[exit].point.clone(),
                            link: None,
                        },
                    ],
                );
            }
        }

        enter = skip::to(&tokenizer.events, exit + 1, &[Name::HeaderSetextUnderline]);
    }

    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    Inside { marker: u8 },
    After,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::HeaderSetext(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::Inside { marker } => inside(tokenizer, marker),
        Step::After => after(tokenizer),
    }
}
