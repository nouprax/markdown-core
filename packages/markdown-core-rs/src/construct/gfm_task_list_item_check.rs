//! GFM: Task list item check occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Checks form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! gfm_task_list_item_check ::= '[' (0x09 | ' ' | 'X' | 'x') ']'
//! ```
//!
//! The check is only allowed at the start of the first paragraph, optionally
//! following zero or more definitions or a blank line, in a list item.
//! The check must be followed by whitespace, which is in turn followed by
//! non-whitespace.
//!
//! ## HTML
//!
//! Checks relate to the `<input>` element, in the checkbox state
//! (`type=checkbox`), in HTML.
//! See [*§ 4.10.5.1.15 Checkbox state (`type=checkbox`)*][html-input-checkbox]
//! in the HTML spec for more info.
//!
//! ## Recommendation
//!
//! It is recommended to use lowercase `x` (instead of uppercase `X`), because
//! in markdown, it is more common to use lowercase in places where casing does
//! not matter.
//! It is also recommended to use a space (instead of a tab), as there is no
//! benefit of using tabs in this case.
//!
//! ## Tokens
//!
//! * [`GfmTaskListItemCheck`][Name::GfmTaskListItemCheck]
//! * [`GfmTaskListItemMarker`][Name::GfmTaskListItemMarker]
//! * [`GfmTaskListItemValueChecked`][Name::GfmTaskListItemValueChecked]
//! * [`GfmTaskListItemValueUnchecked`][Name::GfmTaskListItemValueUnchecked]
//!
//! ## References
//!
//! * [`micromark-extension-gfm-task-list-item`](https://github.com/micromark/micromark-extension-gfm-task-list-item)
//! * [*§ 5.3 Task list items (extension)* in `GFM`](https://github.github.com/gfm/#task-list-items-extension-)
//!
//! [text]: crate::construct::text
//! [html-input-checkbox]: https://html.spec.whatwg.org/multipage/input.html#checkbox-state-(type=checkbox)

use crate::construct::partial_space_or_tab::space_or_tab;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;

/// At start of task list item check.
///
/// ```markdown
/// > | * [x] y.
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.document.first && tokenizer.current == Some(b'[') && tokenizer.previous.is_none() {
        tokenizer.enter(Name::GfmTaskListItemCheck);
        tokenizer.enter(Name::GfmTaskListItemMarker);
        tokenizer.consume();
        tokenizer.exit(Name::GfmTaskListItemMarker);
        State::Next(StateName::GfmTaskListItemCheck(Step::Inside))
    } else {
        State::Nok
    }
}

/// In task list item check.
///
/// ```markdown
/// > | * [x] y.
///        ^
/// ```
fn inside(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\t' | b'\n' | b' ') => {
            tokenizer.enter(Name::GfmTaskListItemValueUnchecked);
            tokenizer.consume();
            tokenizer.exit(Name::GfmTaskListItemValueUnchecked);
            State::Next(StateName::GfmTaskListItemCheck(Step::Close))
        }
        Some(b'X' | b'x') => {
            tokenizer.enter(Name::GfmTaskListItemValueChecked);
            tokenizer.consume();
            tokenizer.exit(Name::GfmTaskListItemValueChecked);
            State::Next(StateName::GfmTaskListItemCheck(Step::Close))
        }
        _ => State::Nok,
    }
}

/// At close of task list item check.
///
/// ```markdown
/// > | * [x] y.
///         ^
/// ```
fn close(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b']') => {
            tokenizer.enter(Name::GfmTaskListItemMarker);
            tokenizer.consume();
            tokenizer.exit(Name::GfmTaskListItemMarker);
            tokenizer.exit(Name::GfmTaskListItemCheck);
            State::Next(StateName::GfmTaskListItemCheck(Step::After))
        }
        _ => State::Nok,
    }
}

/// After task list item check.
///
/// ```markdown
/// > | * [x] y.
///          ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // EOL in paragraph means there must be something else after it.
        Some(b'\n') => State::Ok,
        // Space or tab?
        // Check what comes after.
        Some(b'\t' | b' ') => {
            tokenizer.check(State::Ok, State::Nok);
            tokenizer.attempt(
                State::Next(StateName::GfmTaskListItemCheck(Step::AfterSpaceOrTab)),
                State::Nok,
            );
            State::Retry(space_or_tab())
        }
        // EOF, or non-whitespace, both wrong.
        _ => State::Nok,
    }
}

/// After whitespace, after task list item check.
///
/// ```markdown
/// > | * [x] y.
///           ^
/// ```
fn after_space_or_tab(tokenizer: &mut Tokenizer) -> State {
    // End of paragraph, after whitespace, after check, is not okay.
    if tokenizer.current.is_none() {
        State::Nok
    } else {
        State::Ok
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Inside,
    Close,
    After,
    AfterSpaceOrTab,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::GfmTaskListItemCheck(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Inside => inside(tokenizer),
        Step::Close => close(tokenizer),
        Step::After => after(tokenizer),
        Step::AfterSpaceOrTab => after_space_or_tab(tokenizer),
    }
}
