//! Comment occurs in the [text][] and [flow][] content types.
//!
//! ## Grammar
//!
//! Comment forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: the body ends at the first `%%` after the opener.
//! comment_text ::= '%%' *byte '%%'
//!
//! ; Restriction: a later line under the same containers is a fence.
//! comment_flow ::= fence *(eol *line) eol fence
//! fence ::= 0*3space_or_tab '%%' *space_or_tab
//! ```
//!
//! In flow, a fence line opens a comment only when a later line under the
//! same containers is a fence, which a [lookahead][crate::ahead] finds
//! before anything opens.
//! Otherwise the line is not a comment, and is likely a paragraph.
//! The lines between the fences are the body, and no construct is
//! recognized in them: not even containers.
//!
//! The opener is `%%` at the cursor, so `%%%a%%%` is the comment `%a`
//! followed by the text `%`, and `%%%%` is an empty comment.
//! Every byte of the body is the comment's, as written: no construct is
//! recognized inside it.
//! The body may span the lines of its text; the whitespace that starts a
//! continuation line is not part of it.
//! An opener without a closer is not a comment.
//!
//! ## Tokens
//!
//! * [`Comment`][Name::Comment]
//! * [`CommentSequence`][Name::CommentSequence]
//! * [`CommentText`][Name::CommentText]
//! * [`LineEnd`][Name::LineEnd]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//! * [`CommentFlow`][Name::CommentFlow]
//! * [`CommentFence`][Name::CommentFence]
//! * [`CommentChunk`][Name::CommentChunk]
//!
//! [text]: crate::construct::text
//! [flow]: crate::construct::flow

use crate::ahead::ahead;
use crate::choice::Starts;
use crate::construct::partial_space_or_tab::{space_or_tab, space_or_tab_min_max};
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::constant::TAB_SIZE;

/// Bytes this construct can start at in text.
pub const STARTS: Starts = Starts::Codes(b"%");

/// Bytes this construct can start at in flow.
pub const FLOW: Starts = Starts::Codes(b"\t %");

/// Start of comment.
///
/// ```markdown
/// > | a %%b%% c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'%') {
        tokenizer.enter(Name::Comment);
        tokenizer.enter(Name::CommentSequence);
        tokenizer.consume();
        State::Next(StateName::Comment(Step::Open))
    } else {
        State::Nok
    }
}

/// After the first sign of the opener.
///
/// ```markdown
/// > | a %%b%% c
///        ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'%') {
        tokenizer.consume();
        tokenizer.exit(Name::CommentSequence);
        State::Next(StateName::Comment(Step::Between))
    } else {
        State::Nok
    }
}

/// In the body, between parts.
///
/// ```markdown
/// > | a %%b%% c
///         ^
/// ```
fn between(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnd);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnd);
            State::Next(StateName::Comment(Step::Line))
        }
        Some(b'%') => {
            tokenizer.enter(Name::CommentSequence);
            tokenizer.consume();
            State::Next(StateName::Comment(Step::Close))
        }
        _ => {
            tokenizer.enter(Name::CommentText);
            State::Redo(StateName::Comment(Step::Text))
        }
    }
}

/// At the start of a continuation line.
///
/// ```markdown
///   | a %%b
/// > |   c%% d
///     ^
/// ```
fn line(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.enter(Name::SpaceOrTab);
        State::Redo(StateName::Comment(Step::Indent))
    } else {
        State::Redo(StateName::Comment(Step::Between))
    }
}

/// In the whitespace that starts a continuation line.
///
/// ```markdown
///   | a %%b
/// > |   c%% d
///     ^
/// ```
fn indent(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.consume();
        State::Next(StateName::Comment(Step::Indent))
    } else {
        tokenizer.exit(Name::SpaceOrTab);
        State::Redo(StateName::Comment(Step::Between))
    }
}

/// In body text.
///
/// ```markdown
/// > | a %%b%% c
///         ^
/// ```
fn text(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n' | b'%')) {
        tokenizer.exit(Name::CommentText);
        State::Redo(StateName::Comment(Step::Between))
    } else {
        tokenizer.consume();
        State::Next(StateName::Comment(Step::Text))
    }
}

/// After the first sign of a closer.
///
/// ```markdown
/// > | a %%b%% c
///          ^
/// ```
fn close(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'%') {
        tokenizer.consume();
        tokenizer.exit(Name::CommentSequence);
        tokenizer.exit(Name::Comment);
        State::Ok
    } else {
        // A lone sign is body text.
        tokenizer.exit(Name::CommentSequence);
        let len = tokenizer.events.len();
        tokenizer.events[len - 2].name = Name::CommentText;
        tokenizer.events[len - 1].name = Name::CommentText;
        State::Redo(StateName::Comment(Step::Between))
    }
}

/// Start of a comment in flow.
///
/// ```markdown
/// > | %%
///     ^
///   | a
///   | %%
/// ```
fn flow_start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::CommentFlow);
    tokenizer.attempt(
        State::Next(StateName::Comment(Step::FlowOpen)),
        State::Nok,
    );
    State::Redo(StateName::Comment(Step::FenceStart))
}

/// After the opening fence, at its line ending.
///
/// ```markdown
/// > | %%
///       ^
///   | a
///   | %%
/// ```
fn flow_open(tokenizer: &mut Tokenizer) -> State {
    if ahead(tokenizer, StateName::Comment(Step::FenceStart)) {
        // Do not form containers.
        tokenizer.concrete = true;
        State::Redo(StateName::Comment(Step::FlowBreak))
    } else {
        State::Nok
    }
}

/// At a line ending in the comment, before a closing fence or a body line.
///
/// ```markdown
/// > | %%
///       ^
/// > | a
///      ^
///   | %%
/// ```
fn flow_break(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Comment(Step::FlowAfter)),
        State::Next(StateName::Comment(Step::FlowEol)),
    );
    State::Redo(StateName::Comment(Step::FlowClose))
}

/// At a line ending, before a closing fence.
///
/// ```markdown
///   | %%
/// > | a
///      ^
///   | %%
/// ```
fn flow_close(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::LineEnd);
    tokenizer.consume();
    tokenizer.exit(Name::LineEnd);
    State::Next(StateName::Comment(Step::FenceStart))
}

/// At a line ending, before a body line.
///
/// ```markdown
/// > | %%
///       ^
///   | a
///   | %%
/// ```
fn flow_eol(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::LineEnd);
    tokenizer.consume();
    tokenizer.exit(Name::LineEnd);
    State::Next(StateName::Comment(Step::FlowLine))
}

/// At the start of a body line.
///
/// ```markdown
///   | %%
/// > | a
///     ^
///   | %%
/// ```
fn flow_line(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Redo(StateName::Comment(Step::FlowAfter)),
        Some(b'\n') => State::Redo(StateName::Comment(Step::FlowBreak)),
        _ => {
            tokenizer.enter(Name::CommentChunk);
            State::Redo(StateName::Comment(Step::FlowChunk))
        }
    }
}

/// In a body line.
///
/// ```markdown
///   | %%
/// > | a
///     ^
///   | %%
/// ```
fn flow_chunk(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) {
        tokenizer.exit(Name::CommentChunk);
        State::Redo(StateName::Comment(Step::FlowLine))
    } else {
        tokenizer.consume();
        State::Next(StateName::Comment(Step::FlowChunk))
    }
}

/// After the closing fence.
///
/// ```markdown
///   | %%
///   | a
/// > | %%
///       ^
/// ```
fn flow_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::CommentFlow);
    // No longer concrete.
    tokenizer.concrete = false;
    // Feel free to interrupt.
    tokenizer.interrupt = false;
    State::Ok
}

/// Start of a fence line.
///
/// ```markdown
/// > | %%
///     ^
/// ```
fn fence_start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::CommentFence);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::Comment(Step::FenceBefore)),
            State::Nok,
        );
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::Comment(Step::FenceBefore))
    }
}

/// In a fence line, at its sequence.
///
/// ```markdown
/// > | %%
///     ^
/// ```
fn fence_before(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'%') {
        tokenizer.enter(Name::CommentSequence);
        tokenizer.consume();
        State::Next(StateName::Comment(Step::FenceSecond))
    } else {
        State::Nok
    }
}

/// In a fence line, after its first sign.
///
/// ```markdown
/// > | %%
///      ^
/// ```
fn fence_second(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'%') {
        tokenizer.consume();
        State::Next(StateName::Comment(Step::FenceRest))
    } else {
        State::Nok
    }
}

/// In a fence line, after its sequence.
///
/// ```markdown
/// > | %%
///       ^
/// ```
fn fence_rest(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::CommentSequence);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::Comment(Step::FenceAfter)),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::Comment(Step::FenceAfter))
    }
}

/// In a fence line, after its sequence and whitespace.
///
/// ```markdown
/// > | %%
///       ^
/// ```
fn fence_after(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) {
        tokenizer.exit(Name::CommentFence);
        State::Ok
    } else {
        State::Nok
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Open,
    Between,
    Line,
    Indent,
    Text,
    Close,
    FlowStart,
    FlowOpen,
    FlowBreak,
    FlowClose,
    FlowEol,
    FlowLine,
    FlowChunk,
    FlowAfter,
    FenceStart,
    FenceBefore,
    FenceSecond,
    FenceRest,
    FenceAfter,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Comment(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Open => open(tokenizer),
        Step::Between => between(tokenizer),
        Step::Line => line(tokenizer),
        Step::Indent => indent(tokenizer),
        Step::Text => text(tokenizer),
        Step::Close => close(tokenizer),
        Step::FlowStart => flow_start(tokenizer),
        Step::FlowOpen => flow_open(tokenizer),
        Step::FlowBreak => flow_break(tokenizer),
        Step::FlowClose => flow_close(tokenizer),
        Step::FlowEol => flow_eol(tokenizer),
        Step::FlowLine => flow_line(tokenizer),
        Step::FlowChunk => flow_chunk(tokenizer),
        Step::FlowAfter => flow_after(tokenizer),
        Step::FenceStart => fence_start(tokenizer),
        Step::FenceBefore => fence_before(tokenizer),
        Step::FenceSecond => fence_second(tokenizer),
        Step::FenceRest => fence_rest(tokenizer),
        Step::FenceAfter => fence_after(tokenizer),
    }
}
