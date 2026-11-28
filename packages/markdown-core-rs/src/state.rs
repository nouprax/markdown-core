//! States of the state machine.

use crate::construct;
use crate::tokenizer::Tokenizer;

/// Result of a state.
#[derive(Clone, Debug, PartialEq)]
pub enum State {
    /// Move to [`Name`][] next.
    Next(Name),
    /// Retry in [`Name`][].
    Redo(Name),
    /// The state is successful.
    Ok,
    /// The state is not successful.
    Nok,
}

/// Names of states to move to.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[allow(clippy::enum_variant_names)]
pub enum Name {
    Ahead(crate::ahead::Step),
    Attention(construct::attention::Step),
    Autolink(construct::autolink::Step),
    BlankLine(construct::blank_line::Step),
    BlockQuote(construct::block_quote::Step),
    Choice(crate::choice::Step),
    CharacterEscape(construct::character_escape::Step),
    CharacterReference(construct::character_reference::Step),
    Callout(construct::callout::Step),
    CodeIndented(construct::code_indented::Step),
    Comment(construct::comment::Step),
    CrossLink(construct::cross_link::Step),
    Citation(construct::citation::Step),
    DirectiveText(construct::directive_text::Step),
    DirectiveLeaf(construct::directive_leaf::Step),
    DirectiveContainer(construct::directive_container::Step),
    Directive(construct::parts::directive::Step),
    Topic(construct::topic::Step),
    Attributes(construct::parts::attributes::Step),
    BlockIdentifier(construct::block_identifier::Step),
    LabelStartNote(construct::label_start_note::Step),
    Formula(construct::formula::Step),
    Content(construct::content::Step),
    Data(construct::parts::data::Step),
    Definition(construct::definition::Step),
    Destination(construct::parts::destination::Step),
    Document(construct::document::Step),
    Flow(construct::flow::Step),
    Frontmatter(construct::frontmatter::Step),
    AutolinkLiteral(construct::autolink_literal::Step),
    FootnoteDefinition(construct::footnote_definition::Step),
    LabelStartFootnote(construct::label_start_footnote::Step),
    Table(construct::table::Step),
    TaskListItemCheck(construct::task_list_item_check::Step),
    HardBreakEscape(construct::hard_break_escape::Step),
    HeadingAtx(construct::heading_atx::Step),
    HeadingSetext(construct::heading_setext::Step),
    HtmlFlow(construct::html_flow::Step),
    HtmlText(construct::html_text::Step),
    Label(construct::parts::label::Step),
    LabelEnd(construct::label_end::Step),
    LabelStartImage(construct::label_start_image::Step),
    LabelStartLink(construct::label_start_link::Step),
    ListItem(construct::list_item::Step),
    DefinitionBody(construct::definition_body::Step),
    DefinitionTerm(construct::definition_term::Step),
    NonLazyContinuation(construct::parts::non_lazy_continuation::Step),
    Paragraph(construct::paragraph::Step),
    RawFlow(construct::raw_flow::Step),
    RawText(construct::raw_text::Step),
    String(construct::string::Step),
    Text(construct::text::Step),
    ThematicBreak(construct::thematic_break::Step),
    Title(construct::parts::title::Step),
    SpaceOrTab(construct::parts::space_or_tab::Step),
    SpaceOrTabEol(construct::parts::space_or_tab_eol::Step),
}

#[allow(clippy::too_many_lines)]
/// Call the corresponding state for a state name.
pub fn call(tokenizer: &mut Tokenizer, name: Name) -> State {
    match name {
        Name::Ahead(step) => crate::ahead::step(tokenizer, step),
        Name::Attention(step) => construct::attention::step(tokenizer, step),
        Name::Autolink(step) => construct::autolink::step(tokenizer, step),
        Name::BlankLine(step) => construct::blank_line::step(tokenizer, step),
        Name::BlockQuote(step) => construct::block_quote::step(tokenizer, step),
        Name::Choice(step) => crate::choice::step(tokenizer, step),
        Name::CharacterEscape(step) => construct::character_escape::step(tokenizer, step),
        Name::CharacterReference(step) => construct::character_reference::step(tokenizer, step),
        Name::Callout(step) => construct::callout::step(tokenizer, step),
        Name::CodeIndented(step) => construct::code_indented::step(tokenizer, step),
        Name::Comment(step) => construct::comment::step(tokenizer, step),
        Name::CrossLink(step) => construct::cross_link::step(tokenizer, step),
        Name::Citation(step) => construct::citation::step(tokenizer, step),
        Name::DirectiveText(step) => construct::directive_text::step(tokenizer, step),
        Name::DirectiveLeaf(step) => construct::directive_leaf::step(tokenizer, step),
        Name::DirectiveContainer(step) => construct::directive_container::step(tokenizer, step),
        Name::Directive(step) => construct::parts::directive::step(tokenizer, step),
        Name::Topic(step) => construct::topic::step(tokenizer, step),
        Name::Attributes(step) => construct::parts::attributes::step(tokenizer, step),
        Name::BlockIdentifier(step) => construct::block_identifier::step(tokenizer, step),
        Name::LabelStartNote(step) => construct::label_start_note::step(tokenizer, step),
        Name::Formula(step) => construct::formula::step(tokenizer, step),
        Name::Content(step) => construct::content::step(tokenizer, step),
        Name::Data(step) => construct::parts::data::step(tokenizer, step),
        Name::Definition(step) => construct::definition::step(tokenizer, step),
        Name::Destination(step) => construct::parts::destination::step(tokenizer, step),
        Name::Document(step) => construct::document::step(tokenizer, step),
        Name::Flow(step) => construct::flow::step(tokenizer, step),
        Name::Frontmatter(step) => construct::frontmatter::step(tokenizer, step),
        Name::AutolinkLiteral(step) => construct::autolink_literal::step(tokenizer, step),
        Name::FootnoteDefinition(step) => construct::footnote_definition::step(tokenizer, step),
        Name::LabelStartFootnote(step) => construct::label_start_footnote::step(tokenizer, step),
        Name::Table(step) => construct::table::step(tokenizer, step),
        Name::TaskListItemCheck(step) => construct::task_list_item_check::step(tokenizer, step),
        Name::HardBreakEscape(step) => construct::hard_break_escape::step(tokenizer, step),
        Name::HeadingAtx(step) => construct::heading_atx::step(tokenizer, step),
        Name::HeadingSetext(step) => construct::heading_setext::step(tokenizer, step),
        Name::HtmlFlow(step) => construct::html_flow::step(tokenizer, step),
        Name::HtmlText(step) => construct::html_text::step(tokenizer, step),
        Name::Label(step) => construct::parts::label::step(tokenizer, step),
        Name::LabelEnd(step) => construct::label_end::step(tokenizer, step),
        Name::LabelStartImage(step) => construct::label_start_image::step(tokenizer, step),
        Name::LabelStartLink(step) => construct::label_start_link::step(tokenizer, step),
        Name::ListItem(step) => construct::list_item::step(tokenizer, step),
        Name::DefinitionBody(step) => construct::definition_body::step(tokenizer, step),
        Name::DefinitionTerm(step) => construct::definition_term::step(tokenizer, step),
        Name::NonLazyContinuation(step) => {
            construct::parts::non_lazy_continuation::step(tokenizer, step)
        }
        Name::Paragraph(step) => construct::paragraph::step(tokenizer, step),
        Name::RawFlow(step) => construct::raw_flow::step(tokenizer, step),
        Name::RawText(step) => construct::raw_text::step(tokenizer, step),
        Name::String(step) => construct::string::step(tokenizer, step),
        Name::Text(step) => construct::text::step(tokenizer, step),
        Name::ThematicBreak(step) => construct::thematic_break::step(tokenizer, step),
        Name::Title(step) => construct::parts::title::step(tokenizer, step),
        Name::SpaceOrTab(step) => construct::parts::space_or_tab::step(tokenizer, step),
        Name::SpaceOrTabEol(step) => construct::parts::space_or_tab_eol::step(tokenizer, step),
    }
}
