//! Directive (container) occurs in the [document][] content type.
//!
//! ## Grammar
//!
//! Directive (container) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! directive_container ::= fence_open *(eol *document) [eol fence_close]
//!
//! fence_open ::= 0*3space_or_tab sequence name [label] [attributes] *space_or_tab
//! ; Restriction: at least as many colons as in the opening sequence.
//! fence_close ::= 0*3space_or_tab sequence *space_or_tab
//! ; Restriction: three or more.
//! sequence ::= 3*':'
//! ```
//!
//! See [`partial_directive`][crate::construct::partial_directive] for the
//! name and the label, and
//! [`partial_attributes`][crate::construct::partial_attributes] for
//! attributes, which are on the line.
//! Every line up to the closing fence is the directive’s: there is no
//! prefix, and no lazy line.
//!
//! ## Tokens
//!
//! * [`DirectiveContainer`][Name::DirectiveContainer]
//! * [`DirectiveContainerFence`][Name::DirectiveContainerFence]
//! * [`DirectiveContainerSequence`][Name::DirectiveContainerSequence]
//!
//! ## References
//!
//! * [`micromark-extension-directive`](https://github.com/micromark/micromark-extension-directive)
//!
//! [document]: crate::construct::document

use crate::construct::partial_directive;
use crate::construct::partial_space_or_tab::space_or_tab_min_max;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::constant::TAB_SIZE;

/// The fewest colons of a container’s fence.
const SEQUENCE_SIZE_MIN: usize = 3;

/// Start of a directive (container).
///
/// ```markdown
/// > | :::a
///     ^
///   | b
///   | :::
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::DirectiveContainer);
    tokenizer.enter(Name::DirectiveContainerFence);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::DirectiveContainer(Step::Before)),
            State::Nok,
        );
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::DirectiveContainer(Step::Before))
    }
}

/// At the opening colons, after optional whitespace.
///
/// ```markdown
/// > | :::a
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b':') {
        tokenizer.enter(Name::DirectiveContainerSequence);
        State::Redo(StateName::DirectiveContainer(Step::Sequence { size: 0 }))
    } else {
        State::Nok
    }
}

/// In the opening colons.
///
/// ```markdown
/// > | :::a
///     ^^^
/// ```
fn sequence(tokenizer: &mut Tokenizer, size: usize) -> State {
    if tokenizer.current == Some(b':') {
        tokenizer.consume();
        State::Next(StateName::DirectiveContainer(Step::Sequence {
            size: size + 1,
        }))
    } else if size >= SEQUENCE_SIZE_MIN {
        tokenizer.exit(Name::DirectiveContainerSequence);
        tokenizer.document.stack[tokenizer.document.continued].size = size;
        tokenizer.attempt(
            State::Next(StateName::DirectiveContainer(Step::NameAfter)),
            State::Nok,
        );
        State::Redo(StateName::Directive(partial_directive::Step::NameStart))
    } else {
        State::Nok
    }
}

/// After the name.
///
/// ```markdown
/// > | :::a[b]
///         ^
/// ```
fn name_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::DirectiveContainer(Step::FenceEnd)),
        State::Nok,
    );
    State::Redo(StateName::Directive(partial_directive::Step::BlockRest))
}

/// At the end of the opening fence.
///
/// ```markdown
/// > | :::a
///         ^
/// ```
fn fence_end(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::DirectiveContainerFence);
    State::Ok
}

/// Start of a line in the directive: every line goes on in it, and a
/// closing fence is noted for the document, which ends the directive with
/// it unless a container inside goes on on the line
/// ([`closer`][crate::tokenizer::Document::closer]).
///
/// ```markdown
///   | :::a
/// > | b
///     ^
/// > | :::
///     ^
/// ```
fn cont_start(tokenizer: &mut Tokenizer) -> State {
    let container = &tokenizer.document.stack[tokenizer.document.continued];
    if container.closed {
        return State::Nok;
    }
    let size = container.size;
    tokenizer.check(
        State::Next(StateName::DirectiveContainer(Step::ContCloser)),
        State::Ok,
    );
    State::Redo(StateName::DirectiveContainer(Step::CloseStart { size }))
}

/// At a closing fence of the directive.
fn cont_closer(tokenizer: &mut Tokenizer) -> State {
    tokenizer.document.closer = Some(tokenizer.document.continued);
    State::Ok
}

/// Start of a closing fence of `size` colons or more.
///
/// ```markdown
///   | :::a
/// > | :::
///     ^
/// ```
fn close_start(tokenizer: &mut Tokenizer, size: usize) -> State {
    tokenizer.enter(Name::DirectiveContainerFence);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::DirectiveContainer(Step::CloseBefore { size })),
            State::Nok,
        );
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::DirectiveContainer(Step::CloseBefore { size }))
    }
}

/// At the closing colons, after optional whitespace.
///
/// ```markdown
///   | :::a
/// > | :::
///     ^
/// ```
fn close_before(tokenizer: &mut Tokenizer, size: usize) -> State {
    if tokenizer.current == Some(b':') {
        tokenizer.enter(Name::DirectiveContainerSequence);
        State::Redo(StateName::DirectiveContainer(Step::CloseSequence {
            size,
            count: 0,
        }))
    } else {
        State::Nok
    }
}

/// In the closing colons.
///
/// ```markdown
///   | :::a
/// > | :::
///     ^^^
/// ```
fn close_sequence(tokenizer: &mut Tokenizer, size: usize, count: usize) -> State {
    if tokenizer.current == Some(b':') {
        tokenizer.consume();
        State::Next(StateName::DirectiveContainer(Step::CloseSequence {
            size,
            count: count + 1,
        }))
    } else if count >= size {
        tokenizer.exit(Name::DirectiveContainerSequence);
        tokenizer.attempt(
            State::Next(StateName::DirectiveContainer(Step::CloseEnd)),
            State::Nok,
        );
        State::Redo(StateName::Directive(partial_directive::Step::BlockEnd))
    } else {
        State::Nok
    }
}

/// At the end of the closing fence.
///
/// ```markdown
///   | :::a
/// > | :::
///        ^
/// ```
fn close_end(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::DirectiveContainerFence);
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    Sequence { size: usize },
    NameAfter,
    FenceEnd,
    ContStart,
    ContCloser,
    CloseStart { size: usize },
    CloseBefore { size: usize },
    CloseSequence { size: usize, count: usize },
    CloseEnd,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::DirectiveContainer(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::Sequence { size } => sequence(tokenizer, size),
        Step::NameAfter => name_after(tokenizer),
        Step::FenceEnd => fence_end(tokenizer),
        Step::ContStart => cont_start(tokenizer),
        Step::ContCloser => cont_closer(tokenizer),
        Step::CloseStart { size } => close_start(tokenizer, size),
        Step::CloseBefore { size } => close_before(tokenizer, size),
        Step::CloseSequence { size, count } => close_sequence(tokenizer, size, count),
        Step::CloseEnd => close_end(tokenizer),
    }
}
