//! Label occurs in [definition][] and [label end][label_end].
//!
//! ## Grammar
//!
//! Label forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: maximum `999` codes allowed between brackets.
//! ; Restriction: no blank lines.
//! ; Restriction: at least 1 `text` byte must exist.
//! label ::= '[' *(label_byte | label_escape) ']'
//! label_byte ::= code - '[' - '\\' - ']'
//! label_escape ::= '\\' ['[' | '\\' | ']']
//! ```
//!
//! The maximum allowed size of the label, without the brackets, is `999`
//! (inclusive), which is defined in
//! [`LINK_REFERENCE_SIZE_MAX`][].
//!
//! Labels can contain line endings and whitespace, but they are not allowed to
//! contain blank lines, and they must not be blank themselves.
//!
//! The label is interpreted as the [string][string] content type.
//! That means that [character escapes][character_escape] and
//! [character references][character_reference] are allowed.
//!
//! > 👉 **Note**: this label relates to, but is not, the initial “label” of
//! > what is know as a reference in markdown:
//! >
//! > | Kind      | Link     | Image     |
//! > | --------- | -------- | --------- |
//! > | Shortcut  | `[x]`    | `![x]`    |
//! > | Collapsed | `[x][]`  | `![x][]`  |
//! > | Full      | `[x][y]` | `![x][y]` |
//! >
//! > The 6 above things are references, in the three kinds they come in, as
//! > links and images.
//! > The label that this module focusses on is only the thing that contains
//! > `y`.
//! >
//! > The thing that contains `x` is not a single thing when parsing markdown,
//! > but instead constists of an opening
//! > ([label start (image)][label_start_image] or
//! > [label start (link)][label_start_link]) and a closing
//! > ([label end][label_end]), so as to allow further phrasing such as
//! > [code (text)][raw_text] or [attention][].
//!
//! ## References
//!
//! * [`micromark-factory-label/index.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-factory-label/dev/index.js)
//!
//! [definition]: crate::construct::definition
//! [plain]: crate::construct::plain
//! [attention]: crate::construct::attention
//! [character_escape]: crate::construct::character_escape
//! [character_reference]: crate::construct::character_reference
//! [label_start_picture]: crate::construct::label_start_picture
//! [label_start_link]: crate::construct::label_start_link
//! [label_end]: crate::construct::label_end
//! [raw_text]: crate::construct::raw_text
//! [link_reference_size_max]: crate::util::constant::LINK_REFERENCE_SIZE_MAX

use crate::construct::partial_space_or_tab_eol::{space_or_tab_eol_with_options, Options};
use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::subtokenize::link;
use crate::tokenizer::Tokenizer;
use crate::util::constant::LINK_REFERENCE_SIZE_MAX;

/// Event names a caller gives a label.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Names {
    /// Whole label.
    pub whole: Name,
    /// Opening and closing marker.
    pub marker: Name,
    /// Text between the markers.
    pub text: Name,
}

/// Label being read.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Label {
    names: Names,
    /// Whether data before connects to the next data.
    connect: bool,
    /// Whether a non-whitespace byte was seen.
    seen: bool,
    /// Bytes inside so far.
    size: usize,
}

/// Start of label.
///
/// ```markdown
/// > | [a]
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer, names: Names) -> State {
    debug_assert_eq!(tokenizer.current, Some(b'['), "expected `[`");
    tokenizer.enter(names.whole);
    tokenizer.enter(names.marker);
    tokenizer.consume();
    tokenizer.exit(names.marker);
    tokenizer.enter(names.text);
    State::Next(StateName::Label(Step::AtBreak(Label {
        names,
        connect: false,
        seen: false,
        size: 0,
    })))
}

/// In label, at something, before something else.
///
/// ```markdown
/// > | [a]
///      ^
/// ```
fn at_break(tokenizer: &mut Tokenizer, mut label: Label) -> State {
    if label.size > LINK_REFERENCE_SIZE_MAX
        || matches!(tokenizer.current, None | Some(b'['))
        || (matches!(tokenizer.current, Some(b']')) && !label.seen)
    {
        State::Nok
    } else {
        match tokenizer.current {
            Some(b'\n') => {
                tokenizer.attempt(
                    State::Next(StateName::Label(Step::EolAfter(label))),
                    State::Nok,
                );
                State::Redo(space_or_tab_eol_with_options(Options {
                    content: Some(Content::String),
                    connect: label.connect,
                }))
            }
            Some(b']') => {
                tokenizer.exit(label.names.text);
                tokenizer.enter(label.names.marker);
                tokenizer.consume();
                tokenizer.exit(label.names.marker);
                tokenizer.exit(label.names.whole);
                State::Ok
            }
            _ => {
                tokenizer.enter_link(
                    Name::Data,
                    Link {
                        previous: None,
                        next: None,
                        content: Content::String,
                    },
                );

                if label.connect {
                    let index = tokenizer.events.len() - 1;
                    link(&mut tokenizer.events, index);
                } else {
                    label.connect = true;
                }

                State::Redo(StateName::Label(Step::Inside(label)))
            }
        }
    }
}

/// In label, after whitespace.
///
/// ```markdown
///   | [a␊
/// > | b]
///     ^
/// ```
fn eol_after(mut label: Label) -> State {
    label.connect = true;
    State::Redo(StateName::Label(Step::AtBreak(label)))
}

/// In label, in text.
///
/// ```markdown
/// > | [a]
///      ^
/// ```
fn inside(tokenizer: &mut Tokenizer, mut label: Label) -> State {
    match tokenizer.current {
        None | Some(b'\n' | b'[' | b']') => {
            tokenizer.exit(Name::Data);
            State::Redo(StateName::Label(Step::AtBreak(label)))
        }
        Some(code) => {
            if label.size > LINK_REFERENCE_SIZE_MAX {
                tokenizer.exit(Name::Data);
                State::Redo(StateName::Label(Step::AtBreak(label)))
            } else {
                tokenizer.consume();
                label.size += 1;
                if !matches!(code, b'\t' | b' ') {
                    label.seen = true;
                }
                State::Next(StateName::Label(if code == b'\\' {
                    Step::Escape(label)
                } else {
                    Step::Inside(label)
                }))
            }
        }
    }
}

/// After `\`, at a special character.
///
/// ```markdown
/// > | [a\*a]
///        ^
/// ```
fn escape(tokenizer: &mut Tokenizer, mut label: Label) -> State {
    match tokenizer.current {
        Some(b'[' | b'\\' | b']') => {
            tokenizer.consume();
            label.size += 1;
            State::Next(StateName::Label(Step::Inside(label)))
        }
        _ => State::Redo(StateName::Label(Step::Inside(label))),
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start(Names),
    AtBreak(Label),
    EolAfter(Label),
    Escape(Label),
    Inside(Label),
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Label(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start(names) => start(tokenizer, names),
        Step::AtBreak(label) => at_break(tokenizer, label),
        Step::EolAfter(label) => eol_after(label),
        Step::Escape(label) => escape(tokenizer, label),
        Step::Inside(label) => inside(tokenizer, label),
    }
}
