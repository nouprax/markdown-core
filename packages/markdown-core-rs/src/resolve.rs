//! Resolve events.

use crate::construct;
use crate::subtokenize::Subresult;
use crate::tokenizer::Tokenizer;

/// Names of resolvers.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Name {
    /// Resolve labels.
    ///
    /// Labels are parsed as starts and ends, and when they match, merged
    /// together to form media (links and images), and otherwise turned into
    /// data.
    Label,
    /// Resolve attention.
    ///
    /// Attention sequences are parsed and finally matched together to form
    /// attention (emphasis and strong) based on which characters they contain,
    /// and what occurs before and after each sequence.
    /// Otherwise they are turned into data.
    Attention,
    /// Resolve GFM tables.
    ///
    /// The table head, and later each row, are all parsed separately.
    /// Resolving groups everything together, and groups cells.
    Table,
    /// Resolve heading (atx).
    ///
    /// Heading (atx) contains further sequences and data.
    /// At the end, a final sequence is kept that way, while the rest is merged
    /// with the data.
    HeaderAtx,
    /// Resolve heading (setext).
    ///
    /// Heading (setext) is parsed as an underline that is preceded by content,
    /// both will form the whole construct.
    HeaderSetext,
    /// Resolve list item.
    ///
    /// List items are parsed on their own.
    /// They are wrapped into ordered or unordered lists based on whether items
    /// with the same marker occur next to each other.
    ListItem,
    /// Resolve content.
    ///
    /// Content is parsed as single lines, as what remains if other flow
    /// constructs don’t match.
    /// But, when they occur next to each other, they need to be merged.
    Content,
    /// Resolve data.
    ///
    /// Data is parsed as many small bits, due to many punctuation characters
    /// potentially starting something in particularly text content.
    /// It helps performance to merge them together if those markers did not
    /// match anything and hence they occur next to each other.
    Data,
    /// Resolve whitespace in `plain`.
    Plain,
    /// Resolve whitespace in `text`.
    Text,
}

/// Call the corresponding resolver.
pub fn call(tokenizer: &mut Tokenizer, name: Name) -> Option<Subresult> {
    match name {
        Name::Label => construct::label_end::resolve(tokenizer),
        Name::Attention => construct::attention::resolve(tokenizer),
        Name::Table => construct::table::resolve(tokenizer),
        Name::HeaderAtx => construct::header_atx::resolve(tokenizer),
        Name::HeaderSetext => construct::header_setext::resolve(tokenizer),
        Name::ListItem => construct::list_item::resolve(tokenizer),
        Name::Content => Some(construct::content::resolve(tokenizer)),
        Name::Data => construct::partial_data::resolve(tokenizer),
        Name::Plain => construct::plain::resolve(tokenizer),
        Name::Text => construct::text::resolve(tokenizer),
    }
}
