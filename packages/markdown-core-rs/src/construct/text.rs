//! The text content type.
//!
//! **Text** contains phrasing content such as
//! [attention] (emphasis, gfm strikethrough, strong),
//! [raw (text)][crate::construct::raw_text] (code (text)), formulas, and actual text.
//!
//! The constructs found in text are:
//!
//! * [Attention][crate::construct::attention] (emphasis, gfm strikethrough, strong)
//! * [Autolink][crate::construct::autolink]
//! * [Character escape][crate::construct::character_escape]
//! * [Character reference][crate::construct::character_reference]
//! * [Comment][crate::construct::comment]
//! * [Cross link][crate::construct::cross_link]
//! * [Topic][crate::construct::topic]
//! * [Formula][crate::construct::formula]
//! * [Raw (text)][crate::construct::raw_text] (code (text))
//! * [GFM: Label start (footnote)][crate::construct::label_start_footnote]
//! * [GFM: Task list item check][crate::construct::task_list_item_check]
//! * [Hard break (escape)][crate::construct::hard_break_escape]
//! * [HTML (text)][crate::construct::html_text]
//! * [Label start (image)][crate::construct::label_start_image]
//! * [Label start (link)][crate::construct::label_start_link]
//! * [Label start (note)][crate::construct::label_start_note]
//! * [Label end][crate::construct::label_end]
//!
//! > 👉 **Note**: for performance reasons, hard break (trailing) is formed by
//! > [whitespace][crate::construct::partial_whitespace].

use crate::choice::{self, Choice, Entry, List};
use crate::construct::autolink_literal::resolve as resolve_autolink_literal;
use crate::construct::partial_whitespace::resolve_whitespace;
use crate::construct::{
    attention, autolink, autolink_literal, character_escape, character_reference, citation,
    comment, cross_link, directive_text, formula, hard_break_escape, html_text, label_end,
    label_start_footnote, label_start_image, label_start_link, label_start_note, partial_data,
    raw_text, task_list_item_check, topic,
};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::Tokenizer;

/// Constructs in text, in the order they are tried.
///
/// Data when none of these matched.
pub const CHOICE: Choice = Choice::new(
    &[
        Entry {
            starts: cross_link::STARTS,
            step: StateName::CrossLink(cross_link::Step::Start),
        },
        Entry {
            starts: label_start_image::STARTS,
            step: StateName::LabelStartImage(label_start_image::Step::Start),
        },
        Entry {
            starts: formula::STARTS,
            step: StateName::Formula(formula::Step::Start),
        },
        Entry {
            starts: raw_text::STARTS,
            step: StateName::RawText(raw_text::Step::Start),
        },
        Entry {
            starts: character_reference::STARTS,
            step: StateName::CharacterReference(character_reference::Step::Start),
        },
        Entry {
            starts: comment::STARTS,
            step: StateName::Comment(comment::Step::Start),
        },
        Entry {
            starts: directive_text::STARTS,
            step: StateName::DirectiveText(directive_text::Step::Start),
        },
        Entry {
            starts: citation::STARTS,
            step: StateName::Citation(citation::Step::Start),
        },
        Entry {
            starts: topic::STARTS,
            step: StateName::Topic(topic::Step::Start),
        },
        Entry {
            starts: label_start_note::STARTS,
            step: StateName::LabelStartNote(label_start_note::Step::Start),
        },
        Entry {
            starts: attention::STARTS,
            step: StateName::Attention(attention::Step::Start),
        },
        Entry {
            starts: autolink::STARTS,
            step: StateName::Autolink(autolink::Step::Start),
        },
        Entry {
            starts: html_text::STARTS,
            step: StateName::HtmlText(html_text::Step::Start),
        },
        Entry {
            starts: autolink_literal::PROTOCOL,
            step: StateName::AutolinkLiteral(autolink_literal::Step::ProtocolStart),
        },
        Entry {
            starts: autolink_literal::WWW,
            step: StateName::AutolinkLiteral(autolink_literal::Step::WwwStart),
        },
        Entry {
            starts: label_start_footnote::STARTS,
            step: StateName::LabelStartFootnote(label_start_footnote::Step::Start),
        },
        Entry {
            starts: label_start_link::STARTS,
            step: StateName::LabelStartLink(label_start_link::Step::Start),
        },
        Entry {
            starts: character_escape::STARTS,
            step: StateName::CharacterEscape(character_escape::Step::Start),
        },
        Entry {
            starts: hard_break_escape::STARTS,
            step: StateName::HardBreakEscape(hard_break_escape::Step::Start),
        },
        Entry {
            starts: label_end::STARTS,
            step: StateName::LabelEnd(label_end::Step::Start),
        },
    ],
    StateName::Text(Step::Before),
    StateName::Text(Step::BeforeData),
);

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
    State::Redo(StateName::TaskListItemCheck(
        task_list_item_check::Step::Start,
    ))
}

/// Before text.
///
/// ```markdown
/// > | abc
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current.is_none() {
        tokenizer.register_resolver(ResolveName::Data);
        tokenizer.register_resolver(ResolveName::Text);
        State::Ok
    } else {
        State::Redo(choice::start(List::Text))
    }
}

/// Before data.
///
/// ```markdown
/// > | a
///     ^
/// ```
fn before_data(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(State::Next(StateName::Text(Step::Before)), State::Nok);
    State::Redo(StateName::Data(partial_data::Step::Start {
        list: List::Text,
    }))
}

/// Resolve whitespace.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    resolve_whitespace(tokenizer, true, true);

    resolve_autolink_literal(tokenizer);

    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
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
        Step::BeforeData => before_data(tokenizer),
    }
}
