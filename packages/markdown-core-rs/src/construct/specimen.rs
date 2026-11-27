//! Specimen (a numbered example) occurs in the [document][] content type.
//!
//! ## Grammar
//!
//! Specimens form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: the start is not zero.
//! ; Restriction: whitespace or a line ending follows the `)`.
//! specimen_start ::= 0*3space_or_tab '(' [1*9ascii_digit] '@' [label] ')' *space_or_tab
//! label ::= 1*key_char *('-' 1*key_char)
//!
//! ; Restriction: blank line allowed.
//! specimen_cont ::= 4(space_or_tab)
//! ```
//!
//! A key character is a letter, a number, or `_`, by Unicode category, as in
//! a [citation key][citation]: a label is what a bare reference reads whole,
//! so a hyphen stands singly between key characters, and `(@a--b)` is text.
//!
//! A specimen never interrupts a paragraph.
//! Its label, as written, is a definition that citations of it resolve to
//! in the whole document.
//! A start counts only on the first specimen of a group: one that a
//! specimen comes right before, blank lines aside, has none.
//!
//! ## Tokens
//!
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//! * [`Specimen`][Name::Specimen]
//! * [`SpecimenLabel`][Name::SpecimenLabel]
//! * [`SpecimenMarker`][Name::SpecimenMarker]
//! * [`SpecimenPrefix`][Name::SpecimenPrefix]
//! * [`SpecimenStart`][Name::SpecimenStart]
//!
//! ## References
//!
//! * [*Numbered example lists* in Pandoc’s manual](https://pandoc.org/MANUAL.html#numbered-example-lists)
//!
//! [document]: crate::construct::document
//! [citation]: crate::construct::citation

use crate::construct::blank_line;
use crate::construct::citation::key_char;
use crate::construct::partial_space_or_tab::{
    space_or_tab_min_max, space_or_tab_with_options, Options as SpaceOrTabOptions,
};
use crate::event::{Kind, Name};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::char::after_index;
use crate::util::constant::TAB_SIZE;
use alloc::string::String;

/// Start of a specimen.
///
/// ```markdown
/// > | (@a) b
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.interrupt {
        return State::Nok;
    }
    tokenizer.enter(Name::Specimen);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(State::Next(StateName::Specimen(Step::Open)), State::Nok);
        State::Redo(space_or_tab_min_max(1, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::Specimen(Step::Open))
    }
}

/// At the `(`.
///
/// ```markdown
/// > | (@a) b
///     ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current != Some(b'(') {
        return State::Nok;
    }
    tokenizer.enter(Name::SpecimenPrefix);
    tokenizer.enter(Name::SpecimenMarker);
    tokenizer.consume();
    tokenizer.exit(Name::SpecimenMarker);
    State::Next(StateName::Specimen(Step::Before))
}

/// After the `(`, at the start or the `@`.
///
/// ```markdown
/// > | (5@) b
///      ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'0'..=b'9')) {
        tokenizer.enter(Name::SpecimenStart);
        State::Redo(StateName::Specimen(Step::Number { size: 0 }))
    } else {
        State::Redo(StateName::Specimen(Step::At))
    }
}

/// In the start: one to nine digits, not all zero.
///
/// ```markdown
/// > | (5@) b
///      ^
/// ```
fn number(tokenizer: &mut Tokenizer, size: usize) -> State {
    match tokenizer.current {
        Some(b'0'..=b'9') if size < 9 => {
            tokenizer.consume();
            State::Next(StateName::Specimen(Step::Number { size: size + 1 }))
        }
        _ => {
            let from = tokenizer.events[tokenizer.events.len() - 1].point.index;
            let text = &tokenizer.parse_state.bytes[from..tokenizer.point.index];
            if text.iter().all(|byte| *byte == b'0') {
                return State::Nok;
            }
            tokenizer.exit(Name::SpecimenStart);
            State::Redo(StateName::Specimen(Step::At))
        }
    }
}

/// At the `@`.
///
/// ```markdown
/// > | (@a) b
///      ^
/// ```
fn at(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current != Some(b'@') {
        return State::Nok;
    }
    tokenizer.enter(Name::SpecimenMarker);
    tokenizer.consume();
    tokenizer.exit(Name::SpecimenMarker);
    State::Next(StateName::Specimen(Step::LabelBefore))
}

/// After the `@`, at the label or the `)`.
///
/// ```markdown
/// > | (@a) b
///       ^
/// ```
fn label_before(tokenizer: &mut Tokenizer) -> State {
    if key_width(tokenizer) > 0 {
        tokenizer.enter(Name::SpecimenLabel);
        State::Redo(StateName::Specimen(Step::Label { left: 0 }))
    } else {
        State::Redo(StateName::Specimen(Step::Close))
    }
}

/// The width of the key character at the cursor, or 0.
fn key_width(tokenizer: &Tokenizer) -> usize {
    let char = after_index(tokenizer.parse_state.bytes, tokenizer.point.index);
    if key_char(char) {
        char.map_or(0, char::len_utf8)
    } else {
        0
    }
}

/// In the label, with `left` bytes of a key character still to go.
///
/// ```markdown
/// > | (@a-b) c
///       ^^^
/// ```
fn label(tokenizer: &mut Tokenizer, left: usize) -> State {
    if left > 0 {
        tokenizer.consume();
        return State::Next(StateName::Specimen(Step::Label { left: left - 1 }));
    }
    let width = key_width(tokenizer);
    if width > 0 {
        tokenizer.consume();
        return State::Next(StateName::Specimen(Step::Label { left: width - 1 }));
    }
    if tokenizer.current == Some(b'-') {
        tokenizer.consume();
        return State::Next(StateName::Specimen(Step::Dash));
    }
    tokenizer.exit(Name::SpecimenLabel);
    State::Redo(StateName::Specimen(Step::Close))
}

/// After a `-` in the label, which a key character must follow.
///
/// ```markdown
/// > | (@a-b) c
///         ^
/// ```
fn dash(tokenizer: &mut Tokenizer) -> State {
    let width = key_width(tokenizer);
    if width > 0 {
        tokenizer.consume();
        State::Next(StateName::Specimen(Step::Label { left: width - 1 }))
    } else {
        State::Nok
    }
}

/// At the `)`, which whitespace or a line ending follows.
///
/// ```markdown
/// > | (@a) b
///        ^
/// ```
fn close(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current != Some(b')') {
        return State::Nok;
    }
    tokenizer.enter(Name::SpecimenMarker);
    tokenizer.consume();
    tokenizer.exit(Name::SpecimenMarker);
    State::Next(StateName::Specimen(Step::CloseAfter))
}

/// After the `)`, at the whitespace or line ending it needs.
///
/// ```markdown
/// > | (@a) b
///         ^
/// ```
fn close_after(tokenizer: &mut Tokenizer) -> State {
    if !matches!(tokenizer.current, None | Some(b'\t' | b'\n' | b'\r' | b' ')) {
        return State::Nok;
    }
    tokenizer.attempt(State::Next(StateName::Specimen(Step::After)), State::Nok);
    State::Redo(space_or_tab_min_max(0, usize::MAX))
}

/// After the prefix.
///
/// ```markdown
/// > | (@a) b
///          ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::SpecimenPrefix);
    // A label is a definition as written.
    let mut index = tokenizer.events.len() - 1;
    while tokenizer.events[index].name != Name::SpecimenPrefix
        || tokenizer.events[index].kind != Kind::Enter
    {
        if tokenizer.events[index].name == Name::SpecimenLabel {
            let label = &tokenizer.parse_state.bytes
                [tokenizer.events[index - 1].point.index..tokenizer.events[index].point.index];
            tokenizer
                .definitions
                .specimens
                .push(String::from_utf8_lossy(label).into_owned());
            break;
        }
        index -= 1;
    }
    State::Ok
}

/// Start of a specimen continuation.
///
/// ```markdown
///   | (@a) b
/// > |     c
///     ^
/// ```
fn cont_start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.check(
        State::Next(StateName::Specimen(Step::ContBlank)),
        State::Next(StateName::Specimen(Step::ContFilled)),
    );
    State::Redo(StateName::BlankLine(blank_line::Step::Start))
}

/// Start of a specimen continuation, at a blank line.
///
/// ```markdown
///   | (@a) b
/// > | ␠␠␊
///     ^
/// ```
fn cont_blank(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        State::Redo(indent(0, TAB_SIZE))
    } else {
        State::Ok
    }
}

/// Start of a specimen continuation, at a filled line.
///
/// ```markdown
///   | (@a) b
/// > |     c
///     ^
/// ```
fn cont_filled(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        State::Redo(indent(TAB_SIZE, TAB_SIZE))
    } else {
        State::Nok
    }
}

/// The indentation a continuation line consumes for the specimen.
fn indent(min: usize, max: usize) -> StateName {
    space_or_tab_with_options(SpaceOrTabOptions {
        kind: Name::ContainerIndent,
        min,
        max,
        content: None,
        connect: false,
    })
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Open,
    Before,
    Number { size: usize },
    At,
    LabelBefore,
    Label { left: usize },
    Dash,
    Close,
    CloseAfter,
    After,
    ContStart,
    ContBlank,
    ContFilled,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Specimen(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Open => open(tokenizer),
        Step::Before => before(tokenizer),
        Step::Number { size } => number(tokenizer, size),
        Step::At => at(tokenizer),
        Step::LabelBefore => label_before(tokenizer),
        Step::Label { left } => label(tokenizer, left),
        Step::Dash => dash(tokenizer),
        Step::Close => close(tokenizer),
        Step::CloseAfter => close_after(tokenizer),
        Step::After => after(tokenizer),
        Step::ContStart => cont_start(tokenizer),
        Step::ContBlank => cont_blank(tokenizer),
        Step::ContFilled => cont_filled(tokenizer),
    }
}
