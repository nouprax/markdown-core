//! Callout header occurs in the [flow][] content type, as the first line of
//! a [block quote][block_quote].
//!
//! ## Grammar
//!
//! Callout header forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: only on the line that starts the block quote.
//! callout ::= 0*3' ' '[!' 1*variant ']' [fold] [1*space_or_tab [title]]
//!
//! variant ::= ascii_alphanumeric | '_' | '-'
//! fold ::= '+' | '-'
//! title ::= 1*(byte - eol)
//! ```
//!
//! The header makes its block quote a callout: `variant` is its kind, `+`
//! makes it fold open and `-` fold shut, and the title is text.
//! Text right after the header, without whitespace, is not a header.
//!
//! ## Tokens
//!
//! * [`Callout`][Name::Callout]
//! * [`CalloutMarker`][Name::CalloutMarker]
//! * [`CalloutVariant`][Name::CalloutVariant]
//! * [`CalloutFold`][Name::CalloutFold]
//! * [`CalloutTitle`][Name::CalloutTitle]
//!
//! [flow]: crate::construct::flow
//! [block_quote]: crate::construct::block_quote

use crate::choice::Starts;
use crate::construct::partial_space_or_tab::{space_or_tab, space_or_tab_min_max};
use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::tokenizer::{Container, Tokenizer};
use crate::util::constant::TAB_SIZE;

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b" [");

/// Start of a callout header.
///
/// ```markdown
/// > | > [!note] a
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    // Only where a block quote starts on this line, as its first flow.
    let quote = tokenizer.pierce
        && matches!(
            tokenizer.ahead.stack.last(),
            Some(container) if container.kind == Container::BlockQuote
        );
    if !quote {
        return State::Nok;
    }
    tokenizer.enter(Name::Callout);
    if tokenizer.current == Some(b' ') {
        tokenizer.attempt(State::Next(StateName::Callout(Step::Before)), State::Nok);
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::Callout(Step::Before))
    }
}

/// At `[`.
///
/// ```markdown
/// > | > [!note] a
///       ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'[') {
        tokenizer.enter(Name::CalloutMarker);
        tokenizer.consume();
        State::Next(StateName::Callout(Step::Excl))
    } else {
        State::Nok
    }
}

/// After `[`, at `!`.
///
/// ```markdown
/// > | > [!note] a
///        ^
/// ```
fn excl(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'!') {
        tokenizer.consume();
        tokenizer.exit(Name::CalloutMarker);
        State::Next(StateName::Callout(Step::VariantStart))
    } else {
        State::Nok
    }
}

/// Whether `code` can be in a variant.
fn variant(code: Option<u8>) -> bool {
    matches!(
        code,
        Some(b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'_' | b'-')
    )
}

/// After `[!`, at the variant.
///
/// ```markdown
/// > | > [!note] a
///         ^
/// ```
fn variant_start(tokenizer: &mut Tokenizer) -> State {
    if variant(tokenizer.current) {
        tokenizer.enter(Name::CalloutVariant);
        State::Redo(StateName::Callout(Step::Variant))
    } else {
        State::Nok
    }
}

/// In the variant.
///
/// ```markdown
/// > | > [!note] a
///         ^
/// ```
fn variant_inside(tokenizer: &mut Tokenizer) -> State {
    if variant(tokenizer.current) {
        tokenizer.consume();
        State::Next(StateName::Callout(Step::Variant))
    } else if tokenizer.current == Some(b']') {
        tokenizer.exit(Name::CalloutVariant);
        tokenizer.enter(Name::CalloutMarker);
        tokenizer.consume();
        tokenizer.exit(Name::CalloutMarker);
        State::Next(StateName::Callout(Step::Fold))
    } else {
        State::Nok
    }
}

/// After `]`, at an optional fold.
///
/// ```markdown
/// > | > [!note]- a
///              ^
/// ```
fn fold(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'+' | b'-')) {
        tokenizer.enter(Name::CalloutFold);
        tokenizer.consume();
        tokenizer.exit(Name::CalloutFold);
        State::Next(StateName::Callout(Step::After))
    } else {
        State::Redo(StateName::Callout(Step::After))
    }
}

/// After the header, at whitespace or the end of the line.
///
/// ```markdown
/// > | > [!note] a
///              ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Redo(StateName::Callout(Step::End)),
        Some(b'\t' | b' ') => {
            tokenizer.attempt(State::Next(StateName::Callout(Step::Title)), State::Nok);
            State::Redo(space_or_tab())
        }
        _ => State::Nok,
    }
}

/// After whitespace, at an optional title.
///
/// ```markdown
/// > | > [!note] a
///               ^
/// ```
fn title(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) {
        State::Redo(StateName::Callout(Step::End))
    } else {
        tokenizer.enter(Name::CalloutTitle);
        tokenizer.enter_link(
            Name::Data,
            Link {
                previous: None,
                next: None,
                content: Content::Text,
            },
        );
        State::Redo(StateName::Callout(Step::TitleInside))
    }
}

/// In the title.
///
/// ```markdown
/// > | > [!note] a
///               ^
/// ```
fn title_inside(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) {
        tokenizer.exit(Name::Data);
        tokenizer.exit(Name::CalloutTitle);
        State::Redo(StateName::Callout(Step::End))
    } else {
        tokenizer.consume();
        State::Next(StateName::Callout(Step::TitleInside))
    }
}

/// At the end of the header line.
///
/// ```markdown
/// > | > [!note] a
///                ^
/// ```
fn end(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::Callout);
    // Feel free to interrupt.
    tokenizer.interrupt = false;
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    Excl,
    VariantStart,
    Variant,
    Fold,
    After,
    Title,
    TitleInside,
    End,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Callout(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::Excl => excl(tokenizer),
        Step::VariantStart => variant_start(tokenizer),
        Step::Variant => variant_inside(tokenizer),
        Step::Fold => fold(tokenizer),
        Step::After => after(tokenizer),
        Step::Title => title(tokenizer),
        Step::TitleInside => title_inside(tokenizer),
        Step::End => end(tokenizer),
    }
}
