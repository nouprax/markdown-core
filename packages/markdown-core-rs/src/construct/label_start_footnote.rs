//! Label start (footnote) occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Label start (footnote) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: not followed by `[`, which makes `^[` an inline note.
//! label_start_footnote ::= '[' '^'
//! ```
//!
//! ## HTML
//!
//! Label start (footnote) does not, on its own, relate to anything in HTML.
//! When matched with a [label end][label_end], they together relate to `<sup>`
//! and `<a>` elements in HTML.
//! See [*§ 4.5.19 The `sub` and `sup` elements*][html_sup] and
//! [*§ 4.5.1 The `a` element*][html_a] in the HTML spec for more info.
//! Without an end, the characters (`[^`) are output.
//!
//! ## Tokens
//!
//! * [`FootnoteCallLabel`][Name::FootnoteCallLabel]
//! * [`FootnoteCallMarker`][Name::FootnoteCallMarker]
//! * [`LabelMarker`][Name::LabelMarker]
//!
//! ## References
//!
//! * [`micromark-extension-gfm-footnote`](https://github.com/micromark/micromark-extension-gfm-footnote)
//!
//! > 👉 **Note**: Footnotes are not specified in GFM yet.
//! > See [`github/cmark-gfm#270`](https://github.com/github/cmark-gfm/issues/270)
//! > for the related issue.
//!
//! [text]: crate::construct::text
//! [label_end]: crate::construct::label_end
//! [html_a]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-a-element
//! [html_sup]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-sub-and-sup-elements

use crate::choice::Starts;
use crate::event::Name;
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::tokenizer::{LabelKind, LabelStart, Tokenizer};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b"[");

/// Start of label (footnote) start.
///
/// ```markdown
/// > | a [^b] c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'[') {
        tokenizer.enter(Name::FootnoteCallLabel);
        tokenizer.enter(Name::LabelMarker);
        tokenizer.consume();
        tokenizer.exit(Name::LabelMarker);
        State::Next(StateName::LabelStartFootnote(Step::Open))
    } else {
        State::Nok
    }
}

/// After `[`, at `^`.
///
/// ```markdown
/// > | a [^b] c
///        ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'^') => {
            tokenizer.enter(Name::FootnoteCallMarker);
            tokenizer.consume();
            tokenizer.exit(Name::FootnoteCallMarker);
            State::Next(StateName::LabelStartFootnote(Step::After))
        }
        _ => State::Nok,
    }
}

/// After `[^`.
///
/// ```markdown
/// > | a [^b] c
///         ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    // `^[` opens an inline note instead.
    if tokenizer.current == Some(b'[') {
        State::Nok
    } else {
        tokenizer.exit(Name::FootnoteCallLabel);
        tokenizer.labels.starts.push(LabelStart {
            kind: LabelKind::Footnote,
            start: (tokenizer.events.len() - 6, tokenizer.events.len() - 1),
            inactive: false,
        });
        tokenizer.register_resolver_before(ResolveName::Label);
        State::Ok
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Open,
    After,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::LabelStartFootnote(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Open => open(tokenizer),
        Step::After => after(tokenizer),
    }
}
