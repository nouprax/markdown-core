//! The canonical AST (`docs/specs/canonical-ast.json`).
//!
//! Every node kind is one variant of [`Kind`], carrying exactly the fields
//! the contract gives that kind, so a consumer that matches on [`Kind`]
//! handles every kind or does not compile.
//! A node's source is its [`Node::runs`]: byte ranges of the document source,
//! in source order.

use alloc::{boxed::Box, string::String, vec, vec::Vec};

/// A byte span of the document source.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Span {
    pub start: usize,
    pub end: usize,
}

impl Span {
    pub fn new(start: usize, end: usize) -> Span {
        debug_assert!(start <= end, "expected a forward span");
        Span { start, end }
    }
}

/// One canonical AST node.
#[derive(Clone, Debug, PartialEq)]
pub struct Node {
    /// The node's own source, in source order; never empty.
    pub runs: Vec<Span>,
    pub anchor: Option<String>,
    pub attributes: Attributes,
    pub kind: Kind,
}

impl Node {
    pub fn new(kind: Kind, runs: Vec<Span>) -> Node {
        debug_assert!(!runs.is_empty(), "expected a node to have a run");
        Node {
            runs,
            anchor: None,
            attributes: Attributes::default(),
            kind,
        }
    }

    /// Where the node's source starts.
    pub fn start(&self) -> usize {
        self.runs[0].start
    }

    /// Where the node's source ends.
    pub fn end(&self) -> usize {
        self.runs[self.runs.len() - 1].end
    }

    /// The nodes this node owns, in canonical walk order.
    pub fn owned(&self) -> Vec<&Node> {
        let mut list: Vec<&Node> = vec![];
        match &self.kind {
            Kind::Document { metadata, content } => {
                list.extend(metadata.iter().map(|node| &**node));
                list.extend(content);
            }
            Kind::Callout { title, content, .. } => {
                list.extend(title.iter().flatten());
                list.extend(content);
            }
            Kind::Paragraph { content }
            | Kind::Heading { content, .. }
            | Kind::ListItem { content, .. }
            | Kind::TableCell { content }
            | Kind::DirectiveLabel { content }
            | Kind::Emphasis { content }
            | Kind::Strong { content }
            | Kind::Strikethrough { content }
            | Kind::Mark { content }
            | Kind::Insertion { content }
            | Kind::Span { content }
            | Kind::Superscript { content }
            | Kind::Subscript { content }
            | Kind::Link { content, .. }
            | Kind::Embed { content, .. }
            | Kind::Footnote { content, .. }
            | Kind::Specimen { content, .. } => list.extend(content),
            Kind::List { items, .. } => list.extend(items),
            Kind::Table { head, content, .. } => {
                list.extend(head);
                list.extend(content);
            }
            Kind::TableRow { cells } => list.extend(cells),
            Kind::DirectiveBlock { label, content, .. } => {
                list.extend(label.iter().map(|node| &**node));
                list.extend(content);
            }
            Kind::Directive { label, .. } => list.extend(label.iter().map(|node| &**node)),
            Kind::Cite { citations } => list.extend(citations),
            Kind::DefinitionList { definitions } => list.extend(definitions),
            Kind::Definition { term, bodies, .. } => {
                list.extend(term);
                list.extend(bodies.iter().flatten());
            }
            Kind::Citation {
                referent,
                prefix,
                suffix,
            } => {
                if let Referent::Footnote(Note::Inline(note)) = referent {
                    list.push(note);
                }
                list.extend(prefix);
                list.extend(suffix);
            }
            Kind::ThematicBreak
            | Kind::CodeBlock { .. }
            | Kind::HtmlBlock { .. }
            | Kind::FormulaBlock { .. }
            | Kind::Text { .. }
            | Kind::SoftBreak
            | Kind::LineBreak
            | Kind::Code { .. }
            | Kind::Html { .. }
            | Kind::CrossLink { .. }
            | Kind::CrossEmbed { .. }
            | Kind::Comment { .. }
            | Kind::Formula { .. }
            | Kind::Topic { .. }
            | Kind::Metadata { .. }
            | Kind::Reference { .. } => {}
        }
        list
    }

    /// The nodes this node owns, mutably, in canonical walk order.
    pub fn owned_mut(&mut self) -> Vec<&mut Node> {
        let mut list: Vec<&mut Node> = vec![];
        match &mut self.kind {
            Kind::Document { metadata, content } => {
                list.extend(metadata.iter_mut().map(|node| &mut **node));
                list.extend(content);
            }
            Kind::Callout { title, content, .. } => {
                list.extend(title.iter_mut().flatten());
                list.extend(content);
            }
            Kind::Paragraph { content }
            | Kind::Heading { content, .. }
            | Kind::ListItem { content, .. }
            | Kind::TableCell { content }
            | Kind::DirectiveLabel { content }
            | Kind::Emphasis { content }
            | Kind::Strong { content }
            | Kind::Strikethrough { content }
            | Kind::Mark { content }
            | Kind::Insertion { content }
            | Kind::Span { content }
            | Kind::Superscript { content }
            | Kind::Subscript { content }
            | Kind::Link { content, .. }
            | Kind::Embed { content, .. }
            | Kind::Footnote { content, .. }
            | Kind::Specimen { content, .. } => list.extend(content),
            Kind::List { items, .. } => list.extend(items),
            Kind::Table { head, content, .. } => {
                list.extend(head);
                list.extend(content);
            }
            Kind::TableRow { cells } => list.extend(cells),
            Kind::DirectiveBlock { label, content, .. } => {
                list.extend(label.iter_mut().map(|node| &mut **node));
                list.extend(content);
            }
            Kind::Directive { label, .. } => list.extend(label.iter_mut().map(|node| &mut **node)),
            Kind::Cite { citations } => list.extend(citations),
            Kind::DefinitionList { definitions } => list.extend(definitions),
            Kind::Definition { term, bodies, .. } => {
                list.extend(term);
                list.extend(bodies.iter_mut().flatten());
            }
            Kind::Citation {
                referent,
                prefix,
                suffix,
            } => {
                if let Referent::Footnote(Note::Inline(note)) = referent {
                    list.push(note);
                }
                list.extend(prefix);
                list.extend(suffix);
            }
            Kind::ThematicBreak
            | Kind::CodeBlock { .. }
            | Kind::HtmlBlock { .. }
            | Kind::FormulaBlock { .. }
            | Kind::Text { .. }
            | Kind::SoftBreak
            | Kind::LineBreak
            | Kind::Code { .. }
            | Kind::Html { .. }
            | Kind::CrossLink { .. }
            | Kind::CrossEmbed { .. }
            | Kind::Comment { .. }
            | Kind::Formula { .. }
            | Kind::Topic { .. }
            | Kind::Metadata { .. }
            | Kind::Reference { .. } => {}
        }
        list
    }
}

/// Universal attributes: classes, then records, both in source order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Attributes {
    pub classes: Vec<String>,
    pub records: Vec<Record>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Record {
    pub name: String,
    pub value: String,
}

/// Every node kind and its fields, in contract order.
#[derive(Clone, Debug, PartialEq)]
pub enum Kind {
    Document {
        metadata: Option<Box<Node>>,
        content: Vec<Node>,
    },
    Callout {
        variant: Option<String>,
        collapsed: Option<bool>,
        title: Option<Vec<Node>>,
        content: Vec<Node>,
    },
    Paragraph {
        content: Vec<Node>,
    },
    Heading {
        level: u8,
        content: Vec<Node>,
    },
    ThematicBreak,
    List {
        flavor: Flavor,
        start: Option<u64>,
        variant: Option<Variant>,
        delimiter: Option<Delimiter>,
        tight: bool,
        items: Vec<Node>,
    },
    ListItem {
        marker: Option<String>,
        content: Vec<Node>,
    },
    CodeBlock {
        info: Option<String>,
        language: Option<String>,
        literal: String,
        fenced: bool,
        closed: bool,
    },
    HtmlBlock {
        literal: String,
    },
    FormulaBlock {
        literal: String,
    },
    Table {
        columns: Vec<Flow>,
        head: Vec<Node>,
        content: Vec<Node>,
    },
    TableRow {
        cells: Vec<Node>,
    },
    TableCell {
        content: Vec<Node>,
    },
    DirectiveBlock {
        name: Option<String>,
        label: Option<Box<Node>>,
        content: Vec<Node>,
    },
    DirectiveLabel {
        content: Vec<Node>,
    },
    Text {
        literal: String,
    },
    SoftBreak,
    LineBreak,
    Code {
        literal: String,
    },
    Html {
        literal: String,
    },
    CrossLink {
        dest: Destination,
        label: Option<String>,
    },
    CrossEmbed {
        dest: Destination,
        label: Option<String>,
        dimensions: Option<Dimensions>,
    },
    Comment {
        literal: String,
    },
    Formula {
        mode: Placement,
        literal: String,
    },
    Emphasis {
        content: Vec<Node>,
    },
    Strong {
        content: Vec<Node>,
    },
    Strikethrough {
        content: Vec<Node>,
    },
    Mark {
        content: Vec<Node>,
    },
    Insertion {
        content: Vec<Node>,
    },
    Span {
        content: Vec<Node>,
    },
    Superscript {
        content: Vec<Node>,
    },
    Subscript {
        content: Vec<Node>,
    },
    Link {
        dest: Destination,
        title: Option<String>,
        content: Vec<Node>,
    },
    Embed {
        dest: Destination,
        title: Option<String>,
        dimensions: Option<Dimensions>,
        content: Vec<Node>,
    },
    Directive {
        name: String,
        label: Option<Box<Node>>,
    },
    Cite {
        citations: Vec<Node>,
    },
    Topic {
        name: String,
    },
    DefinitionList {
        definitions: Vec<Node>,
    },
    Definition {
        term: Vec<Node>,
        bodies: Vec<Vec<Node>>,
        compact: bool,
    },
    Citation {
        referent: Referent,
        prefix: Vec<Node>,
        suffix: Vec<Node>,
    },
    Footnote {
        label: Option<String>,
        content: Vec<Node>,
    },
    Specimen {
        label: Option<String>,
        start: Option<u64>,
        content: Vec<Node>,
    },
    Metadata {
        fields: Box<Fields>,
    },
    Reference {
        label: String,
        dest: Destination,
        title: Option<String>,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Flavor {
    Bullet,
    Ordered,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Variant {
    Decimal,
    Alpha { lowercased: bool },
    Roman { lowercased: bool },
    Default,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Delimiter {
    Period,
    Parenthesis { closed: bool },
    Default,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Flow {
    None,
    Left,
    Center,
    Right,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Placement {
    Embedded,
    Standalone,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Normal,
    AuthorInText,
    SuppressAuthor,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Destination {
    Url(String),
    Cross {
        path: String,
        anchor: Option<String>,
    },
    Reference(String),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Dimensions {
    pub width: u32,
    pub height: Option<u32>,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Referent {
    Bib { key: String, mode: Mode },
    Footnote(Note),
    Specimen { label: String },
}

/// The footnote a footnote referent names.
#[derive(Clone, Debug, PartialEq)]
pub enum Note {
    Label(String),
    /// An inline note owns its footnote.
    Inline(Box<Node>),
}

/// The ten metadata fields, in contract order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Fields {
    pub name: Option<Value>,
    pub title: Option<Value>,
    pub subtitle: Option<Value>,
    pub time: Option<Value>,
    pub date: Option<Value>,
    pub authors: Option<Value>,
    pub keywords: Option<Value>,
    pub abstract_: Option<Value>,
    pub state: Option<Value>,
    pub comment: Option<Value>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Value {
    Scalar(Scalar),
    List(Vec<Item>),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Scalar {
    Null,
    Bool(bool),
    Number(String),
    Text(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Item {
    Number(String),
    Text(String),
}
