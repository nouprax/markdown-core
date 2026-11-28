//! Definition term occurs in the [flow][] content type.
//!
//! ## Grammar
//!
//! Definition term forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: the line is no definition marker and holds no reference
//! ; definition; the next line, or the one after a blank next line, starts
//! ; with a definition marker under the same containers.
//! definition_term ::= 0*3space_or_tab text *space_or_tab
//! ```
//!
//! A term is a line that would start a paragraph, followed by the
//! [definition body][definition_body] it names, at most one blank line on.
//! Its text is trimmed: trailing whitespace or a backslash makes no hard
//! break.
//! It is tried after every other flow construct but a table, which needs a
//! delimiter row on its next line that a marker line never is, so that the
//! term is known by the end of its line; it does not interrupt a paragraph,
//! as no flow construct does at a line that goes on with one.
//!
//! The marker line is found with a [lookahead][crate::ahead], since the
//! containers of that line are decided before its flow is read: the term
//! tells the document on which line the marker is due, where the body opens.
//!
//! ## Tokens
//!
//! * [`Data`][Name::Data]
//! * [`DefinitionTerm`][Name::DefinitionTerm]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! [flow]: crate::construct::flow
//! [definition_body]: crate::construct::definition_body

use crate::ahead::ahead;
use crate::choice::Starts;
use crate::construct::definition_body::{blank_at, marks};
use crate::construct::parts::space_or_tab::space_or_tab;
use crate::construct::{blank_line, definition, definition_body, table};
use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Bytes a term may start with: any.
pub const STARTS: Starts = Starts::Any;

/// At the start of a line that may be a term.
///
/// ```markdown
/// > | a
///     ^
///   | : b
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    let bytes = tokenizer.parse_state.bytes;
    let index = tokenizer.point.index;
    let eol = line_end(bytes, index);
    // A quick refusal of almost every line: no marker is near.
    let near = near(bytes, index);
    // A term interrupts nothing, and a line that goes on with a table holds
    // none.
    if !near || tokenizer.interrupt || table::open(tokenizer) || reference(tokenizer, eol) {
        return State::Nok;
    }
    let mut end = eol;
    while end > index && matches!(bytes[end - 1], b'\t' | b' ') {
        end -= 1;
    }
    tokenizer.check(
        State::Nok,
        State::Next(StateName::DefinitionTerm(Step::Before { end })),
    );
    State::Redo(StateName::DefinitionBody(
        definition_body::Step::MarkerStart,
    ))
}

/// The end of the line at `index`.
fn line_end(bytes: &[u8], index: usize) -> usize {
    bytes[index..]
        .iter()
        .position(|byte| matches!(byte, b'\n' | b'\r'))
        .map_or(bytes.len(), |at| index + at)
}

/// Whether a body marker is near the line at `index`: on the line after it,
/// or on the one after a blank line, so that the line may be a term.
pub fn near(bytes: &[u8], index: usize) -> bool {
    let eol = line_end(bytes, index);
    let next = eol
        + if bytes[eol..].starts_with(b"\r\n") {
            2
        } else {
            1
        };
    next < bytes.len()
        && (marks(bytes, next) || blank_at(bytes, next).is_some_and(|after| marks(bytes, after)))
}

/// Whether the line from the cursor to `eol` holds a reference definition,
/// read as the whole input.
fn reference(tokenizer: &Tokenizer, eol: usize) -> bool {
    let mut scratch = Tokenizer::new(tokenizer.point.clone(), tokenizer.parse_state);
    // Whether found or not, the test ends well; a found one leaves events.
    scratch.attempt(State::Ok, State::Ok);
    scratch.run(
        (tokenizer.point.index, 0),
        (eol, 0),
        State::Next(StateName::Definition(definition::Step::Start)),
    );
    !scratch.events.is_empty()
}

/// At the line, which is no marker line, before its indentation.
///
/// ```markdown
/// > | a
///     ^
///   | : b
/// ```
fn before(tokenizer: &mut Tokenizer, end: usize) -> State {
    tokenizer.enter(Name::DefinitionTerm);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::DefinitionTerm(Step::Open { end })),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::DefinitionTerm(Step::Open { end }))
    }
}

/// At the text of the term.
///
/// ```markdown
/// > | a
///     ^
///   | : b
/// ```
fn open(tokenizer: &mut Tokenizer, end: usize) -> State {
    tokenizer.enter_link(
        Name::Data,
        Link {
            previous: None,
            next: None,
            content: Content::Text,
        },
    );
    State::Redo(StateName::DefinitionTerm(Step::Inside { end }))
}

/// In the text of the term, which ends at `end`.
///
/// ```markdown
/// > | a
///     ^
///   | : b
/// ```
fn inside(tokenizer: &mut Tokenizer, end: usize) -> State {
    if tokenizer.point.index < end {
        tokenizer.consume();
        return State::Next(StateName::DefinitionTerm(Step::Inside { end }));
    }
    tokenizer.exit(Name::Data);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::DefinitionTerm(Step::After)),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::DefinitionTerm(Step::After))
    }
}

/// At the end of the line: the marker follows it, or the line after a blank
/// one.
///
/// ```markdown
/// > | a
///      ^
///   | : b
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current != Some(b'\n') {
        return State::Nok;
    }
    let marker = StateName::DefinitionBody(definition_body::Step::MarkerStart);
    let line = tokenizer.point.line;
    let due = if ahead(tokenizer, marker, 1) {
        line + 1
    } else if ahead(tokenizer, StateName::BlankLine(blank_line::Step::Start), 1)
        && ahead(tokenizer, marker, 2)
    {
        line + 2
    } else {
        return State::Nok;
    };
    tokenizer.exit(Name::DefinitionTerm);
    tokenizer.term = Some(due);
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before { end: usize },
    Open { end: usize },
    Inside { end: usize },
    After,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::DefinitionTerm(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before { end } => before(tokenizer, end),
        Step::Open { end } => open(tokenizer, end),
        Step::Inside { end } => inside(tokenizer, end),
        Step::After => after(tokenizer),
    }
}
