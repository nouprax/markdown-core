//! The text content type.
//!
//! **Text** contains phrasing content such as
//! [attention][crate::construct::attention] (emphasis, gfm strikethrough, strong),
//! [raw (text)][crate::construct::raw_text] (code (text), math (text)), and actual text.
//!
//! The constructs found in text are:
//!
//! * [Attention][crate::construct::attention] (emphasis, gfm strikethrough, strong)
//! * [Autolink][crate::construct::autolink]
//! * [Character escape][crate::construct::character_escape]
//! * [Character reference][crate::construct::character_reference]
//! * [Raw (text)][crate::construct::raw_text] (code (text), math (text))
//! * [GFM: Label start (footnote)][crate::construct::gfm_label_start_footnote]
//! * [GFM: Task list item check][crate::construct::gfm_task_list_item_check]
//! * [Hard break (escape)][crate::construct::hard_break_escape]
//! * [HTML (text)][crate::construct::html_text]
//! * [Label start (image)][crate::construct::label_start_image]
//! * [Label start (link)][crate::construct::label_start_link]
//! * [Label end][crate::construct::label_end]
//!
//! > 👉 **Note**: for performance reasons, hard break (trailing) is formed by
//! > [whitespace][crate::construct::partial_whitespace].

use crate::construct::gfm_autolink_literal::resolve as resolve_gfm_autolink_literal;
use crate::construct::partial_whitespace::resolve_whitespace;
use crate::construct::{
    attention, autolink, character_escape, character_reference, gfm_autolink_literal,
    gfm_label_start_footnote, gfm_task_list_item_check, hard_break_escape, html_text, label_end,
    label_start_image, label_start_link, partial_data, raw_text,
};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::Tokenizer;

/// Characters that can start something in text.
const MARKERS: [u8; 15] = [
    b'!',  // `label_start_image`
    b'$',  // `raw_text` (math (text))
    b'&',  // `character_reference`
    b'*',  // `attention` (emphasis, strong)
    b'<',  // `autolink`, `html_text`
    b'H',  // `gfm_autolink_literal` (`protocol` kind)
    b'W',  // `gfm_autolink_literal` (`www.` kind)
    b'[',  // `label_start_link`
    b'\\', // `character_escape`, `hard_break_escape`
    b']',  // `label_end`, `gfm_label_start_footnote`
    b'_',  // `attention` (emphasis, strong)
    b'`',  // `raw_text` (code (text))
    b'h',  // `gfm_autolink_literal` (`protocol` kind)
    b'w',  // `gfm_autolink_literal` (`www.` kind)
    b'~',  // `attention` (gfm strikethrough)
];

/// Start of text.
///
/// There is a slightly weird case where task list items have their check at
/// the start of the first paragraph.
/// So we start by checking for that.
///
/// ```markdown
/// > | abc
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Text(Step::Before)),
        State::Next(StateName::Text(Step::Before)),
    );
    State::Retry(StateName::GfmTaskListItemCheck(
        gfm_task_list_item_check::Step::Start,
    ))
}

/// Before text.
///
/// ```markdown
/// > | abc
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => {
            tokenizer.register_resolver(ResolveName::Data);
            tokenizer.register_resolver(ResolveName::Text);
            State::Ok
        }
        Some(b'!') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeData)),
            );
            State::Retry(StateName::LabelStartImage(label_start_image::Step::Start))
        }
        // raw (text) (code (text), math (text))
        Some(b'$' | b'`') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeData)),
            );
            State::Retry(StateName::RawText(raw_text::Step::Start))
        }
        Some(b'&') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeData)),
            );
            State::Retry(StateName::CharacterReference(
                character_reference::Step::Start,
            ))
        }
        // attention (emphasis, gfm strikethrough, strong)
        Some(b'*' | b'_' | b'~') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeData)),
            );
            State::Retry(StateName::Attention(attention::Step::Start))
        }
        // `autolink`, `html_text` (order does not matter).
        Some(b'<') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeHtml)),
            );
            State::Retry(StateName::Autolink(autolink::Step::Start))
        }
        Some(b'H' | b'h') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeData)),
            );
            State::Retry(StateName::GfmAutolinkLiteral(
                gfm_autolink_literal::Step::ProtocolStart,
            ))
        }
        Some(b'W' | b'w') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeData)),
            );
            State::Retry(StateName::GfmAutolinkLiteral(
                gfm_autolink_literal::Step::WwwStart,
            ))
        }
        Some(b'[') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeLabelStartLink)),
            );
            State::Retry(StateName::GfmLabelStartFootnote(
                gfm_label_start_footnote::Step::Start,
            ))
        }
        Some(b'\\') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeHardBreakEscape)),
            );
            State::Retry(StateName::CharacterEscape(character_escape::Step::Start))
        }
        Some(b']') => {
            tokenizer.attempt(
                State::Next(StateName::Text(Step::Before)),
                State::Next(StateName::Text(Step::BeforeData)),
            );
            State::Retry(StateName::LabelEnd(label_end::Step::Start))
        }
        _ => State::Retry(StateName::Text(Step::BeforeData)),
    }
}

/// Before html (text).
///
/// At `<`, which wasn’t an autolink.
///
/// ```markdown
/// > | a <b>
///       ^
/// ```
fn before_html(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Text(Step::Before)),
        State::Next(StateName::Text(Step::BeforeData)),
    );
    State::Retry(StateName::HtmlText(html_text::Step::Start))
}

/// Before hard break escape.
///
/// At `\`, which wasn’t a character escape.
///
/// ```markdown
/// > | a \␊
///       ^
/// ```
fn before_hard_break_escape(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Text(Step::Before)),
        State::Next(StateName::Text(Step::BeforeData)),
    );
    State::Retry(StateName::HardBreakEscape(hard_break_escape::Step::Start))
}

/// Before label start (link).
///
/// At `[`, which wasn’t a GFM label start (footnote).
///
/// ```markdown
/// > | [a](b)
///     ^
/// ```
fn before_label_start_link(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Text(Step::Before)),
        State::Next(StateName::Text(Step::BeforeData)),
    );
    State::Retry(StateName::LabelStartLink(label_start_link::Step::Start))
}

/// Before data.
///
/// ```markdown
/// > | a
///     ^
/// ```
fn before_data(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(State::Next(StateName::Text(Step::Before)), State::Nok);
    State::Retry(StateName::Data(partial_data::Step::Start {
        markers: &MARKERS,
    }))
}

/// Resolve whitespace.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    resolve_whitespace(tokenizer, true, true);

    resolve_gfm_autolink_literal(tokenizer);

    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    BeforeHtml,
    BeforeHardBreakEscape,
    BeforeLabelStartLink,
    BeforeData,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Text(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::BeforeHtml => before_html(tokenizer),
        Step::BeforeHardBreakEscape => before_hard_break_escape(tokenizer),
        Step::BeforeLabelStartLink => before_label_start_link(tokenizer),
        Step::BeforeData => before_data(tokenizer),
    }
}
