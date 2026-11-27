//! Turn events into the canonical AST.
//!
//! The walk is recursive over the event tree: a handler takes the index of an
//! enter event and builds that token's node from the events up to its exit.
//! Positions come from the events alone: a node's runs are its token's span
//! less the prefixes of the containers around it.

use crate::ast::{
    Delimiter, Destination, Dimensions, Flavor, Kind, Mode, Node, Note, Placement, Record,
    Referent, Span, Status,
};
use crate::event::{Block, Event, Inline, Kind as EventKind, Name, Role, Space};
use crate::util::{
    character_reference::{decode as decode_character_reference, decode_at},
    classes::{anchor, normalize_label},
    infer::{list_loose, table_align},
    properties,
    slice::{Position, Slice},
};
use alloc::boxed::Box;
use alloc::{
    collections::BTreeSet,
    format,
    string::{String, ToString},
    vec,
    vec::Vec,
};
use core::str;

/// Build the document of `bytes` from its `events`.
pub fn compile(events: &[Event], bytes: &[u8]) -> Node {
    let mut compiler = Compiler::new(events, bytes);
    let content = compiler.block_range(0, events.len());
    let metadata = compiler.metadata().map(Box::new);
    let mut document = Node::new(
        Kind::Document { metadata, content },
        vec![Span::new(0, line_start_before_end(bytes))],
    );
    anchor_headings(&mut document);
    document
}

/// Where the document's span ends: before one final line ending.
fn line_start_before_end(bytes: &[u8]) -> usize {
    let mut end = bytes.len();
    if end > 0 && bytes[end - 1] == b'\n' {
        end -= 1;
        if end > 0 && bytes[end - 1] == b'\r' {
            end -= 1;
        }
    } else if end > 0 && bytes[end - 1] == b'\r' {
        end -= 1;
    }
    end
}

struct Compiler<'a> {
    events: &'a [Event],
    bytes: &'a [u8],
    /// The index of the exit of each enter event.
    exits: Vec<usize>,
    /// Container prefixes of continuation lines, with the whitespace before
    /// them: gaps between the runs of the leaves they interrupt.
    holes: Vec<Span>,
    /// Whether a table cell's content is being compiled, where `\|` in
    /// code is a pipe.
    cell: bool,
    /// The paragraph whose block identifier is the anchor of the list item around it.
    item_paragraph: Option<usize>,
    /// The anchor that paragraph gives the list item.
    item_anchor: Option<String>,
    /// How many superscripts and subscripts are around the content being
    /// compiled, where `\ ` is a non-breaking space.
    script: usize,
    /// The labels of the document's specimens, as written.
    specimens: Vec<&'a [u8]>,
}

/// Text read in an inline content list and not yet a node.
struct Draft {
    start: usize,
    end: usize,
    literal: String,
    /// How many bytes at the end of `literal` are spaces and tabs as written.
    tail: usize,
}

/// Whether `name` is a container prefix of a continuation line.
fn prefix(name: Name) -> bool {
    matches!(name, Name::BlockQuotePrefix | Name::ContainerIndent)
}

/// Whether `name` is whitespace between blocks rather than content.
fn between(name: Name) -> bool {
    matches!(
        name,
        Name::LineEnding | Name::BlankLineEnding | Name::SpaceOrTab
    ) || prefix(name)
}

impl<'a> Compiler<'a> {
    fn new(events: &'a [Event], bytes: &'a [u8]) -> Compiler<'a> {
        let mut exits = vec![0; events.len()];
        let mut stack = vec![];
        let mut holes = vec![];
        let mut specimens = vec![];
        for (index, event) in events.iter().enumerate() {
            match event.kind {
                EventKind::Enter => stack.push(index),
                EventKind::Exit => {
                    let enter = stack.pop().expect("expected an enter for each exit");
                    exits[enter] = index;
                    if event.name == Name::SpecimenLabel {
                        specimens.push(&bytes[events[enter].point.index..event.point.index]);
                    }
                    if prefix(event.name) {
                        holes.push(Span::new(events[enter].point.index, event.point.index));
                    }
                }
            }
        }
        for hole in &mut holes {
            while hole.start > 0 && matches!(bytes[hole.start - 1], b' ' | b'\t') {
                hole.start -= 1;
            }
        }
        holes.sort_by_key(|hole: &Span| hole.start);
        Compiler {
            events,
            bytes,
            exits,
            holes,
            cell: false,
            item_paragraph: None,
            item_anchor: None,
            script: 0,
            specimens,
        }
    }

    fn name(&self, index: usize) -> Name {
        self.events[index].name
    }

    fn start(&self, index: usize) -> usize {
        self.events[index].point.index
    }

    fn end(&self, index: usize) -> usize {
        self.events[self.exits[index]].point.index
    }

    fn slice(&self, start: usize, end: usize) -> &'a str {
        str::from_utf8(&self.bytes[start..end]).expect("expected events at char boundaries")
    }

    fn text(&self, index: usize) -> &'a str {
        self.slice(self.start(index), self.end(index))
    }

    /// The text of the token at `index`, the columns of a tab it starts or
    /// ends inside written as spaces.
    fn value(&self, index: usize) -> String {
        Slice::from_position(
            self.bytes,
            &Position::from_exit_event(self.events, self.exits[index]),
        )
        .serialize()
    }

    /// The runs of the leaf `start..end`: the span less the prefixes of
    /// the containers around it.
    fn runs(&self, start: usize, end: usize) -> Vec<Span> {
        let end = end.max(start);
        let mut runs = vec![];
        let mut from = start;
        for hole in &self.holes {
            if hole.end <= from {
                continue;
            }
            if hole.start < from {
                from = hole.end.min(end);
                continue;
            }
            if hole.start >= end {
                break;
            }
            if hole.start > from {
                runs.push(Span::new(from, hole.start));
            }
            from = hole.end.min(end);
        }
        runs.push(Span::new(from, end));
        // Runs that touch are one run.
        let mut merged: Vec<Span> = vec![];
        for run in runs {
            match merged.last_mut() {
                Some(last) if last.end == run.start => last.end = run.end,
                _ => merged.push(run),
            }
        }
        merged
    }

    /// The one run of the container `start..end`, which holds the prefixes
    /// of its lines.
    fn whole(start: usize, end: usize) -> Vec<Span> {
        vec![Span::new(start, end.max(start))]
    }

    /// The enter indices of the tokens directly in `from..to`.
    fn children(&self, from: usize, to: usize) -> Vec<usize> {
        let mut index = from;
        let mut list = vec![];
        while index < to {
            if self.events[index].kind == EventKind::Enter {
                list.push(index);
                index = self.exits[index] + 1;
            } else {
                index += 1;
            }
        }
        list
    }

    fn inside(&self, index: usize) -> Vec<usize> {
        self.children(index + 1, self.exits[index])
    }

    /// The first token directly in `index` named `name`.
    fn child(&self, index: usize, name: Name) -> Option<usize> {
        self.inside(index)
            .into_iter()
            .find(|child| self.name(*child) == name)
    }

    /// Where the token at `index` starts, past its leading whitespace.
    fn lead(&self, index: usize) -> usize {
        self.skip(self.start(index), self.end(index))
    }

    /// `at` past the spaces and tabs from it, up to `end`.
    fn skip(&self, mut at: usize, end: usize) -> usize {
        while at < end && matches!(self.bytes[at], b' ' | b'\t') {
            at += 1;
        }
        at
    }

    /// Where the content of the token at `index` ends, before its trailing
    /// line endings, blank lines and whitespace.
    fn last(&self, index: usize) -> (usize, usize) {
        let mut found = None;
        for child in self.inside(index) {
            if !between(self.name(child)) {
                found = Some(child);
            }
        }
        match found {
            Some(child) => (self.exits[child], self.end(child)),
            None => (index, self.start(index)),
        }
    }

    /// Where the blank lines after event `from` end, when any follow before
    /// the next content: at the start of the last one's line ending.
    fn blank_tail(&self, from: usize) -> Option<usize> {
        let mut index = from + 1;
        let mut found = None;
        while index < self.events.len() {
            let event = &self.events[index];
            if event.kind == EventKind::Exit {
                // A blank line ends a block quote or a definition body: it
                // is not inside it.
                if matches!(event.name, Name::BlockQuote | Name::DefinitionBody) {
                    break;
                }
                index += 1;
                continue;
            }
            match event.name {
                Name::BlankLineEnding => {
                    found = Some(event.point.index);
                    index = self.exits[index] + 1;
                }
                name if between(name) => index = self.exits[index] + 1,
                _ => break,
            }
        }
        found
    }

    // Blocks.

    fn block_range(&mut self, from: usize, to: usize) -> Vec<Node> {
        let mut nodes = vec![];
        for index in self.children(from, to) {
            if self.name(index) == Name::BlockIdentifierLine {
                if let Some(node) = self.anchor_line(index, nodes.last_mut()) {
                    nodes.push(node);
                }
                continue;
            }
            if let Some(mut node) = self.block(index) {
                // A start counts only on the first specimen of a group.
                if let (
                    Kind::Specimen { start, .. },
                    Some(Node {
                        kind: Kind::Specimen { .. },
                        ..
                    }),
                ) = (&mut node.kind, nodes.last())
                {
                    *start = None;
                }
                nodes.push(node);
            }
        }
        nodes
    }

    /// The metadata of the properties envelope that starts the document:
    /// its payload, from after its opening fence’s line to its closing
    /// fence’s line, decoded; its scope ends after the closing fence.
    fn metadata(&self) -> Option<Node> {
        let index = (0..self.events.len()).find(|index| {
            self.events[*index].kind == EventKind::Enter && self.name(*index) == Name::Frontmatter
        })?;
        let fences: Vec<usize> = self
            .inside(index)
            .into_iter()
            .filter(|child| self.name(*child) == Name::FrontmatterFence)
            .collect();
        let start = self
            .inside(index)
            .into_iter()
            .find(|child| self.name(*child) == Name::LineEnding)
            .map(|ending| self.end(ending))?;
        let close = self.start(*fences.last()?);
        let fields = properties::decode(self.bytes, start, close);
        Some(Node::new(
            Kind::Metadata {
                fields: Box::new(fields),
            },
            vec![Span::new(self.start(index), self.end(index))],
        ))
    }

    /// The blocks inside the container at `index`.
    fn block_content(&mut self, index: usize) -> Vec<Node> {
        self.block_range(index + 1, self.exits[index])
    }

    fn block(&mut self, index: usize) -> Option<Node> {
        let block = match self.name(index).role() {
            Role::Block(block) => block,
            Role::Inline(_) | Role::Space(_) | Role::Part => return None,
        };
        let node = match block {
            Block::Paragraph => self.paragraph(index),
            Block::HeadingAtx => {
                let level = self
                    .child(index, Name::HeadingAtxSequence)
                    .map_or(1, |sequence| self.text(sequence).len());
                let content = match self.child(index, Name::HeadingAtxText) {
                    Some(text) => self.content(text),
                    None => vec![],
                };
                Node::new(
                    Kind::Heading {
                        level: u8::try_from(level).unwrap_or(6),
                        content,
                    },
                    self.runs(self.lead(index), self.end(index)),
                )
            }
            Block::HeadingSetext => {
                let level = match self.child(index, Name::HeadingSetextUnderline) {
                    Some(underline) if self.text(underline).contains('-') => 2,
                    _ => 1,
                };
                let content = match self.child(index, Name::HeadingSetextText) {
                    Some(text) => self.content(text),
                    None => vec![],
                };
                Node::new(
                    Kind::Heading { level, content },
                    self.runs(self.lead(index), self.end(index)),
                )
            }
            Block::ThematicBreak => {
                let end = self
                    .blank_tail(self.exits[index])
                    .unwrap_or_else(|| self.end(index));
                Node::new(Kind::ThematicBreak, self.runs(self.lead(index), end))
            }
            Block::CodeIndented => self.code_block_indented(index),
            Block::CodeFenced => self.code_block_fenced(index),
            Block::Comment => Node::new(
                Kind::Comment {
                    literal: self.fenced_literal(index, Name::CommentFence, Name::CommentChunk),
                },
                self.runs(self.lead(index), self.last(index).1),
            ),
            Block::HtmlFlow => {
                let literal = self.literal_lines(index, Name::HtmlFlowData);
                let kind = match comment_block_literal(&literal) {
                    Some(inner) => Kind::Comment {
                        literal: inner.to_string(),
                    },
                    None => Kind::HtmlBlock { literal },
                };
                Node::new(kind, self.runs(self.lead(index), self.last(index).1))
            }
            Block::Quote => {
                let runs = Self::whole(self.lead(index), self.end(index));
                let content = self.block_content(index);
                let (variant, collapsed, title) = match self.child(index, Name::Callout) {
                    Some(header) => self.callout(header),
                    None => (None, None, None),
                };
                Node::new(
                    Kind::Callout {
                        variant,
                        collapsed,
                        title,
                        content,
                    },
                    runs,
                )
            }
            Block::List => self.list(index),
            Block::DefinitionList => self.definition_list(index),
            Block::Definition => self.reference(index),
            Block::Table => self.table(index),
            Block::DirectiveLeaf => {
                let (name, label) = self.directive_head(index);
                Node::new(
                    Kind::DirectiveBlock {
                        name,
                        label,
                        content: vec![],
                    },
                    self.runs(self.lead(index), self.end(index)),
                )
            }
            Block::DirectiveContainer => {
                let fence = self.child(index, Name::DirectiveContainerFence);
                let (name, label) = fence.map_or((None, None), |fence| self.directive_head(fence));
                let runs = Self::whole(self.lead(index), self.last(index).1);
                let content = self.block_content(index);
                let mut node = Node::new(
                    Kind::DirectiveBlock {
                        name,
                        label,
                        content,
                    },
                    runs,
                );
                if let Some(fence) = fence {
                    if let Some(attributes) = self.child(fence, Name::Attributes) {
                        self.attributes(attributes, &mut node);
                    }
                    // A nameless directive’s class word is one class.
                    if let Some(class) = self.child(fence, Name::DirectiveClass) {
                        node.attributes.classes.push(self.text(class).to_string());
                    }
                }
                node
            }
            Block::Footnote => {
                let label = self
                    .child(index, Name::FootnoteDefinitionPrefix)
                    .and_then(|prefix| self.child(prefix, Name::FootnoteDefinitionLabel))
                    .and_then(|label| self.child(label, Name::FootnoteDefinitionLabelText))
                    .map(|string| normalize_label(self.text(string)));
                let (last, content_end) = self.last(index);
                let end = self.blank_tail(last).unwrap_or(content_end);
                let runs = Self::whole(self.lead(index), end);
                let content = self.block_content(index);
                Node::new(Kind::Footnote { label, content }, runs)
            }
            Block::Specimen => {
                let prefix = self.child(index, Name::SpecimenPrefix);
                let label = prefix
                    .and_then(|prefix| self.child(prefix, Name::SpecimenLabel))
                    .map(|label| self.text(label).to_string());
                let start = prefix
                    .and_then(|prefix| self.child(prefix, Name::SpecimenStart))
                    .and_then(|start| self.text(start).parse::<u64>().ok());
                let (last, content_end) = self.last(index);
                let end = self.blank_tail(last).unwrap_or(content_end);
                let runs = Self::whole(self.lead(index), end);
                let content = self.block_content(index);
                Node::new(
                    Kind::Specimen {
                        label,
                        start,
                        content,
                    },
                    runs,
                )
            }
            Block::Formula => {
                let literal = self.fenced_literal(index, Name::FormulaFence, Name::FormulaChunk);
                Node::new(
                    Kind::FormulaBlock {
                        literal: literal.trim_matches(WHITESPACE).to_string(),
                    },
                    self.runs(self.lead(index), self.end(index)),
                )
            }
            // Frontmatter compiles with the dialect’s properties, which are
            // not ported yet; an identifier line with the block before it,
            // by `block_range`.
            Block::Frontmatter | Block::Identifier => return None,
        };
        let mut node = node;
        if let Some(attributes) = self.child(index, Name::Attributes) {
            self.attributes(attributes, &mut node);
        }
        Some(node)
    }

    /// The identifier line at `index`: it names `owner`, a list, callout
    /// or table without a name and after a blank line, and its source then
    /// ends with the line; otherwise it is a paragraph of its text.
    fn anchor_line(&mut self, index: usize, owner: Option<&mut Node>) -> Option<Node> {
        let identifier = self.child(index, Name::BlockIdentifier)?;
        let text = self.text(identifier);
        let (start, end) = (self.start(identifier), self.end(identifier));
        let runs = self.runs(start, end);
        // The line the item starts on names the item: it is an empty
        // paragraph.
        if self.item_paragraph == Some(index) {
            self.item_paragraph = None;
            self.item_anchor = Some(text[1..text.len() - 1].to_string());
            return Some(Node::new(Kind::Paragraph { content: vec![] }, runs));
        }
        if let Some(owner) = owner {
            let trailed = matches!(
                owner.kind,
                Kind::List { .. } | Kind::Callout { .. } | Kind::Table { .. }
            );
            if trailed && owner.anchor.is_none() && self.blank_before(start) {
                owner.anchor = Some(text[1..text.len() - 1].to_string());
                // The last run goes on to the end of the identifier: less
                // the prefixes of the containers on the way for a table,
                // which is a leaf.
                if let Some(last) = owner.runs.pop() {
                    if matches!(owner.kind, Kind::Table { .. }) {
                        owner.runs.extend(self.runs(last.start, end));
                    } else {
                        owner.runs.extend(Self::whole(last.start, end));
                    }
                }
                return None;
            }
        }
        Some(Node::new(
            Kind::Paragraph {
                content: vec![Node::new(
                    Kind::Text {
                        literal: text.to_string(),
                    },
                    runs.clone(),
                )],
            },
            runs,
        ))
    }

    /// Whether the line before the one of `at` is blank, its container
    /// markers aside.
    fn blank_before(&self, at: usize) -> bool {
        let mut index = at;
        while index > 0 && self.bytes[index - 1] != b'\n' {
            index -= 1;
        }
        if index == 0 {
            return false;
        }
        index -= 1;
        while index > 0 && self.bytes[index - 1] != b'\n' {
            index -= 1;
            if !matches!(self.bytes[index], b' ' | b'\t' | b'>' | b'\r') {
                return false;
            }
        }
        true
    }

    fn paragraph(&mut self, index: usize) -> Node {
        let start = self.start(index);
        let from = index + 1;
        let mut content = self.content_range(from, self.exits[index], true);
        let mut anchor = None;
        if let Some(identifier) = self.child(index, Name::BlockIdentifier) {
            let text = self.text(identifier);
            let id = text[1..text.len() - 1].to_string();
            if self.item_paragraph == Some(index) {
                self.item_paragraph = None;
                self.item_anchor = Some(id);
            } else if self.children(from, self.exits[index]).iter().all(|child| {
                matches!(
                    self.name(*child),
                    Name::BlockIdentifier | Name::BlockIdentifierTail
                )
            }) {
                // An identifier alone names no paragraph: it is text.
                content.push(Node::new(
                    Kind::Text {
                        literal: text.to_string(),
                    },
                    self.runs(self.start(identifier), self.end(identifier)),
                ));
            } else {
                anchor = Some(id);
            }
        }
        let mut node = Node::new(
            Kind::Paragraph { content },
            self.runs(self.skip(start, self.end(index)), self.end(index)),
        );
        node.anchor = anchor;
        node
    }

    /// The variant, fold, and title of the callout header at `index`.
    fn callout(&mut self, index: usize) -> (Option<String>, Option<bool>, Option<Vec<Node>>) {
        let variant = self
            .child(index, Name::CalloutVariant)
            .map(|part| self.text(part).to_string());
        let collapsed = self
            .child(index, Name::CalloutFold)
            .map(|part| self.text(part) == "-");
        let title = self
            .child(index, Name::CalloutTitle)
            .map(|part| self.content(part))
            .filter(|nodes| !nodes.is_empty());
        (variant, collapsed, title)
    }

    fn literal_lines(&self, index: usize, data: Name) -> String {
        let mut literal = String::new();
        let mut line = String::new();
        let mut open = false;
        for child in self.inside(index) {
            let name = self.name(child);
            if name == data {
                line.push_str(&self.value(child));
                open = true;
            } else if name == Name::LineEnding {
                literal.push_str(&line);
                literal.push('\n');
                line.clear();
                open = false;
            }
        }
        if open {
            literal.push_str(&line);
            literal.push('\n');
        }
        literal
    }

    fn code_block_indented(&self, index: usize) -> Node {
        let mut literal = self.literal_lines(index, Name::CodeFlowChunk);
        // Blank lines at the end are not the code's.
        while literal.ends_with('\n') {
            let body = &literal[..literal.len() - 1];
            let line = body.rfind('\n').map_or(0, |at| at + 1);
            if body[line..]
                .bytes()
                .all(|byte| matches!(byte, b' ' | b'\t'))
                && !body.is_empty()
            {
                literal.truncate(line);
            } else {
                break;
            }
        }
        let end = self
            .blank_tail(self.exits[index])
            .unwrap_or_else(|| self.end(index));
        Node::new(
            Kind::CodeBlock {
                info: None,
                language: None,
                literal,
                fenced: false,
                closed: true,
            },
            // The code starts in the byte holding its first column past
            // the indent.
            self.runs(
                self.inside(index)
                    .first()
                    .filter(|first| self.name(**first) == Name::SpaceOrTab)
                    .map_or(self.start(index), |first| self.end(*first)),
                end,
            ),
        )
    }

    fn code_block_fenced(&self, index: usize) -> Node {
        let mut info = None;
        if let Some(open) = self.child(index, Name::CodeFencedFence) {
            let parts: Vec<usize> = self
                .inside(open)
                .into_iter()
                .filter(|child| {
                    matches!(
                        self.name(*child),
                        Name::CodeFencedFenceInfo | Name::CodeFencedFenceMeta
                    )
                })
                .collect();
            if let (Some(first), Some(last)) = (parts.first(), parts.last()) {
                // Attributes after the info take the spaces before them.
                info = Some(
                    self.string(*first, self.exits[*last] + 1)
                        .trim_end_matches([' ', '\t'])
                        .to_string(),
                );
            }
        }
        let language = info.as_ref().map(|info| {
            info.trim_start_matches([' ', '\t'])
                .split([' ', '\t'])
                .next()
                .unwrap_or("")
                .to_string()
        });
        let closed = self
            .inside(index)
            .into_iter()
            .filter(|child| self.name(*child) == Name::CodeFencedFence)
            .count()
            > 1;
        let literal = self.fenced_literal(index, Name::CodeFencedFence, Name::CodeFlowChunk);
        let runs = self.runs(self.lead(index), self.last(index).1);
        // A fence whose info string is `formula` is a formula block.
        if info.as_deref().map(|info| info.trim_matches(WHITESPACE)) == Some("formula") {
            return Node::new(
                Kind::FormulaBlock {
                    literal: literal.trim_matches(WHITESPACE).to_string(),
                },
                runs,
            );
        }
        let mut node = Node::new(
            Kind::CodeBlock {
                info,
                language,
                literal,
                fenced: true,
                closed,
            },
            runs,
        );
        // Attributes end the opening fence’s line.
        if let Some(attributes) = self
            .child(index, Name::CodeFencedFence)
            .and_then(|open| self.child(open, Name::Attributes))
        {
            self.attributes(attributes, &mut node);
        }
        node
    }

    /// The literal of a fenced block: each line between its fences, with a
    /// line feed.
    fn fenced_literal(&self, index: usize, fence: Name, chunk: Name) -> String {
        let mut literal = String::new();
        let mut fences = 0;
        let mut line: Option<String> = None;
        for child in self.inside(index) {
            let name = self.name(child);
            if name == fence {
                fences += 1;
                if fences > 1 {
                    break;
                }
            } else if name == Name::LineEnding {
                if let Some(text) = line.take() {
                    literal.push_str(&text);
                    literal.push('\n');
                }
                line = Some(String::new());
            } else if name == chunk {
                line.get_or_insert_with(String::new)
                    .push_str(&self.value(child));
            }
        }
        if let Some(text) = line {
            if !text.is_empty() {
                literal.push_str(&text);
                literal.push('\n');
            }
        }
        literal
    }

    fn list(&mut self, index: usize) -> Node {
        let loose = list_loose(self.events, index, true);
        let mut items = vec![];
        let mut first = None;
        for child in self.inside(index) {
            if self.name(child) != Name::ListItem {
                continue;
            }
            if items.is_empty() {
                first = Some(child);
            }
            items.push(self.list_item(child));
        }
        let start = items.first().map_or(self.start(index), Node::start);
        let end = items.last().map_or(self.end(index), Node::end).max(
            self.blank_tail(self.exits[index])
                .unwrap_or_else(|| self.end(index)),
        );
        let prefix = first.and_then(|child| self.child(child, Name::ListItemPrefix));
        let (flavor, start_value, delimiter) =
            match prefix.and_then(|prefix| self.child(prefix, Name::ListItemValue)) {
                Some(value) => {
                    let marker = prefix.and_then(|prefix| self.child(prefix, Name::ListItemMarker));
                    (
                        Flavor::Ordered,
                        Some(self.text(value).parse::<u64>().unwrap_or(1)),
                        Some(match marker.map(|marker| self.bytes[self.start(marker)]) {
                            Some(b')') => Delimiter::Parenthesis,
                            _ => Delimiter::Period,
                        }),
                    )
                }
                None => (Flavor::Bullet, None, None),
            };
        Node::new(
            Kind::List {
                flavor,
                start: start_value,
                delimiter,
                tight: !loose,
                items,
            },
            Self::whole(start, end),
        )
    }

    fn definition_list(&mut self, index: usize) -> Node {
        let mut definitions = vec![];
        for child in self.inside(index) {
            if self.name(child) == Name::DefinitionItem {
                definitions.push(self.definition(child));
            }
        }
        let start = definitions.first().map_or(self.start(index), Node::start);
        let end = definitions.last().map_or(self.end(index), Node::end);
        Node::new(
            Kind::DefinitionList { definitions },
            Self::whole(start, end),
        )
    }

    /// A term with its bodies: compact when no blank line comes between the
    /// term and its first body.
    fn definition(&mut self, index: usize) -> Node {
        let mut term = vec![];
        let mut bodies = vec![];
        let mut compact = true;
        let mut start = self.start(index);
        let mut end = start;
        for child in self.inside(index) {
            match self.name(child) {
                Name::DefinitionTerm => {
                    start = self.lead(child);
                    end = self.end(child);
                    term = self.content(child);
                }
                Name::BlankLineEnding if bodies.is_empty() => compact = false,
                Name::DefinitionBody => {
                    let (_, last) = self.last(child);
                    end = end.max(last);
                    bodies.push(self.block_content(child));
                }
                _ => {}
            }
        }
        Node::new(
            Kind::Definition {
                term,
                bodies,
                compact,
            },
            Self::whole(start, end),
        )
    }

    fn list_item(&mut self, index: usize) -> Node {
        let (last, content_end) = self.last(index);
        // An item with no content is closed by the blank line after it.
        let end = if self.name(last) == Name::ListItemPrefix {
            line_stop(self.bytes, content_end)
        } else {
            self.blank_tail(last).unwrap_or(content_end)
        };
        let runs = Self::whole(self.lead(index), end);
        let status = self.status(index);
        // The block identifier on the first line of its first paragraph names
        // the item.
        let first = self
            .inside(index)
            .into_iter()
            .find(|child| !between(self.name(*child)) && self.name(*child) != Name::ListItemPrefix);
        if let Some(first) = first {
            let line = self.events[index].point.line;
            let named = matches!(
                self.name(first),
                Name::Paragraph | Name::BlockIdentifierLine
            ) && self
                .child(first, Name::BlockIdentifier)
                .map_or(false, |identifier| {
                    self.events[identifier].point.line == line
                });
            if named {
                self.item_paragraph = Some(first);
            }
        }
        let content = self.block_content(index);
        let mut node = Node::new(Kind::ListItem { status, content }, runs);
        node.anchor = self.item_anchor.take();
        node
    }

    /// The task status of the list item at `index`: the value of the check
    /// that ends its prefix.
    fn status(&self, index: usize) -> Option<Status> {
        let prefix = self.child(index, Name::ListItemPrefix)?;
        let check = self.child(prefix, Name::TaskListItemCheck)?;
        let value = self.child(check, Name::TaskListItemValue)?;
        match self.bytes[self.start(value)] {
            b' ' => Some(Status::Open),
            b'x' | b'X' => Some(Status::Done),
            b'?' => Some(Status::Question),
            _ => Some(Status::Cancelled),
        }
    }

    fn reference(&self, index: usize) -> Node {
        let label = self
            .child(index, Name::DefinitionLabel)
            .and_then(|label| self.child(label, Name::DefinitionLabelText))
            .map(|string| normalize_label(self.text(string)))
            .unwrap_or_default();
        let dest = self
            .child(index, Name::DefinitionDestination)
            .map(|dest| self.dest(dest))
            .unwrap_or_default();
        let title = self.child(index, Name::DefinitionTitle).map(|title| {
            self.child(title, Name::DefinitionTitleText)
                .map(|string| self.string(string, self.exits[string] + 1))
                .unwrap_or_default()
        });
        Node::new(
            Kind::Reference {
                label,
                dest: Destination::Url(dest),
                title,
            },
            self.runs(self.lead(index), self.end(index)),
        )
    }

    /// The decoded string of a definition or resource destination.
    fn dest(&self, index: usize) -> String {
        if matches!(
            self.name(index),
            Name::DefinitionDestinationText | Name::ResourceDestinationText
        ) {
            return self.string(index, self.exits[index] + 1);
        }
        for child in self.inside(index) {
            if matches!(
                self.name(child),
                Name::DefinitionDestinationLiteral
                    | Name::DefinitionDestinationRaw
                    | Name::DefinitionDestinationText
                    | Name::ResourceDestinationLiteral
                    | Name::ResourceDestinationRaw
                    | Name::ResourceDestinationText
            ) {
                return self.dest(child);
            }
        }
        String::new()
    }

    fn table(&mut self, index: usize) -> Node {
        let columns = table_align(self.events, index);
        let mut head = vec![];
        let mut content = vec![];
        for child in self.inside(index) {
            let into = match self.name(child) {
                Name::TableHead => &mut head,
                Name::TableBody => &mut content,
                _ => continue,
            };
            for row in self.inside(child) {
                if self.name(row) == Name::TableRow {
                    into.push(self.table_row(row, columns.len()));
                }
            }
        }
        Node::new(
            Kind::Table {
                columns,
                head,
                content,
            },
            Self::whole(self.lead(index), self.end(index)),
        )
    }

    fn table_row(&mut self, index: usize, columns: usize) -> Node {
        let mut cells = vec![];
        for child in self.inside(index) {
            if self.name(child) != Name::TableCell {
                continue;
            }
            self.cell = true;
            let content = match self.child(child, Name::TableCellText) {
                Some(text) => self.content(text),
                None => vec![],
            };
            self.cell = false;
            // A cell lies between its dividers; one alone closes it after
            // its content, and opens it otherwise. An empty cell takes the
            // divider that closes it.
            let parts = self.inside(child);
            let dividers: Vec<usize> = parts
                .iter()
                .copied()
                .filter(|part| self.name(*part) == Name::TableCellDivider)
                .collect();
            let mut start = self.start(child);
            let mut end = self.end(child);
            match dividers[..] {
                [open, close, ..] => {
                    start = self.end(open);
                    end = self.start(close);
                }
                [one]
                    if parts
                        .iter()
                        .take_while(|part| **part != one)
                        .any(|part| self.name(*part) != Name::SpaceOrTab) =>
                {
                    end = self.start(one);
                }
                [one] => start = self.end(one),
                [] => {}
            }
            if start == end && end < self.end(index) {
                end += 1;
            }
            cells.push(Node::new(
                Kind::TableCell { content },
                self.runs(start, end),
            ));
        }
        // A missing cell takes the last byte of its row.
        let end = self.end(index);
        let start = end.saturating_sub(1).max(self.start(index));
        while cells.len() < columns {
            cells.push(Node::new(
                Kind::TableCell { content: vec![] },
                vec![Span::new(start, end)],
            ));
        }
        cells.truncate(columns);
        Node::new(Kind::TableRow { cells }, self.runs(self.lead(index), end))
    }

    // Strings.

    /// The decoded value of the string content in events `from..to`.
    fn string(&self, from: usize, to: usize) -> String {
        let mut value = String::new();
        let mut index = from;
        while index < to {
            if self.events[index].kind == EventKind::Enter {
                match self.name(index) {
                    Name::Data | Name::SpaceOrTab | Name::LineEnding => {
                        value.push_str(self.text(index));
                        index = self.exits[index];
                    }
                    Name::CharacterEscape => {
                        value.push_str(&self.text(index)[1..]);
                        index = self.exits[index];
                    }
                    Name::CharacterReference => {
                        value.push_str(&self.character_reference(index));
                        index = self.exits[index];
                    }
                    _ => {}
                }
            }
            index += 1;
        }
        value
    }

    /// The decoded value of a character reference.
    fn character_reference(&self, index: usize) -> String {
        let mut marker = b'&';
        let mut value = "";
        for child in self.inside(index) {
            match self.name(child) {
                Name::CharacterReferenceMarkerNumeric => marker = b'#',
                Name::CharacterReferenceMarkerHexadecimal => marker = b'x',
                Name::CharacterReferenceValue => value = self.text(child),
                _ => {}
            }
        }
        decode_character_reference(value, marker).unwrap_or_else(|| self.text(index).into())
    }

    // Inlines.

    /// The inline content of the block content token at `index`.
    fn content(&mut self, index: usize) -> Vec<Node> {
        self.content_range(index + 1, self.exits[index], true)
    }

    /// The inline content of the inline token at `index`: a label or a
    /// part, which keeps the whitespace at its end.
    fn label_content(&mut self, index: usize) -> Vec<Node> {
        self.content_range(index + 1, self.exits[index], false)
    }

    fn content_range(&mut self, from: usize, to: usize, trim: bool) -> Vec<Node> {
        let mut nodes = vec![];
        let mut draft: Option<Draft> = None;
        let mut hard = false;
        let mut line_start = true;
        let mut skip_ending = false;
        // Where the data event just before ends, if one does.
        let mut data_end = None;
        for index in self.children(from, to) {
            let name = self.name(index);
            let at_line_start = line_start;
            // The prefixes of the containers around a line go before it.
            line_start =
                at_line_start && matches!(name, Name::ContainerIndent | Name::BlockQuotePrefix);
            let after_data = data_end.take();
            match name.role() {
                Role::Space(Space::Whitespace) if at_line_start => {}
                Role::Inline(Inline::Data) | Role::Space(Space::Whitespace) => {
                    let mut literal = self.value(index);
                    if self.script > 0 {
                        self.script_spaces(index, after_data, &mut literal, &mut draft);
                    }
                    if name == Name::Data {
                        data_end = Some(self.end(index));
                    }
                    push_text(
                        &mut draft,
                        self.start(index),
                        self.end(index),
                        &literal,
                        true,
                    );
                }
                Role::Inline(Inline::CharacterEscape) => {
                    // In a cell, `\|` is a pipe before escapes are read, so
                    // an escaped backslash before a pipe is that pipe's
                    // escape.
                    let pipe = self.cell
                        && self.text(index) == "\\\\"
                        && self.bytes.get(self.end(index)) == Some(&b'|');
                    let literal = if pipe {
                        String::new()
                    } else {
                        self.text(index)[1..].to_string()
                    };
                    push_text(
                        &mut draft,
                        self.start(index),
                        self.end(index),
                        &literal,
                        false,
                    );
                }
                Role::Inline(Inline::CharacterReference) => {
                    let literal = self.character_reference(index);
                    push_text(
                        &mut draft,
                        self.start(index),
                        self.end(index),
                        &literal,
                        false,
                    );
                }
                Role::Inline(Inline::HardBreakTail) => {
                    push_text(&mut draft, self.start(index), self.end(index), "", true);
                    hard = true;
                }
                Role::Space(Space::LineEnding) => {
                    line_start = true;
                    if skip_ending {
                        skip_ending = false;
                        continue;
                    }
                    if let Some(text) = &mut draft {
                        let trimmed = text.literal.len() - text.tail;
                        text.literal.truncate(trimmed);
                    }
                    self.flush(&mut nodes, &mut draft);
                    let kind = if hard {
                        Kind::LineBreak
                    } else {
                        Kind::SoftBreak
                    };
                    hard = false;
                    nodes.push(Node::new(
                        kind,
                        self.runs(self.start(index), self.end(index)),
                    ));
                }
                Role::Inline(Inline::HardBreakEscape) => {
                    self.flush(&mut nodes, &mut draft);
                    // The break covers the backslash and the line ending.
                    let end = line_end(self.bytes, self.end(index));
                    nodes.push(Node::new(
                        Kind::LineBreak,
                        self.runs(self.start(index), end),
                    ));
                    skip_ending = true;
                }
                Role::Inline(_)
                | Role::Space(Space::BlankLineEnding)
                | Role::Part
                | Role::Block(_) => {
                    self.flush(&mut nodes, &mut draft);
                    if let Some(node) = self.inline(index) {
                        nodes.push(node);
                    }
                }
            }
        }
        // Whitespace at the end of block content is not content.
        if trim {
            if let Some(text) = &mut draft {
                let cut = text.tail;
                text.literal.truncate(text.literal.len() - cut);
                text.end -= cut.min(text.end - text.start);
            }
        }
        self.flush(&mut nodes, &mut draft);
        nodes
    }

    fn flush(&self, nodes: &mut Vec<Node>, draft: &mut Option<Draft>) {
        if let Some(text) = draft.take() {
            if !text.literal.is_empty() {
                nodes.push(Node::new(
                    Kind::Text {
                        literal: text.literal,
                    },
                    self.runs(text.start, text.end),
                ));
            }
        }
    }

    fn inline(&mut self, index: usize) -> Option<Node> {
        let name = self.name(index);
        let runs = self.runs(self.start(index), self.end(index));
        let inline = match name.role() {
            Role::Inline(inline) => inline,
            Role::Block(_) | Role::Space(_) | Role::Part => return None,
        };
        let kind = match inline {
            Inline::Emphasis => Kind::Emphasis {
                content: self.part_content(index, Name::EmphasisText),
            },
            Inline::Strong => Kind::Strong {
                content: self.part_content(index, Name::StrongText),
            },
            Inline::Strikethrough => Kind::Strikethrough {
                content: self.part_content(index, Name::StrikethroughText),
            },
            Inline::Insertion => Kind::Insertion {
                content: self.part_content(index, Name::InsertionText),
            },
            Inline::Mark => Kind::Mark {
                content: self.part_content(index, Name::MarkText),
            },
            Inline::Subscript => Kind::Subscript {
                content: self.script_content(index, Name::SubscriptText),
            },
            Inline::Superscript => Kind::Superscript {
                content: self.script_content(index, Name::SuperscriptText),
            },
            Inline::CodeText => Kind::Code {
                literal: self.code(index, Name::CodeTextData),
            },
            Inline::Formula => self.formula(index),
            Inline::HtmlText => {
                let literal = self.merged_literal(index, Name::HtmlTextData);
                match comment_literal(&literal) {
                    Some(inner) => Kind::Comment {
                        literal: inner.to_string(),
                    },
                    None => Kind::Html { literal },
                }
            }
            Inline::Comment => {
                let literal = self.merged_literal(index, Name::CommentText);
                Kind::Comment {
                    // In a table cell, an escaped pipe is the cell's.
                    literal: if self.cell {
                        literal.replace("\\|", "|")
                    } else {
                        literal
                    },
                }
            }
            Inline::Autolink => {
                let mut dest = String::new();
                let mut text = None;
                for child in self.inside(index) {
                    match self.name(child) {
                        Name::AutolinkProtocol => {
                            dest = self.text(child).to_string();
                            text = Some(child);
                        }
                        Name::AutolinkEmail => {
                            dest = format!("mailto:{}", self.text(child));
                            text = Some(child);
                        }
                        _ => {}
                    }
                }
                let content = text
                    .map(|child| {
                        Node::new(
                            Kind::Text {
                                literal: self.text(child).to_string(),
                            },
                            self.runs(self.start(child), self.end(child)),
                        )
                    })
                    .into_iter()
                    .collect();
                Kind::Link {
                    dest: Destination::Url(dest),
                    title: None,
                    content,
                }
            }
            Inline::Literal => {
                let literal = self.text(index).to_string();
                let dest = match name {
                    Name::AutolinkLiteralWww => format!("http://{}", literal),
                    Name::AutolinkLiteralEmail => format!("mailto:{}", literal),
                    _ => literal.clone(),
                };
                Kind::Link {
                    dest: Destination::Url(dest),
                    title: None,
                    content: vec![Node::new(Kind::Text { literal }, runs.clone())],
                }
            }
            Inline::Media => self.link(index),
            Inline::CrossLink => self.cross_link(index),
            Inline::Directive => self.directive(index),
            Inline::Span => Kind::Span {
                content: match self
                    .child(index, Name::Label)
                    .and_then(|label| self.child(label, Name::LabelText))
                {
                    Some(text) => self.label_content(text),
                    None => vec![],
                },
            },
            Inline::CitationKey => {
                let mut citation = self.citation(index, Mode::AuthorInText);
                // A bare key alone names the specimen it is the label of.
                if let Kind::Citation {
                    referent: referent @ Referent::Bib { .. },
                    ..
                } = &mut citation.kind
                {
                    if let Referent::Bib {
                        key,
                        mode: Mode::AuthorInText,
                    } = referent
                    {
                        let braced = self.child(index, Name::CitationBrace).is_some();
                        if !braced && self.specimens.contains(&key.as_bytes()) {
                            *referent = Referent::Specimen {
                                label: core::mem::take(key),
                            };
                        }
                    }
                }
                Kind::Cite {
                    citations: vec![citation],
                }
            }
            Inline::SpecimenReference => {
                let key = self.child(index, Name::CitationKey);
                let label = key
                    .and_then(|key| self.child(key, Name::CitationKeyText))
                    .map(|text| self.text(text).to_string())
                    .unwrap_or_default();
                let runs =
                    key.map_or_else(Vec::new, |key| self.runs(self.start(key), self.end(key)));
                Kind::Cite {
                    citations: vec![Node::new(
                        Kind::Citation {
                            referent: Referent::Specimen { label },
                            prefix: vec![],
                            suffix: vec![],
                        },
                        runs,
                    )],
                }
            }
            Inline::Cite => {
                // A key with a tail: its citation starts the cite, at the key.
                let mut mode = Mode::AuthorInText;
                let mut citations = vec![];
                for child in self.inside(index) {
                    match self.name(child) {
                        Name::Citation => {
                            citations.push(self.citation(child, mode));
                            mode = Mode::Normal;
                        }
                        _ => mode = Mode::Normal,
                    }
                }
                Kind::Cite { citations }
            }
            Inline::Topic => Kind::Topic {
                name: self
                    .child(index, Name::TopicText)
                    .map(|name| self.text(name).to_string())
                    .unwrap_or_default(),
            },
            Inline::FootnoteCall => {
                let label = self
                    .child(index, Name::Label)
                    .and_then(|label| self.child(label, Name::LabelText))
                    .map(|text| normalize_label(self.text(text)))
                    .unwrap_or_default();
                let citation = Node::new(
                    Kind::Citation {
                        referent: Referent::Footnote(Note::Label(label)),
                        prefix: vec![],
                        suffix: vec![],
                    },
                    self.runs(self.start(index) + 1, self.end(index) - 1),
                );
                Kind::Cite {
                    citations: vec![citation],
                }
            }
            Inline::Note => {
                // A note is content of its own, in no script.
                let script = core::mem::replace(&mut self.script, 0);
                let text = self
                    .child(index, Name::Label)
                    .and_then(|label| self.child(label, Name::LabelText));
                let (content, inner) = match text {
                    Some(text) => (
                        self.label_content(text),
                        self.runs(self.start(text), self.end(text)),
                    ),
                    None => (vec![], runs.clone()),
                };
                self.script = script;
                let note = Node::new(
                    Kind::Footnote {
                        label: None,
                        content,
                    },
                    runs.clone(),
                );
                let citation = Node::new(
                    Kind::Citation {
                        referent: Referent::Footnote(Note::Inline(Box::new(note))),
                        prefix: vec![],
                        suffix: vec![],
                    },
                    inner,
                );
                Kind::Cite {
                    citations: vec![citation],
                }
            }
            // Text, compiled by `content_range`.
            Inline::Data
            | Inline::CharacterEscape
            | Inline::CharacterReference
            | Inline::HardBreakTail
            | Inline::HardBreakEscape => return None,
        };
        let mut node = Node::new(kind, runs);
        if let Some(attributes) = self.child(index, Name::Attributes) {
            self.attributes(attributes, &mut node);
        }
        Some(node)
    }

    /// Give `node` the anchor, classes, and records of the attributes at
    /// `index`, in source order; a later anchor replaces an earlier one, and
    /// an empty one is none.
    fn attributes(&self, index: usize, node: &mut Node) {
        for member in self.inside(index) {
            match self.name(member) {
                Name::AttributeId => node.anchor = Some(self.text(member)[1..].to_string()),
                Name::AttributeClass => {
                    node.attributes
                        .classes
                        .push(self.text(member)[1..].to_string());
                }
                Name::AttributeRecord => {
                    let name = self
                        .child(member, Name::AttributeNameText)
                        .map_or("", |name| self.text(name));
                    // A bare name is `true`; an empty value has no token.
                    let value = match self.child(member, Name::AttributeValue) {
                        Some(value) => self.attribute_value(value),
                        None if self.child(member, Name::AttributeEquals).is_some() => {
                            String::new()
                        }
                        None => "true".to_string(),
                    };
                    match name {
                        "id" => node.anchor = Some(value),
                        "class" => node.attributes.classes.extend(
                            value
                                .split([' ', '\t', '\n', '\u{c}', '\r'])
                                .filter(|class| !class.is_empty())
                                .map(ToString::to_string),
                        ),
                        _ => node.attributes.records.push(Record {
                            name: name.to_string(),
                            value,
                        }),
                    }
                }
                _ => {}
            }
        }
        if node.anchor.as_deref() == Some("") {
            node.anchor = None;
        }
    }

    /// The decoded attribute value at `index`: a quoted one decodes escapes
    /// and character references, and reads a line ending as a space; an
    /// unquoted one decodes escapes.
    fn attribute_value(&self, index: usize) -> String {
        let quoted = self.child(index, Name::AttributeValueMarker).is_some();
        let mut value = String::new();
        for part in self.inside(index) {
            match self.name(part) {
                Name::LineEnding => value.push(' '),
                Name::AttributeValueText => {
                    value.push_str(&unescape(self.text(part), quoted));
                }
                _ => {}
            }
        }
        value
    }

    /// The `data` and line endings inside the token at `index`, in order.
    fn merged_literal(&self, index: usize, data: Name) -> String {
        let mut literal = String::new();
        for child in self.inside(index) {
            let name = self.name(child);
            if name == data || name == Name::LineEnding {
                literal.push_str(&self.value(child));
            }
        }
        literal
    }

    /// The inline content of the part `name` of the token at `index`.
    /// The citation at `index`: a citation token, or a bare key, which is
    /// one of `mode` unless its marker suppresses the author.
    fn citation(&mut self, index: usize, mode: Mode) -> Node {
        let key = if self.name(index) == Name::CitationKey {
            Some(index)
        } else {
            self.child(index, Name::CitationKey)
        };
        let mut name = String::new();
        let mut mode = mode;
        if let Some(key) = key {
            for child in self.inside(key) {
                match self.name(child) {
                    Name::CitationMarker if self.text(child).starts_with('-') => {
                        mode = Mode::SuppressAuthor;
                    }
                    Name::CitationKeyText => name = self.text(child).to_string(),
                    _ => {}
                }
            }
        }
        let (prefix, suffix) = if key == Some(index) {
            (vec![], vec![])
        } else {
            (
                self.part_content(index, Name::CitationPrefix),
                self.part_content(index, Name::CitationSuffix),
            )
        };
        Node::new(
            Kind::Citation {
                referent: Referent::Bib { key: name, mode },
                prefix,
                suffix,
            },
            self.runs(self.start(index), self.end(index)),
        )
    }

    /// In a superscript or subscript, the escaped spaces of the data or
    /// whitespace event at `index`, whose `literal` is to follow `draft`, as
    /// non-breaking spaces: a space after a backslash of data, which may end
    /// the data event before, ending at `after_data`.
    fn script_spaces(
        &self,
        index: usize,
        after_data: Option<usize>,
        literal: &mut String,
        draft: &mut Option<Draft>,
    ) {
        let start = self.start(index);
        if literal.starts_with(' ') && after_data == Some(start) {
            if let Some(text) = draft.as_mut().filter(|text| text.literal.ends_with('\\')) {
                text.literal.pop();
                literal.replace_range(..1, "\u{a0}");
            }
        }
        if self.name(index) == Name::Data {
            *literal = literal.replace("\\ ", "\u{a0}");
        }
    }

    /// The content of a superscript or subscript, where an escaped space is a
    /// non-breaking space.
    fn script_content(&mut self, index: usize, name: Name) -> Vec<Node> {
        self.script += 1;
        let content = self.part_content(index, name);
        self.script -= 1;
        content
    }

    fn part_content(&mut self, index: usize, name: Name) -> Vec<Node> {
        match self.child(index, name) {
            Some(part) => self.label_content(part),
            None => vec![],
        }
    }

    /// An inline formula.
    fn formula(&self, index: usize) -> Kind {
        let mut opener = String::new();
        let mut body = String::new();
        for child in self.inside(index) {
            match self.name(child) {
                Name::FormulaSequence if opener.is_empty() => opener.push_str(self.text(child)),
                Name::FormulaText if self.cell => {
                    body.push_str(&self.text(child).replace("\\|", "|"));
                }
                Name::FormulaText => body.push_str(self.text(child)),
                Name::LineEnding => body.push('\n'),
                _ => {}
            }
        }
        // `$`...`$` loses its backticks; the backslash forms unescape their
        // closing bracket.
        if opener == "$" && body.len() > 1 && body.starts_with('`') && body.ends_with('`') {
            body = body[1..body.len() - 1].to_string();
        } else if opener == "\\\\(" {
            body = body.replace("\\)", ")");
        } else if opener == "\\\\[" {
            body = body.replace("\\]", "]");
        }
        // One space or line ending off each end, when both ends have one and
        // the body is not all of them.
        let pad = |byte: u8| matches!(byte, b' ' | b'\n' | b'\r');
        let bytes = body.as_bytes();
        if bytes.len() > 1
            && pad(bytes[0])
            && pad(bytes[bytes.len() - 1])
            && !bytes.iter().all(|byte| pad(*byte))
        {
            body = body[1..body.len() - 1].to_string();
        }
        Kind::Formula {
            mode: if opener == "$$" || opener == "\\\\[" {
                Placement::Standalone
            } else {
                Placement::Embedded
            },
            literal: body,
        }
    }

    /// The literal of a code span.
    fn code(&self, index: usize, data: Name) -> String {
        let mut literal = String::new();
        for child in self.inside(index) {
            let name = self.name(child);
            if name == data && self.cell {
                literal.push_str(&self.text(child).replace("\\|", "|"));
            } else if name == data {
                literal.push_str(self.text(child));
            } else if name == Name::LineEnding {
                literal.push(' ');
            }
        }
        let bytes = literal.as_bytes();
        if bytes.len() > 2
            && bytes[0] == b' '
            && bytes[bytes.len() - 1] == b' '
            && !bytes.iter().all(|byte| *byte == b' ')
        {
            literal = literal[1..literal.len() - 1].to_string();
        }
        literal
    }

    /// A directive in text: its name, and its label from bracket to bracket.
    fn directive(&mut self, index: usize) -> Kind {
        let name = self
            .child(index, Name::DirectiveNameText)
            .map(|name| self.text(name).to_string())
            .unwrap_or_default();
        let label = self.directive_label(index);
        Kind::Directive { name, label }
    }

    /// The name and the label of the directive (leaf) at `index`, or of the
    /// opening fence of a directive (container).
    fn directive_head(&mut self, index: usize) -> (Option<String>, Option<Box<Node>>) {
        let name = self
            .child(index, Name::DirectiveNameText)
            .map(|name| self.text(name).to_string());
        (name, self.directive_label(index))
    }

    /// The label of the directive at `index`, from bracket to bracket.
    fn directive_label(&mut self, index: usize) -> Option<Box<Node>> {
        self.child(index, Name::DirectiveLabel).map(|label| {
            let runs = self.runs(self.start(label), self.end(label));
            Box::new(Node::new(
                Kind::DirectiveLabel {
                    content: self.label_content(label),
                },
                runs,
            ))
        })
    }

    /// A cross link or a cross embed.
    fn cross_link(&self, index: usize) -> Kind {
        let mut embed = false;
        let mut written = String::new();
        let mut label: Option<String> = None;
        for child in self.inside(index) {
            match self.name(child) {
                Name::CrossLinkMarker if self.text(child).starts_with('!') => embed = true,
                Name::CrossLinkDest => written.push_str(self.text(child)),
                Name::CrossLinkSeparator => label = Some(String::new()),
                Name::CrossLinkLabel => {
                    let text = self.text(child);
                    let found = label.get_or_insert_with(String::new);
                    // In a cell, an escaped pipe is a pipe of the label.
                    if self.cell && !embed {
                        found.push_str(&text.replace("\\|", "|"));
                    } else {
                        found.push_str(text);
                    }
                }
                _ => {}
            }
        }
        let dest = match written.find('#') {
            Some(hash) => {
                let anchor = &written[hash + 1..];
                // A block identifier: `^` and one part of letters, digits,
                // and dashes.
                let id = anchor.strip_prefix('^').filter(|id| {
                    !id.is_empty()
                        && id
                            .bytes()
                            .all(|code| code.is_ascii_alphanumeric() || code == b'-')
                });
                Destination::Cross {
                    path: written[..hash].to_string(),
                    anchor: Some(id.unwrap_or(anchor).to_string()),
                }
            }
            None => Destination::Cross {
                path: written,
                anchor: None,
            },
        };
        if !embed {
            return Kind::CrossLink { dest, label };
        }
        let mut dimensions = None;
        if let Some(text) = &label {
            if let Some((rest, _, found)) = label_dimensions(text, Pipes::Cross) {
                dimensions = Some(found);
                label = Some(rest.to_string());
            }
        }
        Kind::CrossEmbed {
            dest,
            label,
            dimensions,
        }
    }

    /// A link or an image.
    fn link(&mut self, index: usize) -> Kind {
        let label = self.child(index, Name::Label);
        let text = label.and_then(|label| self.child(label, Name::LabelText));
        let mut content = match text {
            Some(text) => self.label_content(text),
            None => vec![],
        };
        let dimensions = match text {
            Some(text) if self.name(index) == Name::Image => {
                self.label_dimensions(text, &mut content)
            }
            _ => None,
        };
        let mut dest = Destination::Url(String::new());
        let mut title = None;
        if let Some(resource) = self.child(index, Name::Resource) {
            if let Some(found) = self.child(resource, Name::ResourceDestination) {
                dest = Destination::Url(self.dest(found));
            }
            if let Some(found) = self.child(resource, Name::ResourceTitle) {
                title = Some(
                    self.child(found, Name::ResourceTitleText)
                        .map(|string| self.string(string, self.exits[string] + 1))
                        .unwrap_or_default(),
                );
            }
        } else {
            let reference = self
                .child(index, Name::Reference)
                .and_then(|reference| self.child(reference, Name::ReferenceText));
            let written = match (reference, text) {
                (Some(string), _) => self.text(string),
                (None, Some(text)) => self.text(text),
                (None, None) => "",
            };
            dest = Destination::Reference(normalize_label(written));
        }
        if self.name(index) == Name::Link {
            Kind::Link {
                dest,
                title,
                content,
            }
        } else {
            Kind::Embed {
                dest,
                title,
                dimensions,
                content,
            }
        }
    }

    /// The dimensions at the end of the image label text at `index`, which
    /// `content` is compiled from, taken out of its last text: after its
    /// last separator, or as all of the label.
    fn label_dimensions(&self, index: usize, content: &mut Vec<Node>) -> Option<Dimensions> {
        let (start, end) = (self.start(index), self.end(index));
        let pipes = if self.cell { Pipes::Cell } else { Pipes::Image };
        let (rest, width, dimensions) = label_dimensions(self.text(index), pipes)?;
        let cut = start + rest.len();
        if rest.is_empty() && content.len() > 1 {
            return None;
        }
        let last = content.last_mut()?;
        let (from, to) = (last.start(), last.end());
        let literal = match &mut last.kind {
            Kind::Text { literal } if from <= cut && to == end => literal,
            _ => return None,
        };
        // The separator, if any, is one byte of the literal, and each digit
        // one.
        let size = end - cut - width.saturating_sub(1);
        if size >= literal.len() {
            content.pop();
        } else {
            literal.truncate(literal.len() - size);
            if let Some(run) = last.runs.last_mut() {
                run.end = cut;
            }
        }
        Some(dimensions)
    }
}

/// `text` with each backslash before ASCII punctuation dropped, and, when
/// `references`, each character reference decoded.
fn unescape(text: &str, references: bool) -> String {
    let bytes = text.as_bytes();
    let mut result = String::new();
    let mut from = 0;
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] == b'\\' && bytes.get(index + 1).map_or(false, u8::is_ascii_punctuation) {
            result.push_str(&text[from..index]);
            from = index + 1;
            index += 2;
        } else if bytes[index] == b'&' && references {
            if let Some((value, end)) = decode_at(text, index) {
                result.push_str(&text[from..index]);
                result.push_str(&value);
                from = end;
                index = end;
            } else {
                index += 1;
            }
        } else {
            index += 1;
        }
    }
    result.push_str(&text[from..]);
    result
}

/// How a label's pipes separate dimensions.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Pipes {
    /// A cross embed's: `|` or `\|`.
    Cross,
    /// An image's: `|`, and a backslash escapes the byte after it.
    Image,
    /// An image's in a table cell, where each `\|` is a `|` first.
    Cell,
}

/// The dimensions at the end of the embed label `label`, after its last
/// separator or as all of it, the label before them, and the width of the
/// separator.
fn label_dimensions(label: &str, pipes: Pipes) -> Option<(&str, usize, Dimensions)> {
    let source = label.as_bytes();
    let mut cut = None;
    let mut index = 0;
    let mut escape = false;
    while index < source.len() {
        let contracted = source[index] == b'\\' && source.get(index + 1) == Some(&b'|');
        let width = if contracted && pipes != Pipes::Image {
            2
        } else {
            1
        };
        let byte = if contracted && pipes == Pipes::Cell {
            b'|'
        } else {
            source[index]
        };
        if escape {
            escape = false;
        } else if byte == b'|' || (contracted && pipes == Pipes::Cross) {
            cut = Some((index, index + width));
        } else if byte == b'\\' && pipes != Pipes::Cross {
            // In an image label, an escaped byte is no separator.
            escape = true;
        }
        index += width;
    }
    let (rest, tail) = match cut {
        Some((before, after)) => (&label[..before], &label[after..]),
        None => ("", label),
    };
    let width = cut.map_or(0, |(before, after)| after - before);
    if rest.ends_with(|c: char| c.is_ascii_whitespace()) {
        return None;
    }
    let (across, height) = match tail.split_once('x') {
        Some((across, height)) => (dimension(across)?, Some(dimension(height)?)),
        None => (dimension(tail)?, None),
    };
    Some((
        rest,
        width,
        Dimensions {
            width: across,
            height,
        },
    ))
}

/// A dimension: digits without a leading zero, at most `i32::MAX`.
fn dimension(text: &str) -> Option<u32> {
    if !text.starts_with(|c: char| ('1'..='9').contains(&c))
        || !text.bytes().all(|code| code.is_ascii_digit())
    {
        return None;
    }
    text.parse::<i32>()
        .ok()
        .and_then(|value| u32::try_from(value).ok())
}

/// The body of the HTML comment `html`, if it is one: the bytes between
/// `<!--` and `-->`, where `<!-->` and `<!--->` are empty.
fn comment_literal(html: &str) -> Option<&str> {
    let rest = html.strip_prefix("<!--")?;
    if rest == ">" || rest == "->" {
        Some("")
    } else {
        rest.strip_suffix("-->")
    }
}

/// The body of the HTML block `html`, if it holds one comment that its own
/// `-->` closes, with only whitespace after that closer.
fn comment_block_literal(html: &str) -> Option<&str> {
    let html = html.trim_start_matches([' ', '\t']);
    if !html.starts_with("<!--") {
        return None;
    }
    let close = html[2..].find("-->")? + 2;
    if !html[close + 3..]
        .trim_end_matches([' ', '\t', '\n'])
        .is_empty()
    {
        return None;
    }
    Some(if close < 4 { "" } else { &html[4..close] })
}

/// Add text to the pending text.
fn push_text(draft: &mut Option<Draft>, start: usize, end: usize, literal: &str, written: bool) {
    let blank = literal.len() - literal.trim_end_matches([' ', '\t']).len();
    let text = draft.get_or_insert_with(|| Draft {
        start,
        end,
        literal: String::new(),
        tail: 0,
    });
    text.end = end;
    text.literal.push_str(literal);
    text.tail = if !written {
        0
    } else if blank == literal.len() {
        text.tail + blank
    } else {
        blank
    };
}

/// The end of the line ending at `index`, if one is there.
/// Where the line holding `index` ends, before its line ending.
fn line_stop(bytes: &[u8], mut index: usize) -> usize {
    while index < bytes.len() && !matches!(bytes[index], b'\r' | b'\n') {
        index += 1;
    }
    index
}

fn line_end(bytes: &[u8], index: usize) -> usize {
    match bytes.get(index) {
        Some(b'\r') if bytes.get(index + 1) == Some(&b'\n') => index + 2,
        Some(b'\r' | b'\n') => index + 1,
        _ => index,
    }
}

// Anchors.

/// Give every heading without an explicit anchor its automatic one, in
/// source order, after every explicit anchor of the document is reserved.
fn anchor_headings(document: &mut Node) {
    let mut taken = BTreeSet::new();
    reserve_anchors(document, &mut taken);
    assign_anchors(document, &mut taken);
}

fn reserve_anchors(node: &Node, taken: &mut BTreeSet<String>) {
    if let Some(anchor) = &node.anchor {
        taken.insert(anchor.clone());
    }
    for child in node.owned() {
        reserve_anchors(child, taken);
    }
}

fn assign_anchors(node: &mut Node, taken: &mut BTreeSet<String>) {
    if node.anchor.is_none() {
        if let Kind::Heading { content, .. } = &node.kind {
            let mut base = String::new();
            for child in content {
                anchor_text(child, &mut base);
            }
            if base.is_empty() {
                base.push_str("section");
            }
            let mut name = base.clone();
            let mut suffix = 0;
            while taken.contains(&name) {
                suffix += 1;
                name = format!("{}-{}", base, suffix);
            }
            taken.insert(name.clone());
            node.anchor = Some(name);
        }
    }
    for child in node.owned_mut() {
        assign_anchors(child, taken);
    }
}

/// Whitespace that a block literal is trimmed of.
const WHITESPACE: &[char] = &[' ', '\t', '\n', '\r'];

/// The anchor projection of an inline node, appended to `base`.
fn anchor_text(node: &Node, base: &mut String) {
    match &node.kind {
        Kind::Text { literal } | Kind::Code { literal } | Kind::Formula { literal, .. } => {
            base.push_str(&anchor(literal));
        }
        Kind::Topic { name } => {
            base.push_str(&anchor("#"));
            base.push_str(&anchor(name));
        }
        Kind::SoftBreak | Kind::LineBreak => base.push('-'),
        Kind::CrossLink { dest, label } | Kind::CrossEmbed { dest, label, .. } => {
            match (label, dest) {
                (Some(label), _) => base.push_str(&anchor(label)),
                (None, Destination::Cross { path, anchor: name }) => {
                    base.push_str(&anchor(path));
                    if let Some(name) = name {
                        base.push_str(&anchor(name));
                    }
                }
                (None, _) => {}
            }
        }
        Kind::Cite { citations } => {
            for citation in citations {
                if let Kind::Citation {
                    referent,
                    prefix,
                    suffix,
                } = &citation.kind
                {
                    match referent {
                        Referent::Bib { key, .. } => {
                            for child in prefix {
                                anchor_text(child, base);
                            }
                            base.push_str(&anchor("@"));
                            base.push_str(&anchor(key));
                            for child in suffix {
                                anchor_text(child, base);
                            }
                        }
                        Referent::Specimen { label } => {
                            base.push_str(&anchor("@"));
                            base.push_str(&anchor(label));
                        }
                        Referent::Footnote(_) => {}
                    }
                }
            }
        }
        Kind::Directive {
            label: Some(label), ..
        } => anchor_text(label, base),
        Kind::Emphasis { content }
        | Kind::Strong { content }
        | Kind::Strikethrough { content }
        | Kind::Mark { content }
        | Kind::Insertion { content }
        | Kind::Span { content }
        | Kind::Superscript { content }
        | Kind::Subscript { content }
        | Kind::Link { content, .. }
        | Kind::Embed { content, .. }
        | Kind::DirectiveLabel { content } => {
            for child in content {
                anchor_text(child, base);
            }
        }
        _ => {}
    }
}
