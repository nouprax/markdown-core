//! Definition body occurs in the [document][] content type.
//!
//! ## Grammar
//!
//! Definition body forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: the body follows a definition term, whose line is before
//! ; it with at most one blank line between, or another definition body.
//! definition_body ::= prefix [flow *(eol *space_or_tab flow)]
//! prefix ::= 0*3space_or_tab (':' | '~') (eol | 1*4space_or_tab)
//! ```
//!
//! A body is a container like a [list item][list_item], with its padding
//! and continuation rules: one to four columns of padding are its own, more
//! leave one and start code, and a marker that ends its line takes the
//! content of the lines after it, indented one column past the marker.
//!
//! Bodies follow the [definition term][definition_term] they define, or
//! another body of that term.
//! The [resolver][resolve] groups a term and its bodies into a
//! [`DefinitionItem`][Name::DefinitionItem], and the items that follow each
//! other, with only blank lines between, into a
//! [`DefinitionList`][Name::DefinitionList].
//!
//! ## Tokens
//!
//! * [`ContainerIndent`][Name::ContainerIndent]
//! * [`DefinitionBody`][Name::DefinitionBody]
//! * [`DefinitionBodyMarker`][Name::DefinitionBodyMarker]
//! * [`DefinitionBodyPrefix`][Name::DefinitionBodyPrefix]
//! * [`DefinitionItem`][Name::DefinitionItem]
//! * [`DefinitionList`][Name::DefinitionList]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! [document]: crate::construct::document
//! [list_item]: crate::construct::list_item
//! [definition_term]: crate::construct::definition_term

use crate::construct::blank_line;
use crate::construct::partial_space_or_tab::{
    space_or_tab_min_max, space_or_tab_with_options, Options as SpaceOrTabOptions,
};
use crate::event::{Event, Kind, Name};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::{Container, Tokenizer};
use crate::util::{
    constant::TAB_SIZE,
    skip,
    slice::{Position, Slice},
};
use alloc::{vec, vec::Vec};

/// Start of a definition body.
///
/// ```markdown
///   | a
/// > | : b
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    let document = &tokenizer.document;
    let continued = document.continued;
    // After a term, every container went on, and the term's marker is due on
    // this line.
    let term = document.stack.len() == continued + 1
        && document
            .child
            .as_ref()
            .map_or(false, |child| child.term == Some(tokenizer.point.line));
    // After a body at this depth, which did not go on.
    let peer = document.stack.len() > continued + 1
        && document.stack.last().map_or(false, |container| {
            container.kind == Container::DefinitionBody
        });
    if !term && !peer {
        return State::Nok;
    }
    tokenizer.enter(Name::DefinitionBody);
    tokenizer.enter(Name::DefinitionBodyPrefix);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::DefinitionBody(Step::Before)),
            State::Nok,
        );
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::DefinitionBody(Step::Before))
    }
}

/// At the marker.
///
/// ```markdown
///   | a
/// > | : b
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b':' | b'~')) {
        tokenizer.enter(Name::DefinitionBodyMarker);
        tokenizer.consume();
        tokenizer.exit(Name::DefinitionBodyMarker);
        State::Next(StateName::DefinitionBody(Step::MarkerAfter))
    } else {
        State::Nok
    }
}

/// After the marker.
///
/// ```markdown
///   | a
/// > | : b
///      ^
/// ```
fn marker_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Redo(StateName::DefinitionBody(Step::After { blank: true })),
        Some(b'\t' | b' ') => {
            tokenizer.check(
                State::Next(StateName::DefinitionBody(Step::After { blank: true })),
                State::Next(StateName::DefinitionBody(Step::MarkerAfterFilled)),
            );
            State::Redo(StateName::BlankLine(blank_line::Step::Start))
        }
        _ => State::Nok,
    }
}

/// After the marker, before content.
///
/// ```markdown
///   | a
/// > | : b
///      ^
/// ```
fn marker_after_filled(tokenizer: &mut Tokenizer) -> State {
    // Up to the largest padding, or one column when there is more.
    tokenizer.attempt(
        State::Next(StateName::DefinitionBody(Step::After { blank: false })),
        State::Next(StateName::DefinitionBody(Step::PrefixOther)),
    );
    tokenizer.attempt(
        State::Next(StateName::DefinitionBody(Step::WhitespaceAfter)),
        State::Nok,
    );
    State::Redo(space_or_tab_min_max(1, TAB_SIZE))
}

/// After the padding.
///
/// ```markdown
///   | a
/// > | : b
///       ^
/// ```
fn whitespace_after(tokenizer: &mut Tokenizer) -> State {
    if let Some(b'\t' | b' ') = tokenizer.current {
        State::Nok
    } else {
        State::Ok
    }
}

/// After the marker, before more padding than a body takes: one column is
/// its own.
///
/// ```markdown
///   | a
/// > | :      b
///      ^
/// ```
fn prefix_other(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.enter(Name::SpaceOrTab);
            tokenizer.consume();
            tokenizer.exit(Name::SpaceOrTab);
            State::Next(StateName::DefinitionBody(Step::After { blank: false }))
        }
        _ => State::Nok,
    }
}

/// After the prefix.
///
/// ```markdown
///   | a
/// > | : b
///       ^
/// ```
fn after(tokenizer: &mut Tokenizer, blank: bool) -> State {
    let start = skip::to_back(
        &tokenizer.events,
        tokenizer.events.len() - 1,
        &[Name::DefinitionBody],
    );
    let mut prefix = Slice::from_position(
        tokenizer.parse_state.bytes,
        &Position {
            start: &tokenizer.events[start].point,
            end: &tokenizer.point,
        },
    )
    .len();
    if blank {
        prefix += 1;
    }
    let container = &mut tokenizer.document.stack[tokenizer.document.continued];
    container.size = prefix;
    // The term's marker is read.
    if let Some(child) = tokenizer.document.child.as_mut() {
        child.term = None;
    }
    tokenizer.exit(Name::DefinitionBodyPrefix);
    tokenizer.register_resolver(ResolveName::DefinitionList);
    State::Ok
}

/// Start of a continuation line of a body.
///
/// ```markdown
///   | : a
/// > |   b
///     ^
/// ```
fn cont_start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.check(
        State::Next(StateName::DefinitionBody(Step::ContBlank)),
        State::Next(StateName::DefinitionBody(Step::ContFilled)),
    );
    State::Redo(StateName::BlankLine(blank_line::Step::Start))
}

/// At a blank continuation line, which the body takes: the next line that is
/// not blank decides whether the body goes on.
///
/// ```markdown
///   | : a
/// > | ␊
///     ^
///   |   b
/// ```
fn cont_blank(tokenizer: &mut Tokenizer) -> State {
    let size = tokenizer.document.stack[tokenizer.document.continued].size;
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        State::Redo(indent(0, size))
    } else {
        State::Ok
    }
}

/// At a filled continuation line, indented to the content of the body.
///
/// ```markdown
///   | : a
/// > |   b
///     ^
/// ```
fn cont_filled(tokenizer: &mut Tokenizer) -> State {
    let size = tokenizer.document.stack[tokenizer.document.continued].size;
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        State::Redo(indent(size, size))
    } else {
        State::Nok
    }
}

/// The indentation a continuation line consumes for the body.
fn indent(min: usize, max: usize) -> StateName {
    space_or_tab_with_options(SpaceOrTabOptions {
        kind: Name::ContainerIndent,
        min,
        max,
        content: None,
        connect: false,
    })
}

/// A definition marker, as a test of a whole line: no more than three
/// columns of indentation, `:` or `~`, and then whitespace or the line's end.
///
/// ```markdown
///   | a
/// > | : b
///     ^
/// ```
fn marker_start(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::DefinitionBody(Step::MarkerTest)),
            State::Nok,
        );
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::DefinitionBody(Step::MarkerTest))
    }
}

/// At the marker a test looks for.
///
/// ```markdown
///   | a
/// > | : b
///     ^
/// ```
fn marker_test(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b':' | b'~')) {
        tokenizer.consume();
        State::Next(StateName::DefinitionBody(Step::MarkerTestAfter))
    } else {
        State::Nok
    }
}

/// After the marker a test looks for.
///
/// ```markdown
///   | a
/// > | : b
///      ^
/// ```
fn marker_test_after(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\t' | b'\n' | b' ')) {
        State::Ok
    } else {
        State::Nok
    }
}

/// Whether the raw line starting at `index` of `bytes` may hold a definition
/// marker, after the bytes a container prefix may have: a quick refusal of
/// almost every line, before a lookahead decides.
pub fn marks(bytes: &[u8], mut index: usize) -> bool {
    while index < bytes.len() && matches!(bytes[index], b'\t' | b' ' | b'>') {
        index += 1;
    }
    matches!(bytes.get(index), Some(b':' | b'~'))
        && matches!(
            bytes.get(index + 1),
            None | Some(b'\t' | b'\n' | b'\r' | b' ')
        )
}

/// Whether the raw line starting at `index` of `bytes` is blank, after the
/// bytes a container prefix may have; and where the line after it starts.
pub fn blank_at(bytes: &[u8], mut index: usize) -> Option<usize> {
    while index < bytes.len() && matches!(bytes[index], b'\t' | b' ' | b'>') {
        index += 1;
    }
    match bytes.get(index) {
        Some(b'\r') if bytes.get(index + 1) == Some(&b'\n') => Some(index + 2),
        Some(b'\n' | b'\r') => Some(index + 1),
        _ => None,
    }
}

/// Group terms and their bodies into definitions, and definitions that
/// follow each other into lists.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    // Each definition: its term and the exit of its last body, in order.
    let mut items: Vec<(usize, usize)> = vec![];
    let mut index = 0;
    while index < tokenizer.events.len() {
        let event = &tokenizer.events[index];
        if event.kind == Kind::Enter && event.name == Name::DefinitionTerm {
            let term = index;
            let exit = skip::opt(&tokenizer.events, term, &[Name::DefinitionTerm]) - 1;
            let mut end = exit;
            loop {
                let next = skip::opt(&tokenizer.events, end + 1, &BETWEEN);
                if next < tokenizer.events.len()
                    && tokenizer.events[next].kind == Kind::Enter
                    && tokenizer.events[next].name == Name::DefinitionBody
                {
                    end = skip::opt(&tokenizer.events, next, &[Name::DefinitionBody]) - 1;
                } else {
                    break;
                }
            }
            if end == exit {
                // A term no body follows reads as the paragraph it is.
                tokenizer.events[term].name = Name::Paragraph;
                tokenizer.events[exit].name = Name::Paragraph;
            } else {
                items.push((term, end));
            }
        }
        index += 1;
    }
    // Items first: a list's enter goes before its first item's, and its exit
    // after its last item's.
    for (term, end) in &items {
        wrap(tokenizer, Name::DefinitionItem, *term, *end);
    }
    // A definition goes on the list of the one before it when only blank
    // lines are between them; the definitions inside it come between.
    let mut listed = vec![false; items.len()];
    for first in 0..items.len() {
        if listed[first] {
            continue;
        }
        let mut last = first;
        loop {
            let end = items[last].1;
            let next = (last + 1..items.len()).find(|item| items[*item].0 > end);
            match next {
                Some(next) if skip::opt(&tokenizer.events, end + 1, &BETWEEN) == items[next].0 => {
                    listed[next] = true;
                    last = next;
                }
                _ => break,
            }
        }
        wrap(
            tokenizer,
            Name::DefinitionList,
            items[first].0,
            items[last].1,
        );
    }
    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// What may be between a term and its bodies, and between definitions of one
/// list.
const BETWEEN: [Name; 5] = [
    Name::BlankLineEnding,
    Name::BlockQuotePrefix,
    Name::ContainerIndent,
    Name::LineEnding,
    Name::SpaceOrTab,
];

/// Wrap the events from `first` to the exit `last` in `name`.
fn wrap(tokenizer: &mut Tokenizer, name: Name, first: usize, last: usize) {
    let enter = Event {
        kind: Kind::Enter,
        name,
        point: tokenizer.events[first].point.clone(),
        link: None,
    };
    let exit = Event {
        kind: Kind::Exit,
        name,
        point: tokenizer.events[last].point.clone(),
        link: None,
    };
    tokenizer.map.add_before(first, 0, vec![enter]);
    tokenizer.map.add(last + 1, 0, vec![exit]);
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    MarkerAfter,
    MarkerAfterFilled,
    WhitespaceAfter,
    PrefixOther,
    After { blank: bool },
    ContStart,
    ContBlank,
    ContFilled,
    MarkerStart,
    MarkerTest,
    MarkerTestAfter,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::DefinitionBody(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::MarkerAfter => marker_after(tokenizer),
        Step::MarkerAfterFilled => marker_after_filled(tokenizer),
        Step::WhitespaceAfter => whitespace_after(tokenizer),
        Step::PrefixOther => prefix_other(tokenizer),
        Step::After { blank } => after(tokenizer, blank),
        Step::ContStart => cont_start(tokenizer),
        Step::ContBlank => cont_blank(tokenizer),
        Step::ContFilled => cont_filled(tokenizer),
        Step::MarkerStart => marker_start(tokenizer),
        Step::MarkerTest => marker_test(tokenizer),
        Step::MarkerTestAfter => marker_test_after(tokenizer),
    }
}
