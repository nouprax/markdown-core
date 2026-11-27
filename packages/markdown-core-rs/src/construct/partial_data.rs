//! Data occurs in the [string][plain] and [text][] content types.
//!
//! It can include anything (except for line endings) and stops at certain
//! characters.
//!
//! [plain]: crate::construct::plain
//! [text]: crate::construct::text

use crate::choice::List;
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
fn start(tokenizer: &mut Tokenizer, list: List) -> State {
    // At a byte that can start a construct, none did: eat it as data.
    if let Some(code) = tokenizer.current {
        if list.choice().marks(code) {
            tokenizer.enter(Name::Data);
            tokenizer.consume();
            return State::Next(StateName::Data(Step::Inside { list }));
        }
    }

    State::Redo(StateName::Data(Step::AtBreak { list }))
}

/// Before something.
///
/// ```markdown
/// > | abc
///     ^
/// ```
fn at_break(tokenizer: &mut Tokenizer, list: List) -> State {
    if let Some(code) = tokenizer.current {
        if !list.choice().marks(code) {
            if code == b'\n' {
                tokenizer.enter(Name::LineEnd);
                tokenizer.consume();
                tokenizer.exit(Name::LineEnd);
                return State::Next(StateName::Data(Step::AtBreak { list }));
            }
            tokenizer.enter(Name::Data);
            return State::Redo(StateName::Data(Step::Inside { list }));
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
fn inside(tokenizer: &mut Tokenizer, list: List) -> State {
    if let Some(code) = tokenizer.current {
        if code != b'\n' && !list.choice().marks(code) {
            tokenizer.consume();
            return State::Next(StateName::Data(Step::Inside { list }));
        }
    }

    tokenizer.exit(Name::Data);
    State::Redo(StateName::Data(Step::AtBreak { list }))
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
    Start { list: List },
    Inside { list: List },
    AtBreak { list: List },
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Data(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start { list } => start(tokenizer, list),
        Step::Inside { list } => inside(tokenizer, list),
        Step::AtBreak { list } => at_break(tokenizer, list),
    }
}
