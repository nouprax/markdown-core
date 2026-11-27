//! Attributes occur after [code (text)][raw_text], [label end][label_end],
//! [autolink][], and in [definition][].
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
//! member ::= '#' 1*name_byte | '.' 1*name_byte | '-' | record
//! ; Restriction: `-` is followed by whitespace or `}`.
//! ; Restriction: a bare name is followed by whitespace or `}`.
//! record ::= name ['=' value]
//! name ::= name_byte *name_byte
//! name_byte ::= code - space_or_tab - eol - '{' - '}' - '"' - '\'' - '='
//! value ::= '"' *(code - '"' | escape) '"' | '\'' *(code - '\'' | escape) '\'' | *(code - space_or_tab - eol - '}' | escape)
//! escape ::= '\\' ascii_punctuation
//! ```
//!
//! A name is taken as written, as in an HTML start tag: `#`, `.`, and a lone
//! `-` mean something else only as the first byte of a member.
//! A quote that does not close before the content ends starts an unquoted
//! value.
//!
//! The members are the node’s anchor (`#a`, or a record named `id`), its
//! classes (`.a`, `-` as `unnumbered`, or the run of a record named
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

use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

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
        State::Redo(StateName::Attributes(Step::Start))
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

/// Start of attributes.
///
/// ```markdown
/// > | `a`{.b}
///        ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'{') {
        tokenizer.enter(Name::Attributes);
        tokenizer.enter(Name::AttributesMarker);
        tokenizer.consume();
        tokenizer.exit(Name::AttributesMarker);
        State::Next(StateName::Attributes(Step::Between { eol: false }))
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
fn between(tokenizer: &mut Tokenizer, eol: bool) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.enter(Name::SpaceOrTab);
            tokenizer.consume();
            State::Next(StateName::Attributes(Step::Space { eol }))
        }
        Some(b'\n') if !eol => {
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            State::Next(StateName::Attributes(Step::Between { eol: true }))
        }
        Some(b'}') => {
            tokenizer.enter(Name::AttributesMarker);
            tokenizer.consume();
            tokenizer.exit(Name::AttributesMarker);
            tokenizer.exit(Name::Attributes);
            State::Next(StateName::Attributes(Step::End))
        }
        Some(b'-') => {
            tokenizer.attempt(
                State::Next(StateName::Attributes(Step::Between { eol: false })),
                State::Next(StateName::Attributes(Step::NameStart)),
            );
            State::Redo(StateName::Attributes(Step::Dash))
        }
        Some(marker @ (b'#' | b'.')) => {
            let name = if marker == b'#' {
                Name::AttributeId
            } else {
                Name::AttributeClass
            };
            tokenizer.enter(name);
            tokenizer.consume();
            State::Next(StateName::Attributes(Step::Marked { first: true }))
        }
        byte if name_byte(byte) => State::Redo(StateName::Attributes(Step::NameStart)),
        _ => State::Nok,
    }
}

/// In whitespace between members.
///
/// ```markdown
/// > | `a`{.b  .c}
///            ^
/// ```
fn space(tokenizer: &mut Tokenizer, eol: bool) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.consume();
        State::Next(StateName::Attributes(Step::Space { eol }))
    } else {
        tokenizer.exit(Name::SpaceOrTab);
        State::Redo(StateName::Attributes(Step::Between { eol }))
    }
}

/// At a `-`, which alone is a class.
///
/// ```markdown
/// > | `a`{-}
///         ^
/// ```
fn dash(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::AttributeClass);
    tokenizer.consume();
    State::Next(StateName::Attributes(Step::DashAfter))
}

/// After a `-`.
///
/// ```markdown
/// > | `a`{-}
///          ^
/// ```
fn dash_after(tokenizer: &mut Tokenizer) -> State {
    if member_end(tokenizer.current) {
        tokenizer.exit(Name::AttributeClass);
        State::Ok
    } else {
        State::Nok
    }
}

/// In the name of an identifier or a class; `first` is whether at its
/// first byte.
///
/// ```markdown
/// > | `a`{#b}
///          ^
/// ```
fn marked(tokenizer: &mut Tokenizer, first: bool) -> State {
    if name_byte(tokenizer.current) {
        tokenizer.consume();
        State::Next(StateName::Attributes(Step::Marked { first: false }))
    } else if first {
        State::Nok
    } else {
        let name = tokenizer.events[tokenizer.events.len() - 1].name;
        tokenizer.exit(name);
        State::Redo(StateName::Attributes(Step::Between { eol: false }))
    }
}

/// At the name of a record.
///
/// ```markdown
/// > | `a`{b=c}
///         ^
/// ```
fn name_start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::AttributeRecord);
    tokenizer.enter(Name::AttributeNameText);
    tokenizer.consume();
    State::Next(StateName::Attributes(Step::NameInside))
}

/// In the name of a record.
///
/// ```markdown
/// > | `a`{bc=d}
///          ^
/// ```
fn name_inside(tokenizer: &mut Tokenizer) -> State {
    if name_byte(tokenizer.current) {
        tokenizer.consume();
        return State::Next(StateName::Attributes(Step::NameInside));
    }
    tokenizer.exit(Name::AttributeNameText);
    if tokenizer.current == Some(b'=') {
        tokenizer.enter(Name::AttributeEquals);
        tokenizer.consume();
        tokenizer.exit(Name::AttributeEquals);
        State::Next(StateName::Attributes(Step::ValueStart))
    } else if member_end(tokenizer.current) {
        tokenizer.exit(Name::AttributeRecord);
        State::Redo(StateName::Attributes(Step::Between { eol: false }))
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
fn value_start(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(quote @ (b'"' | b'\'')) => {
            tokenizer.attempt(
                State::Next(StateName::Attributes(Step::ValueEnd)),
                State::Next(StateName::Attributes(Step::Unquoted)),
            );
            tokenizer.enter(Name::AttributeValue);
            tokenizer.enter(Name::AttributeValueMarker);
            tokenizer.consume();
            tokenizer.exit(Name::AttributeValueMarker);
            State::Next(StateName::Attributes(Step::Quoted { quote }))
        }
        _ => State::Redo(StateName::Attributes(Step::Unquoted)),
    }
}

/// In a quoted value, outside its text.
///
/// ```markdown
/// > | `a`{b="c"}
///            ^ ^
/// ```
fn quoted(tokenizer: &mut Tokenizer, quote: u8) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            State::Next(StateName::Attributes(Step::Quoted { quote }))
        }
        Some(byte) if byte == quote => {
            tokenizer.enter(Name::AttributeValueMarker);
            tokenizer.consume();
            tokenizer.exit(Name::AttributeValueMarker);
            State::Next(StateName::Attributes(Step::End))
        }
        _ => {
            tokenizer.enter(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::QuotedText { quote }))
        }
    }
}

/// In the text of a quoted value.
///
/// ```markdown
/// > | `a`{b="c"}
///             ^
/// ```
fn quoted_text(tokenizer: &mut Tokenizer, quote: u8) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::Quoted { quote }))
        }
        Some(byte) if byte == quote => {
            tokenizer.exit(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::Quoted { quote }))
        }
        Some(byte) => {
            tokenizer.consume();
            if byte == b'\\' {
                State::Next(StateName::Attributes(Step::QuotedEscape { quote }))
            } else {
                State::Next(StateName::Attributes(Step::QuotedText { quote }))
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
fn quoted_escape(tokenizer: &mut Tokenizer, quote: u8) -> State {
    match tokenizer.current {
        Some(byte) if byte.is_ascii_punctuation() => {
            tokenizer.consume();
            State::Next(StateName::Attributes(Step::QuotedText { quote }))
        }
        _ => State::Redo(StateName::Attributes(Step::QuotedText { quote })),
    }
}

/// At an unquoted value.
///
/// ```markdown
/// > | `a`{b=c}
///           ^
/// ```
fn unquoted(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // An empty value.
        None | Some(b'\t' | b'\n' | b' ' | b'}') => {
            tokenizer.exit(Name::AttributeRecord);
            State::Redo(StateName::Attributes(Step::Between { eol: false }))
        }
        _ => {
            tokenizer.enter(Name::AttributeValue);
            tokenizer.enter(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::UnquotedText))
        }
    }
}

/// In an unquoted value.
///
/// ```markdown
/// > | `a`{b=cd}
///            ^
/// ```
fn unquoted_text(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\t' | b'\n' | b' ' | b'}') => {
            tokenizer.exit(Name::AttributeValueText);
            State::Redo(StateName::Attributes(Step::ValueEnd))
        }
        Some(byte) => {
            tokenizer.consume();
            if byte == b'\\' {
                State::Next(StateName::Attributes(Step::UnquotedEscape))
            } else {
                State::Next(StateName::Attributes(Step::UnquotedText))
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
fn unquoted_escape(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(byte) if byte.is_ascii_punctuation() => {
            tokenizer.consume();
            State::Next(StateName::Attributes(Step::UnquotedText))
        }
        _ => State::Redo(StateName::Attributes(Step::UnquotedText)),
    }
}

/// After a value.
///
/// ```markdown
/// > | `a`{b=c d}
///            ^
/// ```
fn value_end(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::AttributeValue);
    tokenizer.exit(Name::AttributeRecord);
    State::Redo(StateName::Attributes(Step::Between { eol: false }))
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
    Start,
    Between { eol: bool },
    Space { eol: bool },
    Dash,
    DashAfter,
    Marked { first: bool },
    NameStart,
    NameInside,
    ValueStart,
    Quoted { quote: u8 },
    QuotedText { quote: u8 },
    QuotedEscape { quote: u8 },
    Unquoted,
    UnquotedText,
    UnquotedEscape,
    ValueEnd,
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
        Step::Start => start(tokenizer),
        Step::Between { eol } => between(tokenizer, eol),
        Step::Space { eol } => space(tokenizer, eol),
        Step::Dash => dash(tokenizer),
        Step::DashAfter => dash_after(tokenizer),
        Step::Marked { first } => marked(tokenizer, first),
        Step::NameStart => name_start(tokenizer),
        Step::NameInside => name_inside(tokenizer),
        Step::ValueStart => value_start(tokenizer),
        Step::Quoted { quote } => quoted(tokenizer, quote),
        Step::QuotedText { quote } => quoted_text(tokenizer, quote),
        Step::QuotedEscape { quote } => quoted_escape(tokenizer, quote),
        Step::Unquoted => unquoted(tokenizer),
        Step::UnquotedText => unquoted_text(tokenizer),
        Step::UnquotedEscape => unquoted_escape(tokenizer),
        Step::ValueEnd => value_end(tokenizer),
        Step::End => end(tokenizer),
    }
}
