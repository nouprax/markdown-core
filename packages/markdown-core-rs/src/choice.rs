//! Ordered lists of constructs, tried one after another.
//!
//! Each content type (flow, text, string) has one [`Choice`]: the
//! constructs that can occur in it, in the order they are tried, and a
//! fallback (content in flow, data in text and string).
//! A construct says which bytes it can start at with [`Starts`]; the driver
//! here only attempts the entries whose [`Starts`] match the current byte,
//! goes to the list’s `ok` state when one succeeds, and to the fallback when
//! none does.
//! Which bytes can start a construct is also what data stops at, so the
//! data markers of text and string come from the same list.
//!
//! ## References
//!
//! * [`create-tokenizer.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark/dev/lib/create-tokenizer.js)
//! * [`constructs.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark/dev/lib/constructs.js)

use crate::construct::{flow, plain, text};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Bytes a construct can start at.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Starts {
    /// Any byte, and the end of the input.
    All,
    /// One of these bytes.
    Codes(&'static [u8]),
}

impl Starts {
    /// Whether `code` (`None` is the end of the input) can start the
    /// construct.
    fn matches(self, code: Option<u8>) -> bool {
        match self {
            Starts::All => true,
            Starts::Codes(source) => matches!(code, Some(code) if source.contains(&code)),
        }
    }
}

/// One construct in a list.
#[derive(Debug)]
pub struct Item {
    /// Bytes the construct can start at.
    pub starts: Starts,
    /// Start state of the construct.
    pub step: StateName,
}

/// Ordered constructs of a content type.
#[derive(Debug)]
pub struct Choice {
    /// Constructs, in the order they are tried.
    entries: &'static [Item],
    /// State after a construct succeeded.
    ok: StateName,
    /// State when no construct succeeded.
    fallback: StateName,
    /// Bytes that can start some entry, by byte value.
    marks: [bool; 256],
}

impl Choice {
    /// Make a list.
    pub const fn new(entries: &'static [Item], ok: StateName, fallback: StateName) -> Choice {
        let mut marks = [false; 256];
        let mut index = 0;
        while index < entries.len() {
            match entries[index].starts {
                Starts::All => {
                    let mut code = 0;
                    while code < 256 {
                        marks[code] = true;
                        code += 1;
                    }
                }
                Starts::Codes(source) => {
                    let mut at = 0;
                    while at < source.len() {
                        marks[source[at] as usize] = true;
                        at += 1;
                    }
                }
            }
            index += 1;
        }
        Choice {
            entries,
            ok,
            fallback,
            marks,
        }
    }

    /// Whether `code` can start some construct in this list.
    pub fn marks(&self, code: u8) -> bool {
        self.marks[code as usize]
    }
}

/// The lists.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum List {
    Flow,
    Text,
    Plain,
}

impl List {
    /// Get the constructs of this list.
    pub fn choice(self) -> &'static Choice {
        match self {
            List::Flow => &flow::CHOICE,
            List::Text => &text::CHOICE,
            List::Plain => &plain::CHOICE,
        }
    }
}

/// At the entry `index` of `list`, or a later one.
fn at(tokenizer: &mut Tokenizer, list: List, mut index: usize) -> State {
    let choice = list.choice();

    while index < choice.entries.len() && !choice.entries[index].starts.matches(tokenizer.current) {
        index += 1;
    }

    if index == choice.entries.len() {
        State::Redo(choice.fallback)
    } else {
        tokenizer.attempt(
            State::Next(choice.ok),
            State::Next(StateName::Choice(Step::At {
                list,
                index: index + 1,
            })),
        );
        State::Redo(choice.entries[index].step)
    }
}

/// Steps of the driver.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    At { list: List, index: usize },
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Choice(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::At { list, index } => at(tokenizer, list, index),
    }
}

/// Start trying `list`.
pub fn start(list: List) -> StateName {
    StateName::Choice(Step::At { list, index: 0 })
}
