//! Attributes occur after [code (text)][raw_text], [label end][label_end],
//! [autolink][], and [cross link][cross_link], in [definition][], and at the
//! end of the line of a [heading (atx)][heading_atx] and of a
//! [callout][callout] title.
//!
//! ## Grammar
//!
//! Attributes form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: whitespace holds at most one line ending.
//! ; Restriction: a quoted value closes before the content ends.
//! attributes ::= '{' *(whitespace | member) '}'
//! member ::= '#' 1*name_byte | '.' 1*name_byte | record
//! ; Restriction: a bare name is followed by whitespace or `}`.
//! record ::= name ['=' value]
//! name ::= name_byte *name_byte
//! name_byte ::= code - space_or_tab - eol - '{' - '}' - '"' - '\'' - '='
//! value ::= '"' *(code - '"' | escape) '"' | '\'' *(code - '\'' | escape) '\'' | *(code - space_or_tab - eol - '}' | escape)
//! escape ::= '\\' ascii_punctuation
//! ```
//!
//! A name is taken as written, as in an HTML start tag: `#` and `.` mean
//! something else only as the first byte of a member, and `{-}` is the
//! record `-`.
//! A quote that does not close before the content ends starts an unquoted
//! value.
//!
//! The members are the node’s anchor (`#a`, or a record named `id`), its
//! classes (`.a`, or the run of a record named
//! `class`), and its records (a bare name has the value `true`).
//! A quoted value decodes escapes and character references and reads a line
//! ending as a space; an unquoted one decodes escapes only.
//!
//! ## Tokens
//!
//! * [`Attributes`][Name::Attributes]
//! * [`AttributesMarker`][Name::AttributesMarker]
//! * [`AttributeId`][Name::AttributeId]
//! * [`AttributeClass`][Name::AttributeClass]
//! * [`AttributeRecord`][Name::AttributeRecord]
//! * [`AttributeNameText`][Name::AttributeNameText]
//! * [`AttributeEquals`][Name::AttributeEquals]
//! * [`AttributeValue`][Name::AttributeValue]
//! * [`AttributeValueMarker`][Name::AttributeValueMarker]
//! * [`AttributeValueText`][Name::AttributeValueText]
//! * [`LineEnding`][Name::LineEnding]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! [raw_text]: crate::construct::raw_text
//! [label_end]: crate::construct::label_end
//! [autolink]: crate::construct::autolink
//! [definition]: crate::construct::definition
//! [heading_atx]: crate::construct::heading_atx
//! [cross_link]: crate::construct::cross_link
//! [callout]: crate::construct::callout

use crate::construct::partial_space_or_tab::space_or_tab;
use crate::event::{Event, Name, Point};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::slice::advance;
use alloc::vec::Vec;

/// Whether `byte` continues a name.
fn name_byte(byte: Option<u8>) -> bool {
    !matches!(
        byte,
        None | Some(b'\t' | b'\n' | b'\r' | b' ' | b'{' | b'}' | b'"' | b'\'' | b'=')
    )
}

/// Whether `byte` ends a bare member: whitespace or the closing brace.
fn member_end(byte: Option<u8>) -> bool {
    matches!(byte, Some(b'\t' | b'\n' | b' ' | b'}'))
}

/// After the node `name`, which its attributes may follow: its token is
/// exited after them.
///
/// ```markdown
/// > | `a`{.b}
///        ^
/// ```
fn after(tokenizer: &mut Tokenizer, name: Name) -> State {
    if tokenizer.current == Some(b'{') {
        tokenizer.attempt(
            State::Next(StateName::Attributes(Step::Close(name))),
            State::Next(StateName::Attributes(Step::Close(name))),
        );
        State::Redo(StateName::Attributes(Step::Start { line: false }))
    } else {
        State::Redo(StateName::Attributes(Step::Close(name)))
    }
}

/// After the node `name` and its attributes.
///
/// ```markdown
/// > | `a`{.b}
///        ^      ^
/// ```
fn close(tokenizer: &mut Tokenizer, name: Name) -> State {
    tokenizer.exit(name);
    State::Ok
}

/// The attributes that end the line from `start` to `end`, after the
/// spaces and tabs before them, for a block whose text has already been
/// tokenized as a whole (the last line of a setext heading): where its text
/// ends, and the attributes’ events.
///
/// The first brace, not escaped, that starts attributes which end the line
/// is theirs.
/// Text must come before them.
pub fn line_tail(tokenizer: &Tokenizer, start: &Point, end: usize) -> Option<(Point, Vec<Event>)> {
    let bytes = tokenizer.parse_state.bytes;
    let mut at = start.index;
    while at < end {
        match bytes[at] {
            b'\\' if at + 1 < end && bytes[at + 1].is_ascii_punctuation() => {
                at += 2;
                continue;
            }
            b'{' => {
                let mut cut = at;
                while cut > start.index && matches!(bytes[cut - 1], b'\t' | b' ') {
                    cut -= 1;
                }
                if cut > start.index {
                    let mut scratch =
                        Tokenizer::new(advance(bytes, start, at), tokenizer.parse_state);
                    let state = scratch.run(
                        (at, 0),
                        (end, 0),
                        State::Next(StateName::Attributes(Step::Tail)),
                    );
                    if state == State::Ok {
                        return Some((advance(bytes, start, cut), scratch.events));
                    }
                }
            }
            _ => {}
        }
        at += 1;
    }
    None
}

/// At attributes that end a block's line, but for spaces and tabs.
///
/// ```markdown
/// > | # a {.b}
///         ^
/// ```
fn tail(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Attributes(Step::TailAfter)),
        State::Nok,
    );
    State::Redo(StateName::Attributes(Step::Start { line: true }))
}

/// After attributes that may end a block's line.
///
/// ```markdown
/// > | # a {.b}
///             ^
/// ```
fn tail_after(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::Attributes(Step::TailEnd)),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::Attributes(Step::TailEnd))
    }
}

/// At the end of a block's line, after its attributes.
///
/// ```markdown
/// > | # a {.b}
///             ^
/// ```
fn tail_end(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) {
        State::Ok
    } else {
        State::Nok
    }
}

/// Start of attributes.
///
/// ```markdown
/// > | `a`{.b}
///        ^
/// ```
fn start(tokenizer: &mut Tokenizer, line: bool) -> State {
    if tokenizer.current == Some(b'{') {
        tokenizer.enter(Name::Attributes);
        tokenizer.enter(Name::AttributesMarker);
        tokenizer.consume();
        tokenizer.exit(Name::AttributesMarker);
        State::Next(StateName::Attributes(Step::Between { eol: line, line }))
    } else {
        State::Nok
    }
}

/// Between members; `eol` is whether the whitespace holds a line ending.
///
/// ```markdown
/// > | `a`{.b .c}
///         ^ ^  ^
/// ```
fn between(tokenizer: &mut Tokenizer, eol: bool, line: bool) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.enter(Name::SpaceOrTab);
            tokenizer.consume();
            State::Next(StateName::Attributes(Step::Space { eol, line }))
        }
        Some(b'\n') if !eol => {
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            State::Next(StateName::Attributes(Step::Between { eol: true, line }))
        }
        Some(b'}') => {
            tokenizer.enter(Name::AttributesMarker);
            tokenizer.consume();
            tokenizer.exit(Name::AttributesMarker);
            tokenizer.exit(Name::Attributes);
            State::Next(StateName::Attributes(Step::End))
        }
        Some(marker @ (b'#' | b'.')) => {
            let name = if marker == b'#' {
                Name::AttributeId
            } else {
                Name::AttributeClass
            };
            tokenizer.enter(name);
            tokenizer.consume();
            State::Next(StateName::Attributes(Step::Marked { first: true, line }))
        }
        byte if name_byte(byte) => State::Redo(StateName::Attributes(Step::NameStart { line })),
        _ => State::Nok,
    }
}

/// In whitespace between members.
///
/// ```markdown
/// > | `a`{.b  .c}
///            ^
/// ```
fn space(tokenizer: &mut Tokenizer, eol: bool, line: bool) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.consume();
        State::Next(StateName::Attributes(Step::Space { eol, line }))
    } else {
        tokenizer.exit(Name::SpaceOrTab);
        State::Redo(StateName::Attributes(Step::Between { eol, line }))
    }
}

/// In the name of an identifier or a class; `first` is whether at its
/// first byte.
///
/// ```markdown
/// > | `a`{#b}
///          ^
/// ```
fn marked(tokenizer: &mut Tokenizer, first: bool, line: bool) -> State {
    if name_byte(tokenizer.current) {
        tokenizer.consume();
        State::Next(StateName::Attributes(Step::Marked { first: false, line }))
    } else if first {
        State::Nok
    } else {
        let name = tokenizer.events[tokenizer.events.len() - 1].name;
        tokenizer.exit(name);
        State::Redo(StateName::Attributes(Step::Between { eol: line, line }))
    }
}

/// At the name of a record.
///
/// ```markdown
/// > | `a`{b=c}
///         ^
/// ```
fn name_start(tokenizer: &mut Tokenizer, line: bool) -> State {
    tokenizer.enter(Name::AttributeRecord);
    tokenizer.enter(Name::AttributeNameText);
    tokenizer.consume();
    State::Next(StateName::Attributes(Step::NameInside { line }))
}

/// In the name of a record.
///
/// ```markdown
/// > | `a`{bc=d}
///          ^
/// ```
fn name_inside(tokenizer: &mut Tokenizer, line: bool) -> State {
    if name_byte(tokenizer.current) {
        tokenizer.consume();
        return State::Next(StateName::Attributes(Step::NameInside { line }));
    }
    tokenizer.exit(Name::AttributeNameText);
    if tokenizer.current == Some(b'=') {
        tokenizer.enter(Name::AttributeEquals);
        tokenizer.consume();
        tokenizer.exit(Name::AttributeEquals);
        State::Next(StateName::Attributes(Step::ValueStart { line }))
    } else if member_end(tokenizer.current) {
        tokenizer.exit(Name::AttributeRecord);
        State::Redo(StateName::Attributes(Step::Between { eol: line, line }))
    } else {
        State::Nok
    }
}

/// At a value.
///
/// ```markdown
/// > | `a`{b=c}
///           ^
/// ```
fn value_start(tokenizer: &mut Tokenizer, line: bool) -> State {
    match tokenizer.current {
        Some(quote @ (b'"' | b'\'')) => {
            tokenizer.attempt(
                State::Next(StateName::Attributes(Step::ValueEnd { line })),
                State::Next(StateName::Attributes(Step::Unquoted { line })),
            );
            tokenizer.enter(Name::AttributeValue);
            tokenizer.enter(Name::AttributeValueMarker);
            tokenizer.consume();
            tokenizer.exit(Name::AttributeValueMarker);
            State::Next(StateName::Attributes(Step::Quoted { quote, line }))
        }
        _ => State::Redo(StateName::Attributes(Step::Unquoted { line })),
    }
}

/// In a quoted value, outside its text.
///
/// ```markdown
/// > | `a`{b="c"}
///            ^ ^
/// ```
fn quoted(tokenizer: &mut Tokenizer, quote: u8, line: bool) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') if line => State::Nok,
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            State::Next(StateName::Attributes(Step::Quoted { quote, line }))
        }
        Some(byte) if byte == quote => {
            tokenizer.enter(Name::AttributeValueMarker);
            tokenizer.consume();
            tokenizer.exit(Name::AttributeValueMarker);
            State::Next(StateName::Attributes(Step::End))
        }
        _ => {
            tokenizer.enter(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::QuotedText { quote, line }))
        }
    }
}

/// In the text of a quoted value.
///
/// ```markdown
/// > | `a`{b="c"}
///             ^
/// ```
fn quoted_text(tokenizer: &mut Tokenizer, quote: u8, line: bool) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::Quoted { quote, line }))
        }
        Some(byte) if byte == quote => {
            tokenizer.exit(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::Quoted { quote, line }))
        }
        Some(byte) => {
            tokenizer.consume();
            if byte == b'\\' {
                State::Next(StateName::Attributes(Step::QuotedEscape { quote, line }))
            } else {
                State::Next(StateName::Attributes(Step::QuotedText { quote, line }))
            }
        }
    }
}

/// After a backslash in the text of a quoted value.
///
/// ```markdown
/// > | `a`{b="\""}
///              ^
/// ```
fn quoted_escape(tokenizer: &mut Tokenizer, quote: u8, line: bool) -> State {
    match tokenizer.current {
        Some(byte) if byte.is_ascii_punctuation() => {
            tokenizer.consume();
            State::Next(StateName::Attributes(Step::QuotedText { quote, line }))
        }
        _ => State::Redo(StateName::Attributes(Step::QuotedText { quote, line })),
    }
}

/// At an unquoted value.
///
/// ```markdown
/// > | `a`{b=c}
///           ^
/// ```
fn unquoted(tokenizer: &mut Tokenizer, line: bool) -> State {
    match tokenizer.current {
        // An empty value.
        None | Some(b'\t' | b'\n' | b' ' | b'}') => {
            tokenizer.exit(Name::AttributeRecord);
            State::Redo(StateName::Attributes(Step::Between { eol: line, line }))
        }
        _ => {
            tokenizer.enter(Name::AttributeValue);
            tokenizer.enter(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::UnquotedText { line }))
        }
    }
}

/// In an unquoted value.
///
/// ```markdown
/// > | `a`{b=cd}
///            ^
/// ```
fn unquoted_text(tokenizer: &mut Tokenizer, line: bool) -> State {
    match tokenizer.current {
        None | Some(b'\t' | b'\n' | b' ' | b'}') => {
            tokenizer.exit(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::ValueEnd { line }))
        }
        Some(byte) => {
            tokenizer.consume();
            if byte == b'\\' {
                State::Next(StateName::Attributes(Step::UnquotedEscape { line }))
            } else {
                State::Next(StateName::Attributes(Step::UnquotedText { line }))
            }
        }
    }
}

/// After a backslash in an unquoted value.
///
/// ```markdown
/// > | `a`{b=\}}
///            ^
/// ```
fn unquoted_escape(tokenizer: &mut Tokenizer, line: bool) -> State {
    match tokenizer.current {
        Some(byte) if byte.is_ascii_punctuation() => {
            tokenizer.consume();
            State::Next(StateName::Attributes(Step::UnquotedText { line }))
        }
        _ => State::Redo(StateName::Attributes(Step::UnquotedText { line })),
    }
}

/// After a value.
///
/// ```markdown
/// > | `a`{b=c d}
///            ^
/// ```
fn value_end(tokenizer: &mut Tokenizer, line: bool) -> State {
    tokenizer.exit(Name::AttributeValue);
    tokenizer.exit(Name::AttributeRecord);
    State::Redo(StateName::Attributes(Step::Between { eol: line, line }))
}

/// After the closing brace, or the closing quote of a value.
fn end(_tokenizer: &mut Tokenizer) -> State {
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    After(Name),
    Close(Name),
    Tail,
    TailAfter,
    TailEnd,
    /// `line` is whether the attributes are on one line: whether no line
    /// ending may come in them, as in a block's line.
    Start {
        line: bool,
    },
    Between {
        eol: bool,
        line: bool,
    },
    Space {
        eol: bool,
        line: bool,
    },
    Marked {
        first: bool,
        line: bool,
    },
    NameStart {
        line: bool,
    },
    NameInside {
        line: bool,
    },
    ValueStart {
        line: bool,
    },
    Quoted {
        quote: u8,
        line: bool,
    },
    QuotedText {
        quote: u8,
        line: bool,
    },
    QuotedEscape {
        quote: u8,
        line: bool,
    },
    Unquoted {
        line: bool,
    },
    UnquotedText {
        line: bool,
    },
    UnquotedEscape {
        line: bool,
    },
    ValueEnd {
        line: bool,
    },
    End,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Attributes(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::After(name) => after(tokenizer, name),
        Step::Close(name) => close(tokenizer, name),
        Step::Tail => tail(tokenizer),
        Step::TailAfter => tail_after(tokenizer),
        Step::TailEnd => tail_end(tokenizer),
        Step::Start { line } => start(tokenizer, line),
        Step::Between { eol, line } => between(tokenizer, eol, line),
        Step::Space { eol, line } => space(tokenizer, eol, line),
        Step::Marked { first, line } => marked(tokenizer, first, line),
        Step::NameStart { line } => name_start(tokenizer, line),
        Step::NameInside { line } => name_inside(tokenizer, line),
        Step::ValueStart { line } => value_start(tokenizer, line),
        Step::Quoted { quote, line } => quoted(tokenizer, quote, line),
        Step::QuotedText { quote, line } => quoted_text(tokenizer, quote, line),
        Step::QuotedEscape { quote, line } => quoted_escape(tokenizer, quote, line),
        Step::Unquoted { line } => unquoted(tokenizer, line),
        Step::UnquotedText { line } => unquoted_text(tokenizer, line),
        Step::UnquotedEscape { line } => unquoted_escape(tokenizer, line),
        Step::ValueEnd { line } => value_end(tokenizer, line),
        Step::End => end(tokenizer),
    }
}
