//! Space or tab occurs in tons of places.
//!
//! ## Grammar
//!
//! Space or tab forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! space_or_tab ::= 1*('\t' | ' ')
//! ```
//!
//! ## References
//!
//! * [`micromark-factory-space/index.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-factory-space/dev/index.js)

use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::subtokenize::link;
use crate::tokenizer::Tokenizer;

/// Configuration.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Options {
    /// Minimum allowed bytes (inclusive).
    pub min: usize,
    /// Maximum allowed bytes (inclusive).
    pub max: usize,
    /// Name to use for events.
    pub kind: Name,
    /// Connect this event to the previous.
    pub connect: bool,
    /// Embedded content type to use.
    pub content: Option<Content>,
}

/// Steps of `space_or_tab`, each with the options and the bytes seen.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    /// At the start.
    ///
    /// ```markdown
    /// > | a␠␠b
    ///      ^
    /// ```
    Start(Options),
    /// In whitespace.
    ///
    /// ```markdown
    /// > | a␠␠b
    ///       ^
    /// ```
    Inside(Options, usize),
    /// After whitespace, also when there was none.
    ///
    /// ```markdown
    /// > | a␠␠b
    ///        ^
    /// ```
    After(Options, usize),
}

/// One or more `space_or_tab`.
///
/// ```bnf
/// space_or_tab ::= 1*( ' ' '\t' )
/// ```
pub fn space_or_tab() -> StateName {
    space_or_tab_min_max(1, usize::MAX)
}

/// Between `x` and `y` `space_or_tab`.
///
/// ```bnf
/// space_or_tab_min_max ::= x*y( ' ' '\t' )
/// ```
pub fn space_or_tab_min_max(min: usize, max: usize) -> StateName {
    space_or_tab_with_options(Options {
        kind: Name::SpaceOrTab,
        min,
        max,
        content: None,
        connect: false,
    })
}

/// `space_or_tab`, with the given options.
pub fn space_or_tab_with_options(options: Options) -> StateName {
    StateName::SpaceOrTab(Step::Start(options))
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start(options) => {
            if options.max > 0 && matches!(tokenizer.current, Some(b'\t' | b' ')) {
                if let Some(content) = options.content {
                    tokenizer.enter_link(
                        options.kind,
                        Link {
                            previous: None,
                            next: None,
                            content,
                        },
                    );
                } else {
                    tokenizer.enter(options.kind);
                }

                if options.connect {
                    let index = tokenizer.events.len() - 1;
                    link(&mut tokenizer.events, index);
                }

                State::Retry(StateName::SpaceOrTab(Step::Inside(options, 0)))
            } else {
                State::Retry(StateName::SpaceOrTab(Step::After(options, 0)))
            }
        }
        Step::Inside(options, size) => match tokenizer.current {
            Some(b'\t' | b' ') if size < options.max => {
                tokenizer.consume();
                State::Next(StateName::SpaceOrTab(Step::Inside(options, size + 1)))
            }
            _ => {
                tokenizer.exit(options.kind);
                State::Retry(StateName::SpaceOrTab(Step::After(options, size)))
            }
        },
        Step::After(options, size) => {
            if size >= options.min {
                State::Ok
            } else {
                State::Nok
            }
        }
    }
}
