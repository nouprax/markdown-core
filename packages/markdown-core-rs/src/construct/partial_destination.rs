//! Destination occurs in [definition][] and [label end][label_end].
//!
//! ## Grammar
//!
//! Destination forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! destination ::= destination_enclosed | destination_raw
//!
//! destination_enclosed ::= '<' *(destination_enclosed_byte | destination_enclosed_escape) '>'
//! destination_enclosed_byte ::= line - '<' - '\\' - '>'
//! destination_enclosed_escape ::= '\\' ['<' | '\\' | '>']
//!
//! destination_raw ::= 1*(destination_raw_byte | destination_raw_escape)
//! ; Restriction: unbalanced `)` characters are not allowed.
//! destination_raw_byte ::= text - '\\' - ascii_control
//! destination_raw_escape ::= '\\' ['(' | ')' | '\\']
//! ```
//!
//! Balanced parens allowed in raw destinations.
//! They are counted with a counter that starts at `0`, and is incremented
//! every time `(` occurs and decremented every time `)` occurs.
//! If `)` is found when the counter is `0`, the destination closes immediately
//! before it.
//! Escaped parens do not count in balancing.
//!
//! The destination is interpreted as the [string][plain] content type.
//! That means that [character escapes][character_escape] and
//! [character references][character_reference] are allowed.
//!
//! The grammar for enclosed destinations (`<x>`) prohibits the use of `<`,
//! `>`, and line endings to form URLs.
//! The angle brackets can be encoded as a character reference, character
//! escape, or percent encoding:
//!
//! * `<` as `&lt;`, `\<`, or `%3c`
//! * `>` as `&gt;`, `\>`, or `%3e`
//!
//! The grammar for raw destinations (`x`) prohibits space (` `) and all
//! [ASCII control][u8::is_ascii_control] characters, which thus must be
//! encoded.
//! Unbalanced parens can be encoded as a character reference, character escape,
//! or percent encoding:
//!
//! * `(` as `&lpar;`, `\(`, or `%28`
//! * `)` as `&rpar;`, `\)`, or `%29`
//!
//! There are several cases where incorrect encoding of URLs would, in other
//! languages, result in a parse error.
//! In markdown, there are no errors, and URLs are normalized.
//! In addition, unicode characters are percent encoded
//! (`sanitize_uri` of markdown-rs).
//! For example:
//!
//! ```markdown
//! [x]
//!
//! [x]: <https://a👍b%>
//! ```
//!
//! Yields:
//!
//! ```html
//! <p><a href="https://a%F0%9F%91%8Db%25">x</a></p>
//! ```
//!
//! ## Recommendation
//!
//! It is recommended to use the enclosed variant of destinations, as it allows
//! the most characters, including arbitrary parens, in URLs.
//!
//! ## References
//!
//! * [`micromark-factory-destination/index.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-factory-destination/dev/index.js)
//!
//! [definition]: crate::construct::definition
//! [plain]: crate::construct::plain
//! [character_escape]: crate::construct::character_escape
//! [character_reference]: crate::construct::character_reference
//! [label_end]: crate::construct::label_end

use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Event names a caller gives a destination.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Names {
    /// Whole destination.
    pub whole: Name,
    /// Enclosed (`<a>`) destination.
    pub literal: Name,
    /// `<` and `>` of an enclosed destination.
    pub marker: Name,
    /// Raw (`a`) destination.
    pub raw: Name,
    /// Text of either kind.
    pub text: Name,
}

/// Options a caller gives a destination.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Options {
    /// Event names.
    pub names: Names,
    /// Most unbalanced `(` allowed in a raw destination.
    pub limit: usize,
}

/// Start of destination.
///
/// ```markdown
/// > | <aa>
///     ^
/// > | aa
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer, options: Options) -> State {
    let names = options.names;
    match tokenizer.current {
        Some(b'<') => {
            tokenizer.enter(names.whole);
            tokenizer.enter(names.literal);
            tokenizer.enter(names.marker);
            tokenizer.consume();
            tokenizer.exit(names.marker);
            State::Next(StateName::Destination(Step::EnclosedBefore(names)))
        }
        // ASCII control, space, closing paren, but *not* `\0`.
        None | Some(0x01..=0x1F | b' ' | b')' | 0x7F) => State::Nok,
        Some(_) => {
            tokenizer.enter(names.whole);
            tokenizer.enter(names.raw);
            tokenizer.enter(names.text);
            tokenizer.enter_link(
                Name::Data,
                Link {
                    previous: None,
                    next: None,
                    content: Content::Plain,
                },
            );
            State::Redo(StateName::Destination(Step::Raw(options, 0)))
        }
    }
}

/// After `<`, at an enclosed destination.
///
/// ```markdown
/// > | <aa>
///      ^
/// ```
fn enclosed_before(tokenizer: &mut Tokenizer, names: Names) -> State {
    if let Some(b'>') = tokenizer.current {
        tokenizer.enter(names.marker);
        tokenizer.consume();
        tokenizer.exit(names.marker);
        tokenizer.exit(names.literal);
        tokenizer.exit(names.whole);
        State::Ok
    } else {
        tokenizer.enter(names.text);
        tokenizer.enter_link(
            Name::Data,
            Link {
                previous: None,
                next: None,
                content: Content::Plain,
            },
        );
        State::Redo(StateName::Destination(Step::Enclosed(names)))
    }
}

/// In enclosed destination.
///
/// ```markdown
/// > | <aa>
///      ^
/// ```
fn enclosed(tokenizer: &mut Tokenizer, names: Names) -> State {
    match tokenizer.current {
        None | Some(b'\n' | b'<') => State::Nok,
        Some(b'>') => {
            tokenizer.exit(Name::Data);
            tokenizer.exit(names.text);
            State::Redo(StateName::Destination(Step::EnclosedBefore(names)))
        }
        Some(b'\\') => {
            tokenizer.consume();
            State::Next(StateName::Destination(Step::EnclosedEscape(names)))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::Destination(Step::Enclosed(names)))
        }
    }
}

/// After `\`, at a special character.
///
/// ```markdown
/// > | <a\*a>
///        ^
/// ```
fn enclosed_escape(tokenizer: &mut Tokenizer, names: Names) -> State {
    match tokenizer.current {
        Some(b'<' | b'>' | b'\\') => {
            tokenizer.consume();
            State::Next(StateName::Destination(Step::Enclosed(names)))
        }
        _ => State::Redo(StateName::Destination(Step::Enclosed(names))),
    }
}

/// In raw destination, with `open` unbalanced `(`.
///
/// ```markdown
/// > | aa
///     ^
/// ```
fn raw(tokenizer: &mut Tokenizer, options: Options, open: usize) -> State {
    if open == 0 && matches!(tokenizer.current, None | Some(b'\t' | b'\n' | b' ' | b')')) {
        tokenizer.exit(Name::Data);
        tokenizer.exit(options.names.text);
        tokenizer.exit(options.names.raw);
        tokenizer.exit(options.names.whole);
        State::Ok
    } else if open < options.limit && tokenizer.current == Some(b'(') {
        tokenizer.consume();
        State::Next(StateName::Destination(Step::Raw(options, open + 1)))
    } else if tokenizer.current == Some(b')') {
        tokenizer.consume();
        State::Next(StateName::Destination(Step::Raw(options, open - 1)))
    }
    // ASCII control (but *not* `\0`) and space and `(`.
    else if matches!(
        tokenizer.current,
        None | Some(0x01..=0x1F | b' ' | b'(' | 0x7F)
    ) {
        State::Nok
    } else if tokenizer.current == Some(b'\\') {
        tokenizer.consume();
        State::Next(StateName::Destination(Step::RawEscape(options, open)))
    } else {
        tokenizer.consume();
        State::Next(StateName::Destination(Step::Raw(options, open)))
    }
}

/// After `\`, at special character.
///
/// ```markdown
/// > | a\*a
///       ^
/// ```
fn raw_escape(tokenizer: &mut Tokenizer, options: Options, open: usize) -> State {
    match tokenizer.current {
        Some(b'(' | b')' | b'\\') => {
            tokenizer.consume();
            State::Next(StateName::Destination(Step::Raw(options, open)))
        }
        _ => State::Redo(StateName::Destination(Step::Raw(options, open))),
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start(Options),
    EnclosedBefore(Names),
    Enclosed(Names),
    EnclosedEscape(Names),
    Raw(Options, usize),
    RawEscape(Options, usize),
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Destination(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start(options) => start(tokenizer, options),
        Step::EnclosedBefore(names) => enclosed_before(tokenizer, names),
        Step::Enclosed(names) => enclosed(tokenizer, names),
        Step::EnclosedEscape(names) => enclosed_escape(tokenizer, names),
        Step::Raw(options, open) => raw(tokenizer, options, open),
        Step::RawEscape(options, open) => raw_escape(tokenizer, options, open),
    }
}
