//! Label start (image) occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Label start (image) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! label_start_image ::= '!' '['
//! ```
//!
//! ## HTML
//!
//! Label start (image) does not, on its own, relate to anything in HTML.
//! When matched with a [label end][label_end], they together relate to the
//! `<img>` element in HTML.
//! See [*§ 4.8.3 The `img` element*][html_img] in the HTML spec for more info.
//! Without an end, the characters (`![`) are output.
//!
//! ## Tokens
//!
//! * [`LabelPicture`][Name::LabelPicture]
//! * [`LabelPictureMarker`][Name::LabelPictureMarker]
//! * [`LabelMarker`][Name::LabelMarker]
//!
//! ## References
//!
//! * [`label-start-image.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/label-start-image.js)
//! * [*§ 6.4 Images* in `CommonMark`](https://spec.commonmark.org/0.31/#images)
//!
//! [text]: crate::construct::text
//! [label_end]: crate::construct::label_end
//! [html_img]: https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element

use crate::choice::Starts;
use crate::event::Name;
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::tokenizer::{LabelKind, LabelStart, Tokenizer};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Codes(b"!");

/// Start of label (image) start.
///
/// ```markdown
/// > | a ![b] c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'!') {
        tokenizer.enter(Name::LabelPicture);
        tokenizer.enter(Name::LabelPictureMarker);
        tokenizer.consume();
        tokenizer.exit(Name::LabelPictureMarker);
        State::Next(StateName::LabelStartPicture(Step::Open))
    } else {
        State::Nok
    }
}

/// After `!`, at `[`.
///
/// ```markdown
/// > | a ![b] c
///        ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'[') => {
            tokenizer.enter(Name::LabelMarker);
            tokenizer.consume();
            tokenizer.exit(Name::LabelMarker);
            tokenizer.exit(Name::LabelPicture);
            State::Next(StateName::LabelStartPicture(Step::After))
        }
        _ => State::Nok,
    }
}

/// After `![`.
///
/// ```markdown
/// > | a ![b] c
///         ^
/// ```
///
/// This is needed in because, when GFM footnotes are enabled, images never
/// form when started with a `^`.
/// Instead, links form:
///
/// ```markdown
/// ![^a](b)
///
/// ![^a][b]
///
/// [b]: c
/// ```
///
/// ```html
/// <p>!<a href=\"b\">^a</a></p>
/// <p>!<a href=\"c\">^a</a></p>
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'^') {
        State::Nok
    } else {
        tokenizer.labels.starts.push(LabelStart {
            kind: LabelKind::Picture,
            start: (tokenizer.events.len() - 6, tokenizer.events.len() - 1),
            inactive: false,
        });
        tokenizer.add_resolver_before(ResolveName::Label);
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
        StateName::LabelStartPicture(step)
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
