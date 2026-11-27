//! Content occurs in the [flow][] content type.
//!
//! Content contains zero or more [definition]s, followed by zero
//! or one [paragraph][paragraph].
//!
//! The constructs found in flow are:
//!
//! * [Definition][crate::construct::definition]
//! * [Paragraph][crate::construct::paragraph]
//!
//! ## Tokens
//!
//! * [`Content`][Name::Content]
//!
//! > 👉 **Note**: while parsing, [`Content`][Name::Content]
//! > is used, which is later compiled away.
//!
//! ## References
//!
//! * [`content.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/content.js)
//!
//! [flow]: crate::construct::flow
//! [definition]: crate::construct::definition
//! [prose]: crate::construct::prose

use crate::construct::{definition, paragraph};
use crate::event::{Content, Kind, Link, Name};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::{subtokenize, Subresult};
use crate::tokenizer::Tokenizer;
use alloc::vec;

/// Before a content chunk.
///
/// ```markdown
/// > | abc
///     ^
/// ```
fn chunk_start(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => unreachable!("unexpected eol/eof"),
        _ => {
            tokenizer.enter_link(
                Name::Content,
                Link {
                    previous: None,
                    next: None,
                    content: Content::Content,
                },
            );
            State::Redo(StateName::Content(Step::ChunkInside))
        }
    }
}

/// In a content chunk.
///
/// ```markdown
/// > | abc
///     ^^^
/// ```
fn chunk_inside(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::Content);
            tokenizer.add_resolver_before(ResolveName::Content);
            // You’d be interrupting.
            tokenizer.interrupt = true;
            State::Ok
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::Content(Step::ChunkInside))
        }
    }
}

/// Before a definition.
///
/// ```markdown
/// > | [a]: b
///     ^
/// ```
fn definition_before(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Content(Step::DefinitionAfter)),
        State::Next(StateName::Paragraph(paragraph::Step::Start)),
    );
    State::Redo(StateName::Definition(definition::Step::Start))
}

/// After a definition.
///
/// ```markdown
/// > | [a]: b
///           ^
///   | c
/// ```
fn definition_after(tokenizer: &mut Tokenizer) -> State {
    debug_assert!(matches!(tokenizer.current, None | Some(b'\n')));
    if tokenizer.current.is_none() {
        State::Ok
    } else {
        tokenizer.enter(Name::LineEnd);
        tokenizer.consume();
        tokenizer.exit(Name::LineEnd);
        State::Next(StateName::Content(Step::DefinitionBefore))
    }
}

/// Merge `Content` chunks, which currently span a single line, into actual
/// `Content`s that span multiple lines.
pub fn resolve(tokenizer: &mut Tokenizer) -> Subresult {
    let mut index = 0;

    while index < tokenizer.events.len() {
        let event = &tokenizer.events[index];

        if event.kind == Kind::Enter && event.name == Name::Content {
            // Exit:Content
            let mut exit_index = index + 1;

            loop {
                let mut enter_index = exit_index + 1;

                if enter_index == tokenizer.events.len()
                    || tokenizer.events[enter_index].name != Name::LineEnd
                {
                    break;
                }

                // Skip past line ending.
                enter_index += 2;

                // Skip past prefix.
                while enter_index < tokenizer.events.len() {
                    let event = &tokenizer.events[enter_index];

                    if event.name != Name::SpaceOrTab
                        && event.name != Name::ListItemIndent
                        && event.name != Name::FootnoteDefinitionIndent
                        && event.name != Name::BlockQuotePrefix
                        && event.name != Name::BlockQuoteMarker
                    {
                        break;
                    }

                    enter_index += 1;
                }

                if enter_index == tokenizer.events.len()
                    || tokenizer.events[enter_index].name != Name::Content
                {
                    break;
                }

                // Set Exit:Content point to Exit:LineEnding.
                tokenizer.events[exit_index].point = tokenizer.events[exit_index + 2].point.clone();
                // Remove Enter:LineEnding, Exit:LineEnding.
                tokenizer.map.add(exit_index + 1, 2, vec![]);

                // Link Enter:Content to Enter:Content on this line and vice versa.
                tokenizer.events[exit_index - 1].link.as_mut().unwrap().next = Some(enter_index);
                tokenizer.events[enter_index]
                    .link
                    .as_mut()
                    .unwrap()
                    .previous = Some(exit_index - 1);

                // Potential next start.
                exit_index = enter_index + 1;
            }

            // Move to `Exit:Content`.
            index = exit_index;
        }

        index += 1;
    }

    tokenizer.map.consume(&mut tokenizer.events);

    subtokenize(
        &mut tokenizer.events,
        tokenizer.parse_state,
        Some(&Content::Content),
    )
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    ChunkStart,
    ChunkInside,
    DefinitionBefore,
    DefinitionAfter,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Content(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::ChunkStart => chunk_start(tokenizer),
        Step::ChunkInside => chunk_inside(tokenizer),
        Step::DefinitionBefore => definition_before(tokenizer),
        Step::DefinitionAfter => definition_after(tokenizer),
    }
}
