//! Raw (flow) occurs in the [flow][] content type.
//! It forms code (fenced).
//!
//! ## Grammar
//!
//! Code (fenced) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! raw_flow ::= fence_open *( eol *byte ) [ eol fence_close ]
//!
//! fence_open ::= sequence [*space_or_tab info [1*space_or_tab meta]] *space_or_tab
//! ; Restriction: the number of markers in the closing fence sequence must be
//! ; equal to or greater than the number of markers in the opening fence
//! ; sequence.
//! ; Restriction: the marker in the closing fence sequence must match the
//! ; marker in the opening fence sequence
//! fence_close ::= sequence *space_or_tab
//! sequence ::= 3*'`' | 3*'~'
//! ; Restriction: the marker cannot occur in `info` if it is the `` ` `` character.
//! info ::= 1*text
//! ; Restriction: the marker cannot occur in `meta` if it is the `` ` `` character.
//! meta ::= 1*text *(*space_or_tab 1*text)
//! ```
//!
//! As this construct occurs in flow, like all flow constructs, it must be
//! followed by an eol (line ending) or eof (end of file).
//!
//! The above grammar does not show how indentation (with `space_or_tab`) of
//! each line is handled.
//! To parse raw (flow), let `x` be the number of `space_or_tab` characters
//! before the opening fence sequence.
//! Each line of text is then allowed (not required) to be indented with up
//! to `x` spaces or tabs, which are then ignored as an indent instead of being
//! considered as part of the content.
//! This indent does not affect the closing fence.
//! It can be indented up to a separate 3 spaces or tabs.
//! A bigger indent makes it part of the content instead of a fence.
//!
//! The `info` and `meta` parts are interpreted as the [string][string] content type.
//! That means that [character escapes][character_escape] and
//! [character references][character_reference] are allowed.
//!
//! The optional `meta` part is ignored: it is not used when parsing or
//! rendering.
//!
//! The optional `info` part is used and is expected to specify the programming
//! language that the content is in.
//! Which value it holds depends on what your syntax highlighter supports, if
//! one is used.
//!
//! In markdown, it is also possible to use [raw (text)][raw_text] in the
//! [text][] content type.
//! It is also possible to create code with the
//! [code (indented)][code_indented] construct.
//!
//! ## HTML
//!
//! Code (fenced) relates to both the `<pre>` and the `<code>` elements in
//! HTML.
//! See [*§ 4.4.3 The `pre` element*][html_pre] and the [*§ 4.5.15 The `code`
//! element*][html_code] in the HTML spec for more info.
//!
//! The `info` is, when rendering to HTML, typically exposed as a class.
//! This behavior stems from the HTML spec ([*§ 4.5.15 The `code`
//! element*][html_code]).
//! For example:
//!
//! ```markdown
//! ~~~css
//! * { color: tomato }
//! ~~~
//! ```
//!
//! Yields:
//!
//! ```html
//! <pre><code class="language-css">* { color: tomato }
//! </code></pre>
//! ```
//!
//! ## Recommendation
//!
//! It is recommended to use code (fenced) instead of code (indented).
//! Code (fenced) is more explicit, similar to code (text), and has support
//! for specifying the programming language.
//!
//! ## Tokens
//!
//! * [`CodeFenced`][Name::CodeFenced]
//! * [`CodeFencedFence`][Name::CodeFencedFence]
//! * [`CodeFencedFenceInfo`][Name::CodeFencedFenceInfo]
//! * [`CodeFencedFenceMeta`][Name::CodeFencedFenceMeta]
//! * [`CodeFencedFenceSequence`][Name::CodeFencedFenceSequence]
//! * [`CodeFlowChunk`][Name::CodeFlowChunk]
//! * [`LineEnding`][Name::LineEnding]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! ## References
//!
//! * [`code-fenced.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/code-fenced.js)
//! * [*§ 4.5 Fenced code blocks* in `CommonMark`](https://spec.commonmark.org/0.31/#fenced-code-blocks)
//!
//! [flow]: crate::construct::flow
//! [plain]: crate::construct::plain
//! [text]: crate::construct::text
//! [character_escape]: crate::construct::character_escape
//! [character_reference]: crate::construct::character_reference
//! [code_indented]: crate::construct::code_indented
//! [raw_text]: crate::construct::raw_text
//! [html_code]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-code-element
//! [html_pre]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-pre-element

use crate::choice::Starts;
use crate::construct::partial_space_or_tab::{space_or_tab, space_or_tab_min_max};
use crate::construct::{partial_attributes, partial_non_lazy_continuation};
use crate::event::{Content, Link, Name};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::{
    constant::{CODE_FENCED_SEQUENCE_SIZE_MIN, TAB_SIZE},
    slice::{Position, Slice},
};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b"\t `~");

/// Start of raw.
///
/// ```markdown
/// > | ~~~js
///     ^
///   | console.log(1)
///   | ~~~
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::RawFlow(Step::BeforeSequenceOpen)),
            State::Nok,
        );
        return State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1));
    }

    if matches!(tokenizer.current, Some(b'`' | b'~')) {
        return State::Redo(StateName::RawFlow(Step::BeforeSequenceOpen));
    }

    State::Nok
}

/// In opening fence, after prefix, at sequence.
///
/// ```markdown
/// > | ~~~js
///     ^
///   | console.log(1)
///   | ~~~
/// ```
fn before_sequence_open(tokenizer: &mut Tokenizer) -> State {
    let tail = tokenizer.events.last();
    let mut prefix = 0;

    if let Some(event) = tail {
        if event.name == Name::SpaceOrTab {
            prefix = Slice::from_position(
                tokenizer.parse_state.bytes,
                &Position::from_exit_event(&tokenizer.events, tokenizer.events.len() - 1),
            )
            .len();
        }
    }

    if matches!(tokenizer.current, Some(b'`' | b'~')) {
        let fence = Fence {
            marker: tokenizer.current.unwrap(),
            size: 0,
            prefix,
        };
        tokenizer.enter(Name::CodeFenced);
        tokenizer.enter(Name::CodeFencedFence);
        tokenizer.enter(Name::CodeFencedFenceSequence);
        State::Redo(StateName::RawFlow(Step::SequenceOpen { fence }))
    } else {
        State::Nok
    }
}

/// A fence: its marker, the size of its opening sequence, and the indent
/// before it.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Fence {
    marker: u8,
    size: usize,
    prefix: usize,
}

/// In opening fence sequence.
///
/// ```markdown
/// > | ~~~js
///      ^
///   | console.log(1)
///   | ~~~
/// ```
fn sequence_open(tokenizer: &mut Tokenizer, mut fence: Fence) -> State {
    if tokenizer.current == Some(fence.marker) {
        fence.size += 1;
        tokenizer.consume();
        State::Next(StateName::RawFlow(Step::SequenceOpen { fence }))
    } else if fence.size < CODE_FENCED_SEQUENCE_SIZE_MIN {
        State::Nok
    } else {
        let next = StateName::RawFlow(Step::InfoBefore { fence });

        tokenizer.exit(Name::CodeFencedFenceSequence);
        if matches!(tokenizer.current, Some(b'\t' | b' ')) {
            tokenizer.attempt(State::Next(next), State::Nok);
            State::Redo(space_or_tab())
        } else {
            State::Redo(next)
        }
    }
}

/// In opening fence, after the sequence (and optional whitespace), before info.
///
/// ```markdown
/// > | ~~~js
///        ^
///   | console.log(1)
///   | ~~~
/// ```
fn info_before(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::CodeFencedFence);
            // Do not form containers.
            tokenizer.concrete = true;
            tokenizer.check(
                State::Next(StateName::RawFlow(Step::AtNonLazyBreak { fence })),
                State::Next(StateName::RawFlow(Step::After)),
            );
            State::Redo(StateName::NonLazyContinuation(
                partial_non_lazy_continuation::Step::Start,
            ))
        }
        // Attributes that end the line, or info.
        Some(b'{') => {
            tokenizer.attempt(
                State::Next(StateName::RawFlow(Step::InfoBefore { fence })),
                State::Next(StateName::RawFlow(Step::FieldOpen { fence, meta: false })),
            );
            State::Redo(StateName::Attributes(partial_attributes::Step::Tail))
        }
        _ => {
            tokenizer.enter(Name::CodeFencedFenceInfo);
            tokenizer.enter_link(
                Name::Data,
                Link {
                    previous: None,
                    next: None,
                    content: Content::String,
                },
            );
            State::Redo(StateName::RawFlow(Step::Info { fence }))
        }
    }
}

/// In info.
///
/// ```markdown
/// > | ~~~js
///        ^
///   | console.log(1)
///   | ~~~
/// ```
fn info(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::Data);
            tokenizer.exit(Name::CodeFencedFenceInfo);
            State::Redo(StateName::RawFlow(Step::InfoBefore { fence }))
        }
        Some(b'\t' | b' ') => {
            tokenizer.exit(Name::Data);
            tokenizer.exit(Name::CodeFencedFenceInfo);
            tokenizer.attempt(
                State::Next(StateName::RawFlow(Step::MetaBefore { fence })),
                State::Nok,
            );
            State::Redo(space_or_tab())
        }
        // Attributes that end the line, or text.
        Some(b'{') => {
            tokenizer.check(
                State::Next(StateName::RawFlow(Step::FieldClose { fence, meta: false })),
                State::Next(StateName::RawFlow(Step::FieldByte { fence, meta: false })),
            );
            State::Redo(StateName::Attributes(partial_attributes::Step::Tail))
        }
        Some(b'\\') => {
            tokenizer.consume();
            State::Next(StateName::RawFlow(Step::FieldEscape { fence, meta: false }))
        }
        Some(byte) => {
            // This looks like code (text).
            // Note: no reason to check for `~`, because 3 of them can‘t be
            // used as strikethrough in text.
            if fence.marker == byte && byte == b'`' {
                tokenizer.concrete = false;
                State::Nok
            } else {
                tokenizer.consume();
                State::Next(StateName::RawFlow(Step::Info { fence }))
            }
        }
    }
}

/// In opening fence, after info and whitespace, before meta.
///
/// ```markdown
/// > | ~~~js eval
///           ^
///   | console.log(1)
///   | ~~~
/// ```
fn meta_before(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Redo(StateName::RawFlow(Step::InfoBefore { fence })),
        Some(b'{') => {
            tokenizer.attempt(
                State::Next(StateName::RawFlow(Step::InfoBefore { fence })),
                State::Next(StateName::RawFlow(Step::FieldOpen { fence, meta: true })),
            );
            State::Redo(StateName::Attributes(partial_attributes::Step::Tail))
        }
        _ => {
            tokenizer.enter(Name::CodeFencedFenceMeta);
            tokenizer.enter_link(
                Name::Data,
                Link {
                    previous: None,
                    next: None,
                    content: Content::String,
                },
            );
            State::Redo(StateName::RawFlow(Step::Meta { fence }))
        }
    }
}

/// In meta.
///
/// ```markdown
/// > | ~~~js eval
///           ^
///   | console.log(1)
///   | ~~~
/// ```
fn meta(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::Data);
            tokenizer.exit(Name::CodeFencedFenceMeta);
            State::Redo(StateName::RawFlow(Step::InfoBefore { fence }))
        }
        // Attributes that end the line, or text.
        Some(b'{') => {
            tokenizer.check(
                State::Next(StateName::RawFlow(Step::FieldClose { fence, meta: true })),
                State::Next(StateName::RawFlow(Step::FieldByte { fence, meta: true })),
            );
            State::Redo(StateName::Attributes(partial_attributes::Step::Tail))
        }
        Some(b'\\') => {
            tokenizer.consume();
            State::Next(StateName::RawFlow(Step::FieldEscape { fence, meta: true }))
        }
        Some(byte) => {
            // This looks like code (text).
            // Note: no reason to check for `~`, because 3 of them can‘t be
            // used as strikethrough in text.
            if fence.marker == byte && byte == b'`' {
                tokenizer.concrete = false;
                State::Nok
            } else {
                tokenizer.consume();
                State::Next(StateName::RawFlow(Step::Meta { fence }))
            }
        }
    }
}

/// At info or meta that starts with a brace which does not start attributes
/// that end the line.
///
/// ```markdown
/// > | ~~~{a
///        ^
/// ```
fn field_open(tokenizer: &mut Tokenizer, fence: Fence, meta: bool) -> State {
    tokenizer.enter(field_name(meta));
    tokenizer.enter_link(
        Name::Data,
        Link {
            previous: None,
            next: None,
            content: Content::String,
        },
    );
    State::Redo(StateName::RawFlow(Step::FieldByte { fence, meta }))
}

/// In info or meta, at attributes that end the line.
///
/// ```markdown
/// > | ~~~js{.a}
///          ^
/// ```
fn field_close(tokenizer: &mut Tokenizer, fence: Fence, meta: bool) -> State {
    tokenizer.exit(Name::Data);
    tokenizer.exit(field_name(meta));
    tokenizer.attempt(
        State::Next(StateName::RawFlow(Step::InfoBefore { fence })),
        State::Nok,
    );
    State::Redo(StateName::Attributes(partial_attributes::Step::Tail))
}

/// In info or meta, at a byte that is text.
///
/// ```markdown
/// > | ~~~js{a
///          ^
/// ```
fn field_byte(tokenizer: &mut Tokenizer, fence: Fence, meta: bool) -> State {
    tokenizer.consume();
    State::Next(field_step(fence, meta))
}

/// In info or meta, after a backslash: an escaped byte is text.
///
/// ```markdown
/// > | ~~~js \{a}
///            ^
/// ```
fn field_escape(tokenizer: &mut Tokenizer, fence: Fence, meta: bool) -> State {
    match tokenizer.current {
        Some(byte) if byte.is_ascii_punctuation() && !(byte == b'`' && fence.marker == b'`') => {
            tokenizer.consume();
            State::Next(field_step(fence, meta))
        }
        _ => State::Redo(field_step(fence, meta)),
    }
}

/// The token of info or meta.
fn field_name(meta: bool) -> Name {
    if meta {
        Name::CodeFencedFenceMeta
    } else {
        Name::CodeFencedFenceInfo
    }
}

/// The step in info or meta.
fn field_step(fence: Fence, meta: bool) -> StateName {
    StateName::RawFlow(if meta {
        Step::Meta { fence }
    } else {
        Step::Info { fence }
    })
}

/// At eol/eof in raw, before a non-lazy closing fence or content.
///
/// ```markdown
/// > | ~~~js
///          ^
/// > | console.log(1)
///                   ^
///   | ~~~
/// ```
fn at_non_lazy_break(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    tokenizer.attempt(
        State::Next(StateName::RawFlow(Step::After)),
        State::Next(StateName::RawFlow(Step::ContentBefore { fence })),
    );
    tokenizer.enter(Name::LineEnding);
    tokenizer.consume();
    tokenizer.exit(Name::LineEnding);
    State::Next(StateName::RawFlow(Step::CloseStart { fence }))
}

/// Before closing fence, at optional whitespace.
///
/// ```markdown
///   | ~~~js
///   | console.log(1)
/// > | ~~~
///     ^
/// ```
fn close_start(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    tokenizer.enter(Name::CodeFencedFence);

    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::RawFlow(Step::BeforeSequenceClose { fence })),
            State::Nok,
        );

        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::RawFlow(Step::BeforeSequenceClose { fence }))
    }
}

/// In closing fence, after optional whitespace, at sequence.
///
/// ```markdown
///   | ~~~js
///   | console.log(1)
/// > | ~~~
///     ^
/// ```
fn before_sequence_close(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    if tokenizer.current == Some(fence.marker) {
        tokenizer.enter(Name::CodeFencedFenceSequence);
        State::Redo(StateName::RawFlow(Step::SequenceClose { fence, close: 0 }))
    } else {
        State::Nok
    }
}

/// In closing fence sequence.
///
/// ```markdown
///   | ~~~js
///   | console.log(1)
/// > | ~~~
///     ^
/// ```
fn sequence_close(tokenizer: &mut Tokenizer, fence: Fence, close: usize) -> State {
    if tokenizer.current == Some(fence.marker) {
        tokenizer.consume();
        State::Next(StateName::RawFlow(Step::SequenceClose {
            fence,
            close: close + 1,
        }))
    } else if close >= fence.size {
        tokenizer.exit(Name::CodeFencedFenceSequence);

        if matches!(tokenizer.current, Some(b'\t' | b' ')) {
            tokenizer.attempt(
                State::Next(StateName::RawFlow(Step::SequenceCloseAfter)),
                State::Nok,
            );
            State::Redo(space_or_tab())
        } else {
            State::Redo(StateName::RawFlow(Step::SequenceCloseAfter))
        }
    } else {
        State::Nok
    }
}

/// After closing fence sequence, after optional whitespace.
///
/// ```markdown
///   | ~~~js
///   | console.log(1)
/// > | ~~~
///        ^
/// ```
fn sequence_close_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::CodeFencedFence);
            State::Ok
        }
        _ => State::Nok,
    }
}

/// Before raw content, not a closing fence, at eol.
///
/// ```markdown
///   | ~~~js
/// > | console.log(1)
///                   ^
///   | ~~~
/// ```
fn content_before(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    tokenizer.enter(Name::LineEnding);
    tokenizer.consume();
    tokenizer.exit(Name::LineEnding);
    State::Next(StateName::RawFlow(Step::ContentStart { fence }))
}

/// Before raw content, not a closing fence.
///
/// ```markdown
///   | ~~~js
/// > | console.log(1)
///     ^
///   | ~~~
/// ```
fn content_start(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::RawFlow(Step::BeforeContentChunk { fence })),
            State::Nok,
        );
        State::Redo(space_or_tab_min_max(0, fence.prefix))
    } else {
        State::Redo(StateName::RawFlow(Step::BeforeContentChunk { fence }))
    }
}

/// Before raw content, after optional prefix.
///
/// ```markdown
///   | ~~~js
/// > | console.log(1)
///     ^
///   | ~~~
/// ```
fn before_content_chunk(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.check(
                State::Next(StateName::RawFlow(Step::AtNonLazyBreak { fence })),
                State::Next(StateName::RawFlow(Step::After)),
            );
            State::Redo(StateName::NonLazyContinuation(
                partial_non_lazy_continuation::Step::Start,
            ))
        }
        _ => {
            tokenizer.enter(Name::CodeFlowChunk);
            State::Redo(StateName::RawFlow(Step::ContentChunk { fence }))
        }
    }
}

/// In raw content.
///
/// ```markdown
///   | ~~~js
/// > | console.log(1)
///     ^^^^^^^^^^^^^^
///   | ~~~
/// ```
fn content_chunk(tokenizer: &mut Tokenizer, fence: Fence) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::CodeFlowChunk);
            State::Redo(StateName::RawFlow(Step::BeforeContentChunk { fence }))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::RawFlow(Step::ContentChunk { fence }))
        }
    }
}

/// After raw.
///
/// ```markdown
///   | ~~~js
///   | console.log(1)
/// > | ~~~
///        ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::CodeFenced);
    // Feel free to interrupt.
    tokenizer.interrupt = false;
    // No longer concrete.
    tokenizer.concrete = false;
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    BeforeSequenceOpen,
    SequenceOpen { fence: Fence },
    InfoBefore { fence: Fence },
    Info { fence: Fence },
    MetaBefore { fence: Fence },
    Meta { fence: Fence },
    FieldOpen { fence: Fence, meta: bool },
    FieldClose { fence: Fence, meta: bool },
    FieldByte { fence: Fence, meta: bool },
    FieldEscape { fence: Fence, meta: bool },
    AtNonLazyBreak { fence: Fence },
    CloseStart { fence: Fence },
    BeforeSequenceClose { fence: Fence },
    SequenceClose { fence: Fence, close: usize },
    SequenceCloseAfter,
    ContentBefore { fence: Fence },
    ContentStart { fence: Fence },
    BeforeContentChunk { fence: Fence },
    ContentChunk { fence: Fence },
    After,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::RawFlow(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::BeforeSequenceOpen => before_sequence_open(tokenizer),
        Step::SequenceOpen { fence } => sequence_open(tokenizer, fence),
        Step::InfoBefore { fence } => info_before(tokenizer, fence),
        Step::Info { fence } => info(tokenizer, fence),
        Step::MetaBefore { fence } => meta_before(tokenizer, fence),
        Step::Meta { fence } => meta(tokenizer, fence),
        Step::FieldOpen { fence, meta } => field_open(tokenizer, fence, meta),
        Step::FieldClose { fence, meta } => field_close(tokenizer, fence, meta),
        Step::FieldByte { fence, meta } => field_byte(tokenizer, fence, meta),
        Step::FieldEscape { fence, meta } => field_escape(tokenizer, fence, meta),
        Step::AtNonLazyBreak { fence } => at_non_lazy_break(tokenizer, fence),
        Step::CloseStart { fence } => close_start(tokenizer, fence),
        Step::BeforeSequenceClose { fence } => before_sequence_close(tokenizer, fence),
        Step::SequenceClose { fence, close } => sequence_close(tokenizer, fence, close),
        Step::SequenceCloseAfter => sequence_close_after(tokenizer),
        Step::ContentBefore { fence } => content_before(tokenizer, fence),
        Step::ContentStart { fence } => content_start(tokenizer, fence),
        Step::BeforeContentChunk { fence } => before_content_chunk(tokenizer, fence),
        Step::ContentChunk { fence } => content_chunk(tokenizer, fence),
        Step::After => after(tokenizer),
    }
}
