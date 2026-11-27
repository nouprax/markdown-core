//! Title occurs in [definition][] and [label end][label_end].
//!
//! ## Grammar
//!
//! Title forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: no blank lines.
//! ; Restriction: markers must match (in case of `(` with `)`).
//! title ::= marker *(title_byte | title_escape) marker
//! title_byte ::= code - '\\' - marker
//! title_escape ::= '\\' ['\\' | marker]
//! marker ::= '"' | '\'' | '('
//! ```
//!
//! Titles can be double quoted (`"a"`), single quoted (`'a'`), or
//! parenthesized (`(a)`).
//!
//! Titles can contain line endings and whitespace, but they are not allowed to
//! contain blank lines.
//! They are allowed to be blank themselves.
//!
//! The title is interpreted as the [string][plain] content type.
//! That means that [character escapes][character_escape] and
//! [character references][character_reference] are allowed.
//!
//! ## References
//!
//! * [`micromark-factory-title/index.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-factory-title/dev/index.js)
//!
//! [definition]: crate::construct::definition
//! [plain]: crate::construct::plain
//! [character_escape]: crate::construct::character_escape
//! [character_reference]: crate::construct::character_reference
//! [label_end]: crate::construct::label_end

use crate::construct::partial_space_or_tab_eol::{space_or_tab_eol_with_options, Options};
use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::subtokenize::link;
use crate::tokenizer::Tokenizer;

/// Event names a caller gives a title.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Names {
    /// Whole title.
    pub whole: Name,
    /// Opening and closing marker.
    pub marker: Name,
    /// Text between the markers.
    pub text: Name,
}

/// Title being read.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Title {
    names: Names,
    /// Closing marker.
    close: u8,
    /// Whether data before connects to the next data.
    connect: bool,
}

/// Start of title.
///
/// ```markdown
/// > | "a"
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer, names: Names) -> State {
    match tokenizer.current {
        Some(marker @ (b'"' | b'\'' | b'(')) => {
            tokenizer.enter(names.whole);
            tokenizer.enter(names.marker);
            tokenizer.consume();
            tokenizer.exit(names.marker);
            State::Next(StateName::Title(Step::Lead(Title {
                names,
                close: if marker == b'(' { b')' } else { marker },
                connect: false,
            })))
        }
        _ => State::Nok,
    }
}

/// After opening marker.
///
/// This is also used at the closing marker.
///
/// ```markdown
/// > | "a"
///      ^
/// ```
fn lead(tokenizer: &mut Tokenizer, title: Title) -> State {
    if tokenizer.current == Some(title.close) {
        tokenizer.enter(title.names.marker);
        tokenizer.consume();
        tokenizer.exit(title.names.marker);
        tokenizer.exit(title.names.whole);
        State::Ok
    } else {
        tokenizer.enter(title.names.text);
        State::Redo(StateName::Title(Step::AtBreak(title)))
    }
}

/// At something, before something else.
///
/// ```markdown
/// > | "a"
///      ^
/// ```
fn at_break(tokenizer: &mut Tokenizer, mut title: Title) -> State {
    if let Some(code) = tokenizer.current {
        if code == title.close {
            tokenizer.exit(title.names.text);
            State::Redo(StateName::Title(Step::Lead(title)))
        } else if code == b'\n' {
            tokenizer.attempt(
                State::Next(StateName::Title(Step::AfterEol(title))),
                State::Nok,
            );
            State::Redo(space_or_tab_eol_with_options(Options {
                content: Some(Content::Plain),
                connect: title.connect,
            }))
        } else {
            tokenizer.enter_link(
                Name::Data,
                Link {
                    previous: None,
                    next: None,
                    content: Content::Plain,
                },
            );

            if title.connect {
                let index = tokenizer.events.len() - 1;
                link(&mut tokenizer.events, index);
            } else {
                title.connect = true;
            }

            State::Redo(StateName::Title(Step::Inside(title)))
        }
    } else {
        State::Nok
    }
}

/// In title, after whitespace.
///
/// ```markdown
///   | "a␊
/// > | b"
///     ^
/// ```
fn after_eol(mut title: Title) -> State {
    title.connect = true;
    State::Redo(StateName::Title(Step::AtBreak(title)))
}

/// In text.
///
/// ```markdown
/// > | "a"
///      ^
/// ```
fn inside(tokenizer: &mut Tokenizer, title: Title) -> State {
    if tokenizer.current == Some(title.close) || matches!(tokenizer.current, None | Some(b'\n')) {
        tokenizer.exit(Name::Data);
        State::Redo(StateName::Title(Step::AtBreak(title)))
    } else {
        let next = if tokenizer.current == Some(b'\\') {
            Step::Escape(title)
        } else {
            Step::Inside(title)
        };
        tokenizer.consume();
        State::Next(StateName::Title(next))
    }
}

/// After `\`, at a special character.
///
/// ```markdown
/// > | "a\*b"
///      ^
/// ```
fn escape(tokenizer: &mut Tokenizer, title: Title) -> State {
    match tokenizer.current {
        Some(b'"' | b'\'' | b')' | b'\\') => {
            tokenizer.consume();
            State::Next(StateName::Title(Step::Inside(title)))
        }
        _ => State::Redo(StateName::Title(Step::Inside(title))),
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start(Names),
    Lead(Title),
    AfterEol(Title),
    AtBreak(Title),
    Escape(Title),
    Inside(Title),
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Title(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start(names) => start(tokenizer, names),
        Step::Lead(title) => lead(tokenizer, title),
        Step::AfterEol(title) => after_eol(title),
        Step::AtBreak(title) => at_break(tokenizer, title),
        Step::Escape(title) => escape(tokenizer, title),
        Step::Inside(title) => inside(tokenizer, title),
    }
}
