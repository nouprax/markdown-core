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
//! * [Code (indented)][crate::construct::code_indented]
//! * [Heading (atx)][crate::construct::heading_atx]
//! * [Heading (setext)][crate::construct::heading_setext]
//! * [HTML (flow)][crate::construct::html_flow]
//! * [Raw (flow)][crate::construct::raw_flow] (code (fenced), math (flow))
//! * [Thematic break][crate::construct::thematic_break]

use crate::construct::{
    blank_line, code_indented, content, gfm_table, heading_atx, heading_setext, html_flow,
    raw_flow, thematic_break,
};
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// Start of flow.
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
    match tokenizer.current {
        Some(b'#') => {
            tokenizer.attempt(
                State::Next(StateName::Flow(Step::After)),
                State::Next(StateName::Flow(Step::BeforeContent)),
            );
            State::Retry(StateName::HeadingAtx(heading_atx::Step::Start))
        }
        Some(b'$' | b'`' | b'~') => {
            tokenizer.attempt(
                State::Next(StateName::Flow(Step::After)),
                State::Next(StateName::Flow(Step::BeforeContent)),
            );
            State::Retry(StateName::RawFlow(raw_flow::Step::Start))
        }
        // Note: `-` is also used in setext heading underline so it’s not
        // included here.
        Some(b'*' | b'_') => {
            tokenizer.attempt(
                State::Next(StateName::Flow(Step::After)),
                State::Next(StateName::Flow(Step::BeforeContent)),
            );
            State::Retry(StateName::ThematicBreak(thematic_break::Step::Start))
        }
        Some(b'<') => {
            tokenizer.attempt(
                State::Next(StateName::Flow(Step::After)),
                State::Next(StateName::Flow(Step::BeforeHeadingAtx)),
            );
            State::Retry(StateName::HtmlFlow(html_flow::Step::Start))
        }
        // Actual parsing: blank line? Indented code? Indented anything?
        // Tables, setext heading underlines, definitions, and Contents are
        // particularly weird.
        _ => State::Retry(StateName::Flow(Step::BlankLineBefore)),
    }
}

/// At blank line.
///
/// ```markdown
/// > | ␠␠␊
///     ^
/// ```
fn blank_line_before(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Flow(Step::BlankLineAfter)),
        State::Next(StateName::Flow(Step::BeforeCodeIndented)),
    );
    State::Retry(StateName::BlankLine(blank_line::Step::Start))
}

/// At code (indented).
///
/// ```markdown
/// > | ␠␠␠␠a
///     ^
/// ```
fn before_code_indented(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Flow(Step::After)),
        State::Next(StateName::Flow(Step::BeforeRaw)),
    );
    State::Retry(StateName::CodeIndented(code_indented::Step::Start))
}

/// At raw.
///
/// ````markdown
/// > | ```
///     ^
/// ````
fn before_raw(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Flow(Step::After)),
        State::Next(StateName::Flow(Step::BeforeHtml)),
    );
    State::Retry(StateName::RawFlow(raw_flow::Step::Start))
}

/// At html (flow).
///
/// ```markdown
/// > | <a>
///     ^
/// ```
fn before_html(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Flow(Step::After)),
        State::Next(StateName::Flow(Step::BeforeHeadingAtx)),
    );
    State::Retry(StateName::HtmlFlow(html_flow::Step::Start))
}

/// At heading (atx).
///
/// ```markdown
/// > | # a
///     ^
/// ```
fn before_heading_atx(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Flow(Step::After)),
        State::Next(StateName::Flow(Step::BeforeHeadingSetext)),
    );
    State::Retry(StateName::HeadingAtx(heading_atx::Step::Start))
}

/// At heading (setext).
///
/// ```markdown
///   | a
/// > | =
///     ^
/// ```
fn before_heading_setext(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Flow(Step::After)),
        State::Next(StateName::Flow(Step::BeforeThematicBreak)),
    );
    State::Retry(StateName::HeadingSetext(heading_setext::Step::Start))
}

/// At thematic break.
///
/// ```markdown
/// > | ***
///     ^
/// ```
fn before_thematic_break(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Flow(Step::After)),
        State::Next(StateName::Flow(Step::BeforeGfmTable)),
    );
    State::Retry(StateName::ThematicBreak(thematic_break::Step::Start))
}

/// At GFM table.
///
/// ```markdown
/// > | | a |
///     ^
/// ```
fn before_gfm_table(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Flow(Step::After)),
        State::Next(StateName::Flow(Step::BeforeContent)),
    );
    State::Retry(StateName::GfmTable(gfm_table::Step::Start))
}

/// At content.
///
/// ```markdown
/// > | a
///     ^
/// ```
fn before_content(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(State::Next(StateName::Flow(Step::After)), State::Nok);
    State::Retry(StateName::Content(content::Step::ChunkStart))
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
    BeforeGfmTable,
    BeforeCodeIndented,
    BeforeRaw,
    BeforeHtml,
    BeforeHeadingAtx,
    BeforeHeadingSetext,
    BeforeThematicBreak,
    After,
    BlankLineBefore,
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
        Step::BeforeGfmTable => before_gfm_table(tokenizer),
        Step::BeforeCodeIndented => before_code_indented(tokenizer),
        Step::BeforeRaw => before_raw(tokenizer),
        Step::BeforeHtml => before_html(tokenizer),
        Step::BeforeHeadingAtx => before_heading_atx(tokenizer),
        Step::BeforeHeadingSetext => before_heading_setext(tokenizer),
        Step::BeforeThematicBreak => before_thematic_break(tokenizer),
        Step::After => after(tokenizer),
        Step::BlankLineBefore => blank_line_before(tokenizer),
        Step::BlankLineAfter => blank_line_after(tokenizer),
        Step::BeforeContent => before_content(tokenizer),
    }
}
