//! The flow content type.
//!
//! **Flow** represents the sections, such as headings and code, which are
//! parsed per line.
//! An example is HTML, which has a certain starting condition (such as
//! `<script>` on its own line), then continues for a while, until an end
//! condition is found (such as `</style>`).
//! If that line with an end condition is never found, that flow goes until
//! the end.
//!
//! The constructs found in flow are:
//!
//! * [Blank line][crate::construct::blank_line]
//! * [Block identifier][crate::construct::block_identifier] (on a line of its own)
//! * [Code (indented)][crate::construct::code_indented]
//! * [Heading (atx)][crate::construct::heading_atx]
//! * [Heading (setext)][crate::construct::heading_setext]
//! * [HTML (flow)][crate::construct::html_flow]
//! * [Formula][crate::construct::formula] (formula blocks)
//! * [Raw (flow)][crate::construct::raw_flow] (code (fenced))
//! * [Thematic break][crate::construct::thematic_break]

use crate::choice::{self, Choice, Entry, List};
use crate::construct::{
    blank_line, block_identifier, callout, code_indented, comment, content, definition_term,
    directive_leaf, formula, heading_atx, heading_setext, html_flow, raw_flow, table,
    thematic_break,
};
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Constructs in flow, in the order they are tried.
///
/// Blank lines are tried before these, and content when none of these
/// matched.
pub const CHOICE: Choice = Choice::new(
    &[
        Entry {
            starts: code_indented::STARTS,
            step: StateName::CodeIndented(code_indented::Step::Start),
        },
        Entry {
            starts: callout::STARTS,
            step: StateName::Callout(callout::Step::Start),
        },
        Entry {
            starts: formula::FLOW,
            step: StateName::Formula(formula::Step::FlowStart),
        },
        Entry {
            starts: raw_flow::STARTS,
            step: StateName::RawFlow(raw_flow::Step::Start),
        },
        Entry {
            starts: html_flow::STARTS,
            step: StateName::HtmlFlow(html_flow::Step::Start),
        },
        Entry {
            starts: heading_atx::STARTS,
            step: StateName::HeadingAtx(heading_atx::Step::Start),
        },
        Entry {
            starts: heading_setext::STARTS,
            step: StateName::HeadingSetext(heading_setext::Step::Start),
        },
        Entry {
            starts: thematic_break::STARTS,
            step: StateName::ThematicBreak(thematic_break::Step::Start),
        },
        Entry {
            starts: directive_leaf::STARTS,
            step: StateName::DirectiveLeaf(directive_leaf::Step::Start),
        },
        Entry {
            starts: comment::FLOW,
            step: StateName::Comment(comment::Step::FlowStart),
        },
        Entry {
            starts: block_identifier::FLOW,
            step: StateName::BlockIdentifier(block_identifier::Step::Start),
        },
        Entry {
            starts: definition_term::STARTS,
            step: StateName::DefinitionTerm(definition_term::Step::Start),
        },
        Entry {
            starts: table::STARTS,
            step: StateName::Table(table::Step::Start),
        },
    ],
    StateName::Flow(Step::After),
    StateName::Flow(Step::BeforeContent),
);

/// Start of flow, at a blank line or a construct.
//
/// ```markdown
/// > | ## alpha
///     ^
/// > |     bravo
///     ^
/// > | ***
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Flow(Step::BlankLineAfter)),
        State::Next(choice::start(List::Flow)),
    );
    State::Redo(StateName::BlankLine(blank_line::Step::Start))
}

/// At content.
///
/// ```markdown
/// > | a
///     ^
/// ```
fn before_content(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(State::Next(StateName::Flow(Step::After)), State::Nok);
    State::Redo(StateName::Content(content::Step::ChunkStart))
}

/// After blank line.
///
/// ```markdown
/// > | ␠␠␊
///       ^
/// ```
fn blank_line_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Ok,
        Some(b'\n') => {
            tokenizer.enter(Name::BlankLineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::BlankLineEnding);
            // Feel free to interrupt.
            tokenizer.interrupt = false;
            State::Next(StateName::Flow(Step::Start))
        }
        _ => unreachable!("expected eol/eof"),
    }
}

/// After flow.
///
/// ```markdown
/// > | # a␊
///        ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Ok,
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            State::Next(StateName::Flow(Step::Start))
        }
        _ => unreachable!("expected eol/eof"),
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    After,
    BlankLineAfter,
    BeforeContent,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Flow(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::After => after(tokenizer),
        Step::BlankLineAfter => blank_line_after(tokenizer),
        Step::BeforeContent => before_content(tokenizer),
    }
}
