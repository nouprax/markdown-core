//! List item occurs in the [document][] content type.
//!
//! ## Grammar
//!
//! List item forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: if there is no space after the marker, the start must be followed by an `eol`.
//! ; Restriction: if the first line after the marker is not blank and starts with `5(space_or_tab)`,
//! ; only the first `space_or_tab` is part of the start.
//! list_item_start ::= '*' | '+' | '-' | 1*9(ascii_decimal) ('.' | ')') [1*4 space_or_tab]
//!
//! ; Restriction: blank line allowed, except when this is the first continuation after a blank start.
//! ; Restriction: if not blank, the line must be indented, exactly `n` times.
//! list_item_cont ::= [n(space_or_tab)]
//! ```
//!
//! Further lines that are not prefixed with `list_item_cont` cause the list
//! item to be exited, except when those lines are lazy continuation or blank.
//! Like so many things in markdown, list items too are complex.
//! See [*§ Phase 1: block structure* in `CommonMark`][commonmark_block] for
//! more on parsing details.
//!
//! As list item is a container, it takes several bytes from the start of the
//! line, while the rest of the line includes more containers or flow.
//!
//! ## HTML
//!
//! List item relates to the `<li>`, `<ol>`, and `<ul>` elements in HTML.
//! See [*§ 4.4.8 The `li` element*][html_li],
//! [*§ 4.4.5 The `ol` element*][html_ol], and
//! [*§ 4.4.7 The `ul` element*][html_ul] in the HTML spec for more info.
//!
//! ## Recommendation
//!
//! Use a single space after a marker.
//! Never use lazy continuation.
//!
//! ## Tokens
//!
//! * [`ListItem`][Name::ListItem]
//! * [`ListItemMarker`][Name::ListItemMarker]
//! * [`ListItemPrefix`][Name::ListItemPrefix]
//! * [`ListItemValue`][Name::ListItemValue]
//! * [`ListOrdered`][Name::ListOrdered]
//! * [`ListUnordered`][Name::ListUnordered]
//!
//! ## References
//!
//! * [`list.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/list.js)
//! * [*§ 5.2 List items* in `CommonMark`](https://spec.commonmark.org/0.31/#list-items)
//! * [*§ 5.3 Lists* in `CommonMark`](https://spec.commonmark.org/0.31/#lists)
//!
//! [document]: crate::construct::document
//! [html_li]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-li-element
//! [html_ol]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-ol-element
//! [html_ul]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-ul-element
//! [commonmark_block]: https://spec.commonmark.org/0.31/#phase-1-block-structure

use crate::construct::partial_space_or_tab::{
    space_or_tab_min_max, space_or_tab_with_options, Options as SpaceOrTabOptions,
};
use crate::construct::{blank_line, thematic_break};
use crate::event::{Kind, Name};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::Tokenizer;
use crate::util::{
    constant::{LIST_ITEM_VALUE_SIZE_MAX, TAB_SIZE},
    skip,
    slice::{Position, Slice},
};
use alloc::{vec, vec::Vec};

/// Start of list item.
///
/// ```markdown
/// > | * a
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::ListItem);

    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(State::Next(StateName::ListItem(Step::Before)), State::Nok);
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::ListItem(Step::Before))
    }
}

/// After optional whitespace, at list item prefix.
///
/// ```markdown
/// > | * a
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    // Unordered.
    if matches!(tokenizer.current, Some(b'*' | b'-')) {
        tokenizer.check(
            State::Nok,
            State::Next(StateName::ListItem(Step::BeforeUnordered)),
        );
        State::Redo(StateName::ThematicBreak(thematic_break::Step::Start))
    } else if tokenizer.current == Some(b'+') {
        State::Redo(StateName::ListItem(Step::BeforeUnordered))
    }
    // Ordered.
    else if tokenizer.current == Some(b'1')
        || (matches!(tokenizer.current, Some(b'0'..=b'9')) && !tokenizer.interrupt)
    {
        State::Redo(StateName::ListItem(Step::BeforeOrdered))
    } else {
        State::Nok
    }
}

/// At unordered list item marker.
///
/// The line is not a thematic break.
///
/// ```markdown
/// > | * a
///     ^
/// ```
fn before_unordered(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::ListItemPrefix);
    State::Redo(StateName::ListItem(Step::Marker))
}

/// At ordered list item value.
///
/// ```markdown
/// > | * a
///     ^
/// ```
fn before_ordered(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::ListItemPrefix);
    tokenizer.enter(Name::ListItemValue);
    State::Redo(StateName::ListItem(Step::Value { size: 0 }))
}

/// In ordered list item value.
///
/// ```markdown
/// > | 1. a
///     ^
/// ```
fn value(tokenizer: &mut Tokenizer, size: usize) -> State {
    if matches!(tokenizer.current, Some(b'.' | b')')) && (!tokenizer.interrupt || size < 2) {
        tokenizer.exit(Name::ListItemValue);
        State::Redo(StateName::ListItem(Step::Marker))
    } else if matches!(tokenizer.current, Some(b'0'..=b'9')) && size + 1 < LIST_ITEM_VALUE_SIZE_MAX
    {
        tokenizer.consume();
        State::Next(StateName::ListItem(Step::Value { size: size + 1 }))
    } else {
        State::Nok
    }
}

/// At list item marker.
///
/// ```markdown
/// > | * a
///     ^
/// > | 1. b
///      ^
/// ```
fn marker(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::ListItemMarker);
    tokenizer.consume();
    tokenizer.exit(Name::ListItemMarker);
    State::Next(StateName::ListItem(Step::MarkerAfter))
}

/// After list item marker.
///
/// ```markdown
/// > | * a
///      ^
/// > | 1. b
///       ^
/// ```
fn marker_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.check(
        State::Next(StateName::ListItem(Step::After { blank: true })),
        State::Next(StateName::ListItem(Step::MarkerAfterFilled)),
    );
    State::Redo(StateName::BlankLine(blank_line::Step::Start))
}

/// After list item marker.
///
/// The marker is not followed by a blank line.
///
/// ```markdown
/// > | * a
///      ^
/// ```
fn marker_after_filled(tokenizer: &mut Tokenizer) -> State {
    // Attempt to parse up to the largest allowed indent, `nok` if there is more whitespace.
    tokenizer.attempt(
        State::Next(StateName::ListItem(Step::After { blank: false })),
        State::Next(StateName::ListItem(Step::PrefixOther)),
    );
    State::Redo(StateName::ListItem(Step::Whitespace))
}

/// After marker, at whitespace.
///
/// ```markdown
/// > | * a
///      ^
/// ```
fn whitespace(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::ListItem(Step::WhitespaceAfter)),
        State::Nok,
    );
    State::Redo(space_or_tab_min_max(1, TAB_SIZE))
}

/// After acceptable whitespace.
///
/// ```markdown
/// > | * a
///      ^
/// ```
fn whitespace_after(tokenizer: &mut Tokenizer) -> State {
    if let Some(b'\t' | b' ') = tokenizer.current {
        State::Nok
    } else {
        State::Ok
    }
}

/// After marker, followed by no indent or more indent that needed.
///
/// ```markdown
/// > | * a
///      ^
/// ```
fn prefix_other(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.enter(Name::SpaceOrTab);
            tokenizer.consume();
            tokenizer.exit(Name::SpaceOrTab);
            State::Next(StateName::ListItem(Step::After { blank: false }))
        }
        _ => State::Nok,
    }
}

/// After list item prefix.
///
/// ```markdown
/// > | * a
///       ^
/// ```
fn after(tokenizer: &mut Tokenizer, blank: bool) -> State {
    if blank && tokenizer.interrupt {
        State::Nok
    } else {
        let start = skip::to_back(
            &tokenizer.events,
            tokenizer.events.len() - 1,
            &[Name::ListItem],
        );
        let mut prefix = Slice::from_position(
            tokenizer.parse_state.source,
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

        container.blank_initial = blank;
        container.size = prefix;

        tokenizer.exit(Name::ListItemPrefix);
        tokenizer.add_resolver_before(ResolveName::ListItem);
        State::Ok
    }
}

/// Start of list item continuation.
///
/// ```markdown
///   | * a
/// > |   b
///     ^
/// ```
fn cont_start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.check(
        State::Next(StateName::ListItem(Step::ContBlank)),
        State::Next(StateName::ListItem(Step::ContFilled)),
    );
    State::Redo(StateName::BlankLine(blank_line::Step::Start))
}

/// Start of blank list item continuation.
///
/// ```markdown
///   | * a
/// > |
///     ^
///   |   b
/// ```
fn cont_blank(tokenizer: &mut Tokenizer) -> State {
    let container = &mut tokenizer.document.stack[tokenizer.document.continued];
    let size = container.size;

    if container.blank_initial {
        State::Nok
    } else if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        // Consume, optionally, at most `size`.
        State::Redo(indent(0, size))
    } else {
        State::Ok
    }
}

/// Start of non-blank list item continuation.
///
/// ```markdown
///   | * a
/// > |   b
///     ^
/// ```
fn cont_filled(tokenizer: &mut Tokenizer) -> State {
    let container = &mut tokenizer.document.stack[tokenizer.document.continued];
    let size = container.size;

    container.blank_initial = false;

    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        // Consume exactly `size`.
        State::Redo(indent(size, size))
    } else {
        State::Nok
    }
}

/// Find adjacent list items with the same marker.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    let mut lists_wip: Vec<(u8, usize, usize, usize)> = vec![];
    let mut lists: Vec<(u8, usize, usize, usize)> = vec![];
    let mut index = 0;
    let mut balance = 0;

    // Merge list items.
    while index < tokenizer.events.len() {
        let event = &tokenizer.events[index];

        if event.name == Name::ListItem {
            if event.kind == Kind::Enter {
                let end = skip::opt(&tokenizer.events, index, &[Name::ListItem]) - 1;
                let marker = skip::to(&tokenizer.events, index, &[Name::ListItemMarker]);
                // Guaranteed to be a valid ASCII byte.
                let marker = tokenizer.parse_state.source[tokenizer.events[marker].point.index];
                let current = (marker, balance, index, end);

                let mut list_index = lists_wip.len();
                let mut matched = false;

                while list_index > 0 {
                    list_index -= 1;
                    let previous = &lists_wip[list_index];
                    let before = skip::opt(
                        &tokenizer.events,
                        previous.3 + 1,
                        &[
                            Name::SpaceOrTab,
                            Name::ListItemIndent,
                            Name::FootnoteDefinitionIndent,
                            Name::LineEnd,
                            Name::BlankLineEnd,
                            Name::BlockQuotePrefix,
                        ],
                    );

                    if previous.0 == current.0 && previous.1 == current.1 && before == current.2 {
                        let previous_mut = &mut lists_wip[list_index];
                        previous_mut.3 = current.3;
                        lists.append(&mut lists_wip.split_off(list_index + 1));
                        matched = true;
                        break;
                    }
                }

                if !matched {
                    let mut index = lists_wip.len();
                    let mut exit = None;

                    while index > 0 {
                        index -= 1;

                        // If the current (new) item starts after where this
                        // item on the stack ends, we can remove it from the
                        // stack.
                        if current.2 > lists_wip[index].3 {
                            exit = Some(index);
                        } else {
                            break;
                        }
                    }

                    if let Some(exit) = exit {
                        lists.append(&mut lists_wip.split_off(exit));
                    }

                    lists_wip.push(current);
                }

                balance += 1;
            } else {
                balance -= 1;
            }
        }

        index += 1;
    }

    lists.append(&mut lists_wip);

    // Inject events.
    let mut index = 0;
    while index < lists.len() {
        let list_item = &lists[index];
        let mut list_start = tokenizer.events[list_item.2].clone();
        let mut list_end = tokenizer.events[list_item.3].clone();
        let name = match list_item.0 {
            b'.' | b')' => Name::ListOrdered,
            _ => Name::ListUnordered,
        };
        list_start.name = name;
        list_end.name = name;

        tokenizer.map.add(list_item.2, 0, vec![list_start]);
        tokenizer.map.add(list_item.3 + 1, 0, vec![list_end]);

        index += 1;
    }

    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// The indentation a continuation line consumes for the list item.
fn indent(min: usize, max: usize) -> StateName {
    space_or_tab_with_options(SpaceOrTabOptions {
        kind: Name::ListItemIndent,
        min,
        max,
        content: None,
        connect: false,
    })
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    BeforeOrdered,
    BeforeUnordered,
    Value { size: usize },
    Marker,
    MarkerAfter,
    After { blank: bool },
    MarkerAfterFilled,
    Whitespace,
    WhitespaceAfter,
    PrefixOther,
    ContStart,
    ContBlank,
    ContFilled,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::ListItem(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::BeforeOrdered => before_ordered(tokenizer),
        Step::BeforeUnordered => before_unordered(tokenizer),
        Step::Value { size } => value(tokenizer, size),
        Step::Marker => marker(tokenizer),
        Step::MarkerAfter => marker_after(tokenizer),
        Step::After { blank } => after(tokenizer, blank),
        Step::MarkerAfterFilled => marker_after_filled(tokenizer),
        Step::Whitespace => whitespace(tokenizer),
        Step::WhitespaceAfter => whitespace_after(tokenizer),
        Step::PrefixOther => prefix_other(tokenizer),
        Step::ContStart => cont_start(tokenizer),
        Step::ContBlank => cont_blank(tokenizer),
        Step::ContFilled => cont_filled(tokenizer),
    }
}
