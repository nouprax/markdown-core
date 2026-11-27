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

use crate::construct::{flow, string, text};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Bytes a construct can start at.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Starts {
    /// Any byte, and the end of the input.
    Any,
    /// One of these bytes.
    Bytes(&'static [u8]),
}

impl Starts {
    /// Whether `byte` (`None` is the end of the input) can start the
    /// construct.
    fn matches(self, byte: Option<u8>) -> bool {
        match self {
            Starts::Any => true,
            Starts::Bytes(bytes) => matches!(byte, Some(byte) if bytes.contains(&byte)),
        }
    }
}

/// One construct in a list.
#[derive(Debug)]
pub struct Entry {
    /// Bytes the construct can start at.
    pub starts: Starts,
    /// Start state of the construct.
    pub step: StateName,
}

/// Ordered constructs of a content type.
#[derive(Debug)]
pub struct Choice {
    /// Constructs, in the order they are tried.
    entries: &'static [Entry],
    /// State after a construct succeeded.
    ok: StateName,
    /// State when no construct succeeded.
    fallback: StateName,
    /// Bytes that can start some entry, by byte value.
    marks: [bool; 256],
}

impl Choice {
    /// Make a list.
    pub const fn new(entries: &'static [Entry], ok: StateName, fallback: StateName) -> Choice {
        let mut marks = [false; 256];
        let mut index = 0;
        while index < entries.len() {
            match entries[index].starts {
                Starts::Any => {
                    let mut byte = 0;
                    while byte < 256 {
                        marks[byte] = true;
                        byte += 1;
                    }
                }
                Starts::Bytes(bytes) => {
                    let mut at = 0;
                    while at < bytes.len() {
                        marks[bytes[at] as usize] = true;
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

    /// Whether `byte` can start some construct in this list.
    pub fn marks(&self, byte: u8) -> bool {
        self.marks[byte as usize]
    }
}

/// The lists.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum List {
    Flow,
    Text,
    String,
}

impl List {
    /// Get the constructs of this list.
    pub fn get(self) -> &'static Choice {
        match self {
            List::Flow => &flow::CHOICE,
            List::Text => &text::CHOICE,
            List::String => &string::CHOICE,
        }
    }
}

/// At the entry `index` of `list`, or a later one.
fn at(tokenizer: &mut Tokenizer, list: List, mut index: usize) -> State {
    let choice = list.get();

    while index < choice.entries.len() && !choice.entries[index].starts.matches(tokenizer.current) {
        index += 1;
    }

    if index == choice.entries.len() {
        State::Retry(choice.fallback)
    } else {
        tokenizer.attempt(
            State::Next(choice.ok),
            State::Next(StateName::Choice(Step::At {
                list,
                index: index + 1,
            })),
        );
        State::Retry(choice.entries[index].step)
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
