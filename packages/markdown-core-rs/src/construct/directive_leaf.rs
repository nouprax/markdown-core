//! Directive (leaf) occurs in the [flow][] content type.
//!
//! ## Grammar
//!
//! Directive (leaf) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! directive_leaf ::= 0*3space_or_tab '::' name [label] [attributes] *space_or_tab
//! ```
//!
//! See [`parts::directive`][crate::construct::parts::directive] for the
//! name and the label, and
//! [`parts::attributes`][crate::construct::parts::attributes] for
//! attributes, which are on the line.
//! Three or more colons start a
//! [directive (container)][crate::construct::directive_container].
//!
//! ## Tokens
//!
//! * [`DirectiveLeaf`][Name::DirectiveLeaf]
//! * [`DirectiveLeafSequence`][Name::DirectiveLeafSequence]
//!
//! ## References
//!
//! * [`micromark-extension-directive`](https://github.com/micromark/micromark-extension-directive)
//!
//! [flow]: crate::construct::flow

use crate::choice::Starts;
use crate::construct::parts;
use crate::construct::parts::space_or_tab::space_or_tab_min_max;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::constant::TAB_SIZE;

/// Bytes this construct may start at.
pub const STARTS: Starts = Starts::Bytes(b"\t :");

/// Start of a directive (leaf).
///
/// ```markdown
/// > | ::a[b]
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::DirectiveLeaf(Step::Before)),
            State::Nok,
        );
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::DirectiveLeaf(Step::Before))
    }
}

/// At the colons, after optional whitespace.
///
/// ```markdown
/// > | ::a[b]
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b':') {
        tokenizer.enter(Name::DirectiveLeaf);
        tokenizer.enter(Name::DirectiveLeafSequence);
        State::Redo(StateName::DirectiveLeaf(Step::Sequence { size: 0 }))
    } else {
        State::Nok
    }
}

/// In the colons.
///
/// ```markdown
/// > | ::a[b]
///     ^^
/// ```
fn sequence(tokenizer: &mut Tokenizer, size: usize) -> State {
    if tokenizer.current == Some(b':') {
        tokenizer.consume();
        State::Next(StateName::DirectiveLeaf(Step::Sequence { size: size + 1 }))
    } else if size == 2 {
        tokenizer.exit(Name::DirectiveLeafSequence);
        tokenizer.attempt(
            State::Next(StateName::DirectiveLeaf(Step::NameAfter)),
            State::Nok,
        );
        State::Redo(StateName::Directive(parts::directive::Step::NameStart))
    } else {
        State::Nok
    }
}

/// After the name.
///
/// ```markdown
/// > | ::a[b]
///        ^
/// ```
fn name_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::DirectiveLeaf(Step::After)),
        State::Nok,
    );
    State::Redo(StateName::Directive(parts::directive::Step::BlockRest))
}

/// At the end of the line.
///
/// ```markdown
/// > | ::a[b]
///           ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::DirectiveLeaf);
    // Feel free to interrupt.
    tokenizer.interrupt = false;
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    Sequence { size: usize },
    NameAfter,
    After,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::DirectiveLeaf(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::Sequence { size } => sequence(tokenizer, size),
        Step::NameAfter => name_after(tokenizer),
        Step::After => after(tokenizer),
    }
}
