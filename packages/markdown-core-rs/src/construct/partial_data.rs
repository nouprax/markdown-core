//! Data occurs in the [string][] and [text][] content types.
//!
//! It can include anything (except for line endings) and stops at certain
//! characters.
//!
//! [string]: crate::construct::string
//! [text]: crate::construct::text

use crate::event::{Kind, Name};
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::Tokenizer;
use alloc::vec;

/// At beginning of data.
///
/// ```markdown
/// > | abc
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer, markers: &'static [u8]) -> State {
    // Make sure to eat the first `markers`.
    if let Some(byte) = tokenizer.current {
        if markers.contains(&byte) {
            tokenizer.enter(Name::Data);
            tokenizer.consume();
            return State::Next(StateName::Data(Step::Inside { markers }));
        }
    }

    State::Retry(StateName::Data(Step::AtBreak { markers }))
}

/// Before something.
///
/// ```markdown
/// > | abc
///     ^
/// ```
fn at_break(tokenizer: &mut Tokenizer, markers: &'static [u8]) -> State {
    if let Some(byte) = tokenizer.current {
        if !markers.contains(&byte) {
            if byte == b'\n' {
                tokenizer.enter(Name::LineEnding);
                tokenizer.consume();
                tokenizer.exit(Name::LineEnding);
                return State::Next(StateName::Data(Step::AtBreak { markers }));
            }
            tokenizer.enter(Name::Data);
            return State::Retry(StateName::Data(Step::Inside { markers }));
        }
    }

    State::Ok
}

/// In data.
///
/// ```markdown
/// > | abc
///     ^^^
/// ```
fn inside(tokenizer: &mut Tokenizer, markers: &'static [u8]) -> State {
    if let Some(byte) = tokenizer.current {
        if byte != b'\n' && !markers.contains(&byte) {
            tokenizer.consume();
            return State::Next(StateName::Data(Step::Inside { markers }));
        }
    }

    tokenizer.exit(Name::Data);
    State::Retry(StateName::Data(Step::AtBreak { markers }))
}

/// Merge adjacent data events.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    let mut index = 0;

    // Loop through events and merge adjacent data events.
    while index < tokenizer.events.len() {
        let event = &tokenizer.events[index];

        if event.kind == Kind::Enter && event.name == Name::Data {
            // Move to exit.
            index += 1;

            let mut exit_index = index;

            // Find the farthest `data` event exit event.
            while exit_index + 1 < tokenizer.events.len()
                && tokenizer.events[exit_index + 1].name == Name::Data
            {
                exit_index += 2;
            }

            if exit_index > index {
                tokenizer.map.add(index, exit_index - index, vec![]);
                // Change positional info.
                tokenizer.events[index].point = tokenizer.events[exit_index].point.clone();
                // Move to the end.
                index = exit_index;
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
    Start { markers: &'static [u8] },
    Inside { markers: &'static [u8] },
    AtBreak { markers: &'static [u8] },
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Data(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start { markers } => start(tokenizer, markers),
        Step::Inside { markers } => inside(tokenizer, markers),
        Step::AtBreak { markers } => at_break(tokenizer, markers),
    }
}
