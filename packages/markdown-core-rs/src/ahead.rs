//! Lookahead over later lines, under the containers of the current line.
//!
//! A flow construct that commits only when a later line closes it (such as
//! a `%%` comment block) cannot try and fail across lines: the document
//! makes container decisions for every line it feeds, based on what the
//! flow is in.
//! So before it commits, the construct looks ahead: from the next line on,
//! each line continues the containers around the current line with their
//! own continuation states, and the rest of the line is tried with the
//! construct's `test` state.
//! The lookahead stops at the first line that passes (found), at a line that
//! does not continue those containers, or at the end (not found).
//!
//! The lookahead runs in its own tokenizer, so its events are dropped.

use crate::event::Point;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Whether a line after the one `tokenizer` is at the end of passes `test`
/// under the containers of the current line.
pub fn ahead(tokenizer: &Tokenizer, test: StateName) -> bool {
    let source = tokenizer.parse_state.source;
    let index = tokenizer.point.index + 1;
    if tokenizer.current != Some(b'\n') || index >= source.len() {
        return false;
    }
    let point = Point {
        line: tokenizer.point.line + 1,
        column: 1,
        index,
        vs: 0,
    };
    let mut scratch = Tokenizer::new(point, tokenizer.parse_state);
    scratch
        .document
        .stack
        .extend_from_slice(&tokenizer.ahead.stack);
    scratch.ahead.test = Some(test);
    // Whether found or not, the lookahead ends well.
    scratch.attempt(State::Ok, State::Ok);
    scratch.run(
        (index, 0),
        (source.len(), 0),
        State::Next(StateName::Ahead(Step::Line)),
    );
    scratch.ahead.found
}

/// At the start of a line.
fn line(tokenizer: &mut Tokenizer) -> State {
    tokenizer.document.continued = 0;
    State::Redo(StateName::Ahead(Step::Cont))
}

/// Before the next container, or the test after the last.
fn cont(tokenizer: &mut Tokenizer) -> State {
    let continued = tokenizer.document.continued;
    if continued < tokenizer.document.stack.len() {
        let name = tokenizer.document.stack[continued].kind.cont();
        tokenizer.attempt(State::Next(StateName::Ahead(Step::Kept)), State::Nok);
        State::Redo(name)
    } else {
        let test = tokenizer.ahead.test.expect("expected a test");
        tokenizer.attempt(
            State::Next(StateName::Ahead(Step::Found)),
            State::Next(StateName::Ahead(Step::Skip)),
        );
        State::Redo(test)
    }
}

/// After a container that continued.
fn kept(tokenizer: &mut Tokenizer) -> State {
    tokenizer.document.continued += 1;
    State::Redo(StateName::Ahead(Step::Cont))
}

/// After a line that passed the test.
fn found(tokenizer: &mut Tokenizer) -> State {
    tokenizer.ahead.found = true;
    State::Ok
}

/// In a line that failed the test, to its end.
fn skip(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.consume();
            State::Next(StateName::Ahead(Step::Line))
        }
        Some(_) => {
            tokenizer.consume();
            State::Next(StateName::Ahead(Step::Skip))
        }
    }
}

/// Steps of a lookahead.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Line,
    Cont,
    Kept,
    Found,
    Skip,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Ahead(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Line => line(tokenizer),
        Step::Cont => cont(tokenizer),
        Step::Kept => kept(tokenizer),
        Step::Found => found(tokenizer),
        Step::Skip => skip(tokenizer),
    }
}
