//! Topic occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Topic forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: not after a letter, mark, decimal digit, `_`, joiner, or `&`.
//! ; Restriction: the name holds a letter, mark, emoji, `_`, or `-`.
//! ; Restriction: not followed by `#` or `://`.
//! topic ::= '#' segment *('/' segment)
//! segment ::= 1*(letter | mark | emoji | number | '_' | '-' | joiner)
//! ```
//!
//! As twitter-text bounds a hashtag: `C#` and `a#b` are no topics, and a
//! name of numbers alone (`#2026`) is none either.
//!
//! ## Tokens
//!
//! * [`Topic`][Name::Topic]
//! * [`TopicMarker`][Name::TopicMarker]
//! * [`TopicText`][Name::TopicText]
//!
//! [text]: crate::construct::text

use crate::choice::Starts;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::char::{after_index, before_index};
use crate::util::classes::{classes, DECIMAL, EMOJI, LETTER, MARK, NUMBER};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b"#");

/// The joiners twitter-text lets a name hold besides segment characters.
const JOINERS: [char; 16] = [
    '\u{200C}', '\u{200D}', '\u{A67E}', '\u{05BE}', '\u{05F3}', '\u{05F4}', '\u{FF5E}', '\u{301C}',
    '\u{309B}', '\u{309C}', '\u{30A0}', '\u{30FB}', '\u{3003}', '\u{0F0B}', '\u{0F0C}', '\u{00B7}',
];

/// What a character is to a name.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Part {
    /// Not part of a name.
    None,
    /// A number or joiner: part of a name, but not enough for one.
    Other,
    /// A letter, mark, emoji, `_`, or `-`.
    Word,
}

/// What `char` is to a name.
fn part(char: char) -> Part {
    if char.is_ascii() {
        if char.is_ascii_alphabetic() || char == '_' || char == '-' {
            Part::Word
        } else if char.is_ascii_digit() {
            Part::Other
        } else {
            Part::None
        }
    } else {
        let found = classes(char);
        if found & (LETTER | MARK | EMOJI) != 0 {
            Part::Word
        } else if found & NUMBER != 0 || JOINERS.contains(&char) {
            Part::Other
        } else {
            Part::None
        }
    }
}

/// Whether a topic can open after `char`.
fn opens_after(char: Option<char>) -> bool {
    match char {
        None => true,
        Some(char) if char.is_ascii() => char == '-' || (part(char) == Part::None && char != '&'),
        Some(char) => classes(char) & (LETTER | MARK | DECIMAL) == 0 && !JOINERS.contains(&char),
    }
}

/// The character at the current byte, and what it is to a name.
fn at(tokenizer: &Tokenizer) -> (Part, usize) {
    match tokenizer.current {
        None | Some(b'\n') => (Part::None, 0),
        Some(byte) if byte < 0x80 => (part(char::from(byte)), 1),
        Some(_) => match after_index(tokenizer.parse_state.bytes, tokenizer.point.index) {
            Some(char) => (part(char), char.len_utf8()),
            None => (Part::None, 0),
        },
    }
}

/// Start of a topic.
///
/// ```markdown
/// > | a #b c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    let before = match tokenizer.previous {
        None => None,
        Some(byte) if byte < 0x80 => Some(char::from(byte)),
        Some(_) => before_index(tokenizer.parse_state.bytes, tokenizer.point.index),
    };
    if tokenizer.current == Some(b'#') && opens_after(before) {
        tokenizer.enter(Name::Topic);
        tokenizer.enter(Name::TopicMarker);
        tokenizer.consume();
        tokenizer.exit(Name::TopicMarker);
        State::Next(StateName::Topic(Step::Open))
    } else {
        State::Nok
    }
}

/// After `#`, at the name.
///
/// ```markdown
/// > | a #b c
///        ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    if at(tokenizer).0 == Part::None {
        State::Nok
    } else {
        tokenizer.enter(Name::TopicText);
        State::Redo(StateName::Topic(Step::Name { word: false }))
    }
}

/// In the name, at a character; `word` is whether it holds a word
/// character.
///
/// ```markdown
/// > | a #b c
///        ^
/// ```
fn name(tokenizer: &mut Tokenizer, word: bool) -> State {
    match at(tokenizer) {
        (Part::None, _) if tokenizer.current == Some(b'/') => {
            tokenizer.check(
                State::Next(StateName::Topic(Step::Slash { word })),
                State::Next(StateName::Topic(Step::End { word })),
            );
            State::Redo(StateName::Topic(Step::SlashCheck))
        }
        (Part::None, _) => State::Redo(StateName::Topic(Step::End { word })),
        (found, size) => {
            tokenizer.consume();
            State::Next(StateName::Topic(Step::Char {
                word: word || found == Part::Word,
                left: size - 1,
            }))
        }
    }
}

/// In the bytes of a character of the name.
fn char_inside(tokenizer: &mut Tokenizer, word: bool, left: usize) -> State {
    if left == 0 {
        State::Redo(StateName::Topic(Step::Name { word }))
    } else {
        tokenizer.consume();
        State::Next(StateName::Topic(Step::Char {
            word,
            left: left - 1,
        }))
    }
}

/// At a `/`: whether a name character follows it.
///
/// ```markdown
/// > | a #b/c d
///         ^
/// ```
fn slash_check(tokenizer: &mut Tokenizer) -> State {
    tokenizer.consume();
    State::Next(StateName::Topic(Step::SlashAfter))
}

/// After a `/`, in the check.
fn slash_after(tokenizer: &mut Tokenizer) -> State {
    if at(tokenizer).0 == Part::None {
        State::Nok
    } else {
        State::Ok
    }
}

/// At a `/` that a name character follows.
fn slash(tokenizer: &mut Tokenizer, word: bool) -> State {
    tokenizer.consume();
    State::Next(StateName::Topic(Step::Name { word }))
}

/// After the name.
///
/// ```markdown
/// > | a #b c
///         ^
/// ```
fn end(tokenizer: &mut Tokenizer, word: bool) -> State {
    if !word || tokenizer.current == Some(b'#') {
        State::Nok
    } else if tokenizer.current == Some(b':') {
        tokenizer.check(State::Nok, State::Next(StateName::Topic(Step::Done)));
        State::Redo(StateName::Topic(Step::Url { size: 0 }))
    } else {
        State::Redo(StateName::Topic(Step::Done))
    }
}

/// In a `://` after the name, which makes it no topic.
fn url(tokenizer: &mut Tokenizer, size: usize) -> State {
    let expected = if size == 0 { b':' } else { b'/' };
    if tokenizer.current != Some(expected) {
        State::Nok
    } else if size == 2 {
        State::Ok
    } else {
        tokenizer.consume();
        State::Next(StateName::Topic(Step::Url { size: size + 1 }))
    }
}

/// At the end of the topic.
fn done(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::TopicText);
    tokenizer.exit(Name::Topic);
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Open,
    Name { word: bool },
    Char { word: bool, left: usize },
    SlashCheck,
    SlashAfter,
    Slash { word: bool },
    End { word: bool },
    Url { size: usize },
    Done,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Topic(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Open => open(tokenizer),
        Step::Name { word } => name(tokenizer, word),
        Step::Char { word, left } => char_inside(tokenizer, word, left),
        Step::SlashCheck => slash_check(tokenizer),
        Step::SlashAfter => slash_after(tokenizer),
        Step::Slash { word } => slash(tokenizer, word),
        Step::End { word } => end(tokenizer, word),
        Step::Url { size } => url(tokenizer, size),
        Step::Done => done(tokenizer),
    }
}
