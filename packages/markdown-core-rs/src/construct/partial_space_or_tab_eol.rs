//! Space or tab (eol) occurs in [destination][], [label][], and [title][].
//!
//! ## Grammar
//!
//! Space or tab (eol) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! space_or_tab_eol ::= 1*space_or_tab | *space_or_tab eol *space_or_tab
//! ```
//!
//! Importantly, this allows one line ending, but not blank lines.
//!
//! ## References
//!
//! * [`micromark-factory-space/index.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-factory-space/dev/index.js)
//!
//! [destination]: crate::construct::partial_destination
//! [label]: crate::construct::partial_label
//! [title]: crate::construct::partial_title

use crate::construct::partial_space_or_tab::{
    space_or_tab_with_options, Options as SpaceOrTabOptions,
};
use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::subtokenize::link;
use crate::tokenizer::Tokenizer;

/// Configuration.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Options {
    /// Connect this whitespace to the previous.
    pub connect: bool,
    /// Embedded content type to use.
    pub content: Option<Content>,
}

/// Steps of `space_or_tab_eol`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    /// At the start.
    ///
    /// ```markdown
    /// > | a␠␠␊
    ///      ^
    ///   | ␠␠b
    /// ```
    Start(Options),
    /// After initial whitespace, at optional eol.
    ///
    /// ```markdown
    /// > | a␠␠␊
    ///        ^
    ///   | ␠␠b
    /// ```
    AfterFirst(Options),
    /// After optional whitespace, at optional eol; with whether whitespace
    /// was seen.
    ///
    /// ```markdown
    /// > | a␠␠␊
    ///        ^
    ///   | ␠␠b
    /// ```
    AtEol(Options, bool),
    /// After eol.
    ///
    /// ```markdown
    ///   | a␠␠␊
    /// > | ␠␠b
    ///     ^
    /// ```
    AfterEol(Options),
    /// After optional final whitespace.
    ///
    /// ```markdown
    ///   | a␠␠␊
    /// > | ␠␠b
    ///       ^
    /// ```
    AfterMore,
}

/// `space_or_tab_eol`
pub fn space_or_tab_eol() -> StateName {
    space_or_tab_eol_with_options(Options {
        content: None,
        connect: false,
    })
}

/// `space_or_tab_eol`, with the given options.
pub fn space_or_tab_eol_with_options(options: Options) -> StateName {
    StateName::SpaceOrTabEol(Step::Start(options))
}

/// Whitespace on one line with `options`.
fn whitespace(options: Options) -> StateName {
    space_or_tab_with_options(SpaceOrTabOptions {
        kind: Name::SpaceOrTab,
        min: 1,
        max: usize::MAX,
        content: options.content,
        connect: options.connect,
    })
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start(options) => {
            if matches!(tokenizer.current, Some(b'\t' | b' ')) {
                tokenizer.attempt(
                    State::Next(StateName::SpaceOrTabEol(Step::AfterFirst(options))),
                    State::Next(StateName::SpaceOrTabEol(Step::AtEol(options, false))),
                );
                State::Retry(whitespace(options))
            } else {
                State::Retry(StateName::SpaceOrTabEol(Step::AtEol(options, false)))
            }
        }
        Step::AfterFirst(options) => {
            debug_assert!(options.content.is_none(), "expected no content");
            // If the above ever errors, set `connect: true` in that case.
            State::Retry(StateName::SpaceOrTabEol(Step::AtEol(options, true)))
        }
        Step::AtEol(mut options, ok) => {
            if let Some(b'\n') = tokenizer.current {
                if let Some(content) = options.content {
                    tokenizer.enter_link(
                        Name::LineEnding,
                        Link {
                            previous: None,
                            next: None,
                            content,
                        },
                    );
                } else {
                    tokenizer.enter(Name::LineEnding);
                }

                if options.connect {
                    let index = tokenizer.events.len() - 1;
                    link(&mut tokenizer.events, index);
                } else if options.content.is_some() {
                    options.connect = true;
                }

                tokenizer.consume();
                tokenizer.exit(Name::LineEnding);
                State::Next(StateName::SpaceOrTabEol(Step::AfterEol(options)))
            } else if ok {
                State::Ok
            } else {
                State::Nok
            }
        }
        Step::AfterEol(options) => {
            if matches!(tokenizer.current, Some(b'\t' | b' ')) {
                tokenizer.attempt(
                    State::Next(StateName::SpaceOrTabEol(Step::AfterMore)),
                    State::Nok,
                );
                State::Retry(whitespace(options))
            } else {
                State::Retry(StateName::SpaceOrTabEol(Step::AfterMore))
            }
        }
        Step::AfterMore => {
            debug_assert!(
                !matches!(tokenizer.current, None | Some(b'\n')),
                "did not expect blank line"
            );
            // If the above ever starts erroring, gracefully `State::Nok` on it.
            // Currently it doesn’t happen, as we only use this in content, which does
            // not allow blank lines.
            State::Ok
        }
    }
}
