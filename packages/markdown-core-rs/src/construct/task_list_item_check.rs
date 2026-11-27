//! Task list item check occurs at the end of a [list item][list_item]'s
//! prefix.
//!
//! ## Grammar
//!
//! Checks form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: the value is one Unicode scalar, no line ending.
//! task_list_item_check ::= '[' value ']' 1*space_or_tab
//! ```
//!
//! The check is the first thing after the list item marker and its
//! padding, before the first block of the item is decided: `- [?] # a` is
//! an item checked `?` holding a heading.
//! It does not count toward the item's indent: continuation lines align with
//! the content after the marker, as without a check.
//! The whitespace after the check is part of it, so a line with nothing else
//! leaves the item empty, with no paragraph that a lax line could continue.
//! Its value is any one scalar, kept as written: `x`, a space, `?`, `🚀` or
//! `]`.
//!
//! ## Tokens
//!
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//! * [`TaskListItemCheck`][Name::TaskListItemCheck]
//! * [`TaskListItemMarker`][Name::TaskListItemMarker]
//! * [`TaskListItemValue`][Name::TaskListItemValue]
//!
//! ## References
//!
//! * [*§ 5.3 Task list items (extension)* in `GFM`](https://github.github.com/gfm/#task-list-items-extension-)
//!
//! [list_item]: crate::construct::list_item

use crate::construct::partial_space_or_tab::space_or_tab;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// At the check, after the list item marker and its padding.
///
/// ```markdown
/// > | * [x] y.
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'[') {
        tokenizer.enter(Name::TaskListItemCheck);
        tokenizer.enter(Name::TaskListItemMarker);
        tokenizer.consume();
        tokenizer.exit(Name::TaskListItemMarker);
        State::Next(StateName::TaskListItemCheck(Step::Inside))
    } else {
        State::Nok
    }
}

/// At the value: one scalar, of as many bytes as its first says.
///
/// ```markdown
/// > | * [x] y.
///        ^
/// ```
fn inside(tokenizer: &mut Tokenizer) -> State {
    let size = match tokenizer.current {
        None | Some(b'\n' | b'\r') => return State::Nok,
        Some(0xF0..=0xFF) => 4,
        Some(0xE0..=0xEF) => 3,
        Some(0xC0..=0xDF) => 2,
        Some(_) => 1,
    };
    tokenizer.enter(Name::TaskListItemValue);
    State::Redo(StateName::TaskListItemCheck(Step::Value { size }))
}

/// In the value, with `size` bytes of it left.
///
/// ```markdown
/// > | * [x] y.
///        ^
/// ```
fn value(tokenizer: &mut Tokenizer, size: usize) -> State {
    if size == 0 {
        tokenizer.exit(Name::TaskListItemValue);
        State::Redo(StateName::TaskListItemCheck(Step::Close))
    } else {
        tokenizer.consume();
        State::Next(StateName::TaskListItemCheck(Step::Value { size: size - 1 }))
    }
}

/// At the closing bracket.
///
/// ```markdown
/// > | * [x] y.
///         ^
/// ```
fn close(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b']') {
        tokenizer.enter(Name::TaskListItemMarker);
        tokenizer.consume();
        tokenizer.exit(Name::TaskListItemMarker);
        State::Next(StateName::TaskListItemCheck(Step::After))
    } else {
        State::Nok
    }
}

/// After the check, at the whitespace it needs.
///
/// ```markdown
/// > | * [x] y.
///          ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::TaskListItemCheck(Step::End)),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Nok
    }
}

/// After the whitespace.
///
/// ```markdown
/// > | * [x] y.
///           ^
/// ```
fn end(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::TaskListItemCheck);
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Inside,
    Value { size: usize },
    Close,
    After,
    End,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::TaskListItemCheck(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Inside => inside(tokenizer),
        Step::Value { size } => value(tokenizer, size),
        Step::Close => close(tokenizer),
        Step::After => after(tokenizer),
        Step::End => end(tokenizer),
    }
}
