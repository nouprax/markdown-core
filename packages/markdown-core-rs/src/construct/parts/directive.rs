//! Directive name and label occur in
//! [directives (text)][crate::construct::directive_text],
//! [directives (leaf)][crate::construct::directive_leaf], and
//! [directives (container)][crate::construct::directive_container].
//!
//! ## Grammar
//!
//! Directive name and label form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! name ::= 1*(byte - whitespace - '[' - '{' - ':')
//! ; Restriction: brackets in the label are balanced, at most 32 deep, and
//! ; a backslash escapes the byte after it.
//! label ::= '[' *byte ']'
//! ```
//!
//! A name is any string without whitespace: `:中文[a]` names `中文`.
//! The label’s content is text; in a block’s line it does not span lines.
//!
//! ## Tokens
//!
//! * [`DirectiveNameText`][Name::DirectiveNameText]
//! * [`DirectiveLabel`][Name::DirectiveLabel]
//! * [`DirectiveLabelMarker`][Name::DirectiveLabelMarker]
//! * [`DirectiveLabelText`][Name::DirectiveLabelText]
//!
//! ## References
//!
//! * [`micromark-extension-directive`](https://github.com/micromark/micromark-extension-directive)

use crate::construct::parts;
use crate::construct::parts::space_or_tab::space_or_tab;
use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::subtokenize::link;
use crate::tokenizer::Tokenizer;

/// The most brackets a label may hold open inside it.
const LABEL_DEPTH_MAX: usize = 32;

/// Whether `byte` ends a name.
fn name_end(byte: Option<u8>) -> bool {
    matches!(
        byte,
        None | Some(b'\t' | b'\n' | b'\r' | b' ' | b'[' | b'{' | b':')
    )
}

/// After the colon, at the name.
///
/// ```markdown
/// > | a :b[c] d
///        ^
/// ```
fn name_start(tokenizer: &mut Tokenizer) -> State {
    if name_end(tokenizer.current) {
        State::Nok
    } else {
        tokenizer.enter(Name::DirectiveNameText);
        tokenizer.consume();
        State::Next(StateName::Directive(Step::Name))
    }
}

/// In the name.
///
/// ```markdown
/// > | a :bc[d] e
///         ^
/// ```
fn name(tokenizer: &mut Tokenizer) -> State {
    if name_end(tokenizer.current) {
        tokenizer.exit(Name::DirectiveNameText);
        State::Ok
    } else {
        tokenizer.consume();
        State::Next(StateName::Directive(Step::Name))
    }
}

/// At the label’s opening bracket.
///
/// ```markdown
/// > | a :b[c] d
///         ^
/// ```
fn label_start(tokenizer: &mut Tokenizer, line: bool) -> State {
    tokenizer.enter(Name::DirectiveLabel);
    tokenizer.enter(Name::DirectiveLabelMarker);
    tokenizer.consume();
    tokenizer.exit(Name::DirectiveLabelMarker);
    State::Next(StateName::Directive(Step::LabelBefore {
        depth: 0,
        connect: false,
        line,
    }))
}

/// In the label, before a chunk of its text, or at its closing bracket;
/// `connect` is whether a chunk on an earlier line goes on in it.
///
/// ```markdown
/// > | a :b[c] d
///          ^
/// ```
fn label_before(tokenizer: &mut Tokenizer, depth: usize, connect: bool, line: bool) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b']') if depth == 0 => State::Redo(StateName::Directive(Step::LabelClose)),
        _ => {
            tokenizer.enter_link(
                Name::DirectiveLabelText,
                Link {
                    previous: None,
                    next: None,
                    content: Content::Text,
                },
            );
            if connect {
                let index = tokenizer.events.len() - 1;
                link(&mut tokenizer.events, index);
            }
            State::Redo(StateName::Directive(Step::LabelInside { depth, line }))
        }
    }
}

/// In a chunk of the label’s text.
///
/// ```markdown
/// > | a :b[c] d
///          ^
/// ```
fn label_inside(tokenizer: &mut Tokenizer, depth: usize, line: bool) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') if line => State::Nok,
        Some(b']') if depth == 0 => {
            tokenizer.exit(Name::DirectiveLabelText);
            State::Redo(StateName::Directive(Step::LabelClose))
        }
        Some(b'\n') => {
            tokenizer.consume();
            tokenizer.exit(Name::DirectiveLabelText);
            State::Next(StateName::Directive(Step::LabelBefore {
                depth,
                connect: true,
                line,
            }))
        }
        Some(b'[') if depth == LABEL_DEPTH_MAX => State::Nok,
        Some(byte) => {
            tokenizer.consume();
            State::Next(StateName::Directive(match byte {
                b'\\' => Step::LabelEscape { depth, line },
                b'[' => Step::LabelInside {
                    depth: depth + 1,
                    line,
                },
                b']' => Step::LabelInside {
                    depth: depth - 1,
                    line,
                },
                _ => Step::LabelInside { depth, line },
            }))
        }
    }
}

/// In the label, after a backslash, which escapes the byte after it.
///
/// ```markdown
/// > | a :b[c\]] d
///            ^
/// ```
fn label_escape(tokenizer: &mut Tokenizer, depth: usize, line: bool) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Redo(StateName::Directive(Step::LabelInside { depth, line })),
        _ => {
            tokenizer.consume();
            State::Next(StateName::Directive(Step::LabelInside { depth, line }))
        }
    }
}

/// At the label’s closing bracket.
///
/// ```markdown
/// > | a :b[c] d
///           ^
/// ```
fn label_close(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::DirectiveLabelMarker);
    tokenizer.consume();
    tokenizer.exit(Name::DirectiveLabelMarker);
    tokenizer.exit(Name::DirectiveLabel);
    State::Ok
}

/// After the name of a block’s directive: an optional label and attributes,
/// on the line, which must end after them.
///
/// ```markdown
/// > | ::a[b]{.c}
///        ^
/// ```
fn block_rest(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'[') {
        tokenizer.attempt(
            State::Next(StateName::Directive(Step::BlockAttributes)),
            State::Nok,
        );
        State::Redo(StateName::Directive(Step::LabelStart { line: true }))
    } else {
        State::Redo(StateName::Directive(Step::BlockAttributes))
    }
}

/// After the name and label of a block’s directive, at optional attributes.
///
/// ```markdown
/// > | ::a[b]{.c}
///           ^
/// ```
fn block_attributes(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'{') {
        tokenizer.attempt(
            State::Next(StateName::Directive(Step::BlockEnd)),
            State::Nok,
        );
        State::Redo(StateName::Attributes(parts::attributes::Step::Start {
            line: true,
        }))
    } else {
        State::Redo(StateName::Directive(Step::BlockEnd))
    }
}

/// At the end of a block’s directive, before optional spaces and tabs and
/// the end of the line.
///
/// ```markdown
/// > | ::a[b]{.c}
///               ^
/// ```
fn block_end(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::Directive(Step::BlockEol)),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::Directive(Step::BlockEol))
    }
}

/// At the end of a block’s directive line.
///
/// ```markdown
/// > | ::a[b]{.c}
///               ^
/// ```
fn block_eol(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) {
        State::Ok
    } else {
        State::Nok
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    NameStart,
    Name,
    /// `line` is whether the label is in one line, as in a block’s line.
    LabelStart {
        line: bool,
    },
    LabelBefore {
        depth: usize,
        connect: bool,
        line: bool,
    },
    LabelInside {
        depth: usize,
        line: bool,
    },
    LabelEscape {
        depth: usize,
        line: bool,
    },
    LabelClose,
    BlockRest,
    BlockAttributes,
    BlockEnd,
    BlockEol,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Directive(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::NameStart => name_start(tokenizer),
        Step::Name => name(tokenizer),
        Step::LabelStart { line } => label_start(tokenizer, line),
        Step::LabelBefore {
            depth,
            connect,
            line,
        } => label_before(tokenizer, depth, connect, line),
        Step::LabelInside { depth, line } => label_inside(tokenizer, depth, line),
        Step::LabelEscape { depth, line } => label_escape(tokenizer, depth, line),
        Step::LabelClose => label_close(tokenizer),
        Step::BlockRest => block_rest(tokenizer),
        Step::BlockAttributes => block_attributes(tokenizer),
        Step::BlockEnd => block_end(tokenizer),
        Step::BlockEol => block_eol(tokenizer),
    }
}
