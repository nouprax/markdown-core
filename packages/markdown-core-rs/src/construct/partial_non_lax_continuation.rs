//! Non-lazy continuation.
//!
//! This is a tiny helper that [flow][] constructs can use to make sure that
//! the following line is not lazy.
//! For example, [html (flow)][html_flow] and ([raw (flow)][raw_flow],
//! [indented][code_indented]), stop when the next line is lazy.
//!
//! [flow]: crate::construct::flow
//! [raw_flow]: crate::construct::raw_flow
//! [code_indented]: crate::construct::code_indented
//! [html_flow]: crate::construct::html_flow

use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// At eol, before continuation.
///
/// ```markdown
/// > | * ```js
///            ^
///   | b
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnd);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnd);
            State::Next(StateName::NonLaxContinuation(Step::After))
        }
        _ => State::Nok,
    }
}

/// A continuation.
///
/// ```markdown
///   | * ```js
/// > | b
///     ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.lax {
        State::Nok
    } else {
        State::Ok
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
        StateName::NonLaxContinuation(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::After => after(tokenizer),
    }
}
