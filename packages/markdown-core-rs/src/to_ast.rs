//! Turn events into the canonical AST.
//!
//! The walk is recursive over the event tree: a handler takes the index of an
//! enter event and builds that token's node from the events up to its exit.
//! Positions come from the events alone: a node's runs are its token's span
//! less the prefixes of the containers around it.

use crate::ast::{
    Delimiter, Destination, Flavor, Kind, Node, Note, Placement, Referent, Span, Variant,
};
use crate::event::{Block, Event, Inline, Kind as EventKind, Name, Role, Space};
use crate::util::{
    character_reference::decode as decode_character_reference,
    classes::{anchor, normalize_label},
    infer::{list_loose, table_sides},
    slice::{Position, Slice},
};
use alloc::{
    collections::BTreeSet,
    format,
    string::{String, ToString},
    vec,
    vec::Vec,
};
use core::str;

/// Build the document of `source` from its `events`.
pub fn compile(events: &[Event], source: &[u8]) -> Node {
    let mut compiler = Compiler::new(events, source);
    let content = compiler.blocks(0, events.len());
    let mut document = Node::new(
        Kind::Document {
            metadata: None,
            content,
        },
        vec![Span::new(0, line_start_before_end(source))],
    );
    name_headings(&mut document);
    document
}

/// Where the document's span ends: before one final line ending.
fn line_start_before_end(source: &[u8]) -> usize {
    let mut end = source.len();
    if end > 0 && source[end - 1] == b'\n' {
        end -= 1;
        if end > 0 && source[end - 1] == b'\r' {
            end -= 1;
        }
    } else if end > 0 && source[end - 1] == b'\r' {
        end -= 1;
    }
    end
}

struct Compiler<'a> {
    events: &'a [Event],
    source: &'a [u8],
    /// The index of the exit of each enter event.
    exits: Vec<usize>,
    /// Container prefixes of continuation lines, with the whitespace before
    /// them: gaps between the runs of the leaves they interrupt.
    holes: Vec<Span>,
    /// Whether a table cell's content is being compiled, where `\|` in
    /// code is a pipe.
    cell: bool,
}

/// Text read in an inline content list and not yet a node.
struct Draft {
    start: usize,
    end: usize,
    literal: String,
}

/// Whether `name` is a container prefix of a continuation line.
fn prefix(name: Name) -> bool {
    matches!(
        name,
        Name::BlockQuotePrefix | Name::ListItemIndent | Name::FootnoteDefinitionIndent
    )
}

/// Whether `name` is whitespace between blocks rather than content.
fn between(name: Name) -> bool {
    matches!(
        name,
        Name::LineEnd | Name::BlankLineEnd | Name::SpaceOrTab | Name::Bom
    ) || prefix(name)
}

impl<'a> Compiler<'a> {
    fn new(events: &'a [Event], source: &'a [u8]) -> Compiler<'a> {
        let mut exits = vec![0; events.len()];
        let mut stack = vec![];
        let mut holes = vec![];
        for (index, event) in events.iter().enumerate() {
            match event.kind {
                EventKind::Enter => stack.push(index),
                EventKind::Exit => {
                    let enter = stack.pop().expect("expected an enter for each exit");
                    exits[enter] = index;
                    if prefix(event.name) {
                        holes.push(Span::new(events[enter].point.index, event.point.index));
                    }
                }
            }
        }
        for hole in &mut holes {
            while hole.start > 0 && matches!(source[hole.start - 1], b' ' | b'\t') {
                hole.start -= 1;
            }
        }
        holes.sort_by_key(|hole: &Span| hole.start);
        Compiler {
            events,
            source,
            exits,
            holes,
            cell: false,
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
        str::from_utf8(&self.source[start..end]).expect("expected events at char boundaries")
    }

    fn text(&self, index: usize) -> &'a str {
        self.slice(self.start(index), self.end(index))
    }

    /// The text of the token at `index`, the columns of a tab it starts or
    /// ends inside written as spaces.
    fn value(&self, index: usize) -> String {
        Slice::from_position(
            self.source,
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
        let mut fused: Vec<Span> = vec![];
        for run in runs {
            match fused.last_mut() {
                Some(last) if last.end == run.start => last.end = run.end,
                _ => fused.push(run),
            }
        }
        fused
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
        while at < end && matches!(self.source[at], b' ' | b'\t') {
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
                // A blank line ends a block quote: it is not inside it.
                if event.name == Name::BlockQuote {
                    break;
                }
                index += 1;
                continue;
            }
            match event.name {
                Name::BlankLineEnd => {
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

    fn blocks(&mut self, from: usize, to: usize) -> Vec<Node> {
        let mut nodes = vec![];
        for index in self.children(from, to) {
            if let Some(node) = self.block(index) {
                nodes.push(node);
            }
        }
        nodes
    }

    /// The blocks inside the container at `index`.
    fn contained(&mut self, index: usize) -> Vec<Node> {
        self.blocks(index + 1, self.exits[index])
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
                    Some(text) => self.inlines(text),
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
                    Some(text) => self.inlines(text),
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
            Block::CodeIndented => self.code_indented(index),
            Block::CodeFenced => self.code_fenced(index),
            Block::HtmlFlow => {
                let literal = self.lines(index, Name::HtmlFlowData);
                let kind = match html_comment_block(&literal) {
                    Some(inner) => Kind::Comment {
                        literal: inner.to_string(),
                    },
                    None => Kind::HtmlBlock { literal },
                };
                Node::new(
                    kind,
                    self.runs(self.lead(index), self.last(index).1),
                )
            }
            Block::Quote => {
                let runs = Self::whole(self.lead(index), self.end(index));
                let content = self.contained(index);
                Node::new(
                    Kind::Callout {
                        variant: None,
                        collapsed: None,
                        title: None,
                        content,
                    },
                    runs,
                )
            }
            Block::List => self.list(index),
            Block::Definition => self.definition(index),
            Block::Table => self.table(index),
            Block::Footnote => {
                let label = self
                    .child(index, Name::FootnoteDefinitionPrefix)
                    .and_then(|prefix| self.child(prefix, Name::FootnoteDefinitionLabel))
                    .and_then(|label| self.child(label, Name::FootnoteDefinitionLabelText))
                    .map(|string| normalize_label(self.text(string)));
                let (last, content_end) = self.last(index);
                let end = self.blank_tail(last).unwrap_or(content_end);
                let runs = Self::whole(self.lead(index), end);
                let content = self.contained(index);
                Node::new(Kind::Footnote { label, content }, runs)
            }
            Block::MathFlow => {
                let literal = self.fenced_literal(index, Name::MathFlowFence, Name::MathFlowChunk);
                Node::new(
                    Kind::FormulaBlock { literal },
                    self.runs(self.lead(index), self.end(index)),
                )
            }
            // Compiled with the dialect’s properties, which are not ported yet.
            Block::Frontmatter => return None,
        };
        Some(node)
    }

    fn paragraph(&mut self, index: usize) -> Node {
        // A task check is the list item's marker, not paragraph content.
        let mut start = self.start(index);
        let mut from = index + 1;
        if let Some(first) = self.inside(index).first().copied() {
            if self.name(first) == Name::TaskListItemCheck {
                from = self.exits[first] + 1;
                if let Some(next) = self.children(from, self.exits[index]).first() {
                    if self.name(*next) == Name::SpaceOrTab {
                        from = self.exits[*next] + 1;
                    }
                }
                start = self.events[from].point.index;
            }
        }
        let content = self.inline_span(from, self.exits[index]);
        Node::new(
            Kind::Paragraph { content },
            self.runs(self.skip(start, self.end(index)), self.end(index)),
        )
    }

    /// The lines of the token at `index`: the `data` on each, every line
    /// with a line feed, and no line after a final line ending.
    fn lines(&self, index: usize, data: Name) -> String {
        let mut literal = String::new();
        let mut line = String::new();
        let mut open = false;
        for child in self.inside(index) {
            let name = self.name(child);
            if name == data {
                line.push_str(&self.value(child));
                open = true;
            } else if name == Name::LineEnd {
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

    fn code_indented(&self, index: usize) -> Node {
        let mut literal = self.lines(index, Name::CodeFlowChunk);
        // Blank lines at the end are not the code's.
        while literal.ends_with('\n') {
            let rest = &literal[..literal.len() - 1];
            let line = rest.rfind('\n').map_or(0, |at| at + 1);
            if rest[line..]
                .bytes()
                .all(|code| matches!(code, b' ' | b'\t'))
                && !rest.is_empty()
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
                script: None,
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

    fn code_fenced(&self, index: usize) -> Node {
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
                info = Some(self.string(*first, self.exits[*last] + 1));
            }
        }
        let script = info.as_ref().map(|info| {
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
        Node::new(
            Kind::CodeBlock {
                info,
                script,
                literal,
                fenced: true,
                closed,
            },
            self.runs(self.lead(index), self.last(index).1),
        )
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
            } else if name == Name::LineEnd {
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
        let ordered = self.name(index) == Name::ListOrdered;
        let loose = list_loose(self.events, index, true);
        let mut items = vec![];
        let mut value = None;
        let mut marker = 0u8;
        for child in self.inside(index) {
            if self.name(child) != Name::ListItem {
                continue;
            }
            if items.is_empty() {
                if let Some(prefix) = self.child(child, Name::ListItemPrefix) {
                    if let Some(found) = self.child(prefix, Name::ListItemValue) {
                        value = self.text(found).parse::<u64>().ok();
                    }
                    if let Some(found) = self.child(prefix, Name::ListItemMarker) {
                        marker = self.source[self.start(found)];
                    }
                }
            }
            items.push(self.item(child));
        }
        let start = items.first().map_or(self.start(index), Node::start);
        let end = items.last().map_or(self.end(index), Node::end).max(
            self.blank_tail(self.exits[index])
                .unwrap_or_else(|| self.end(index)),
        );
        let (flavor, start_value, variant, delimiter) = if ordered {
            (
                Flavor::Ordered,
                Some(value.unwrap_or(1)),
                Some(Variant::Decimal),
                Some(if marker == b')' {
                    Delimiter::Parenthesis { closed: false }
                } else {
                    Delimiter::Period
                }),
            )
        } else {
            (Flavor::Bullet, None, None, None)
        };
        Node::new(
            Kind::List {
                flavor,
                start: start_value,
                variant,
                delimiter,
                dense: !loose,
                items,
            },
            Self::whole(start, end),
        )
    }

    fn item(&mut self, index: usize) -> Node {
        let (last, content_end) = self.last(index);
        // An item with no content is closed by the blank line after it.
        let end = if self.name(last) == Name::ListItemPrefix {
            line_stop(self.source, content_end)
        } else {
            self.blank_tail(last).unwrap_or(content_end)
        };
        let runs = Self::whole(self.lead(index), end);
        let marker = self.task(index);
        let content = self.contained(index);
        Node::new(Kind::ListItem { marker, content }, runs)
    }

    /// The task marker of the list item at `index`: the check its first
    /// paragraph starts with.
    fn task(&self, index: usize) -> Option<String> {
        let paragraph = self.inside(index).into_iter().find(|child| {
            !between(self.name(*child)) && self.name(*child) != Name::ListItemPrefix
        })?;
        if self.name(paragraph) != Name::Paragraph {
            return None;
        }
        let check = *self.inside(paragraph).first()?;
        if self.name(check) != Name::TaskListItemCheck {
            return None;
        }
        self.inside(check)
            .into_iter()
            .find(|child| {
                matches!(
                    self.name(*child),
                    Name::TaskListItemValueChecked | Name::TaskListItemValueUnchecked
                )
            })
            .map(|value| self.text(value).to_string())
    }

    fn definition(&self, index: usize) -> Node {
        let label = self
            .child(index, Name::DefinitionLabel)
            .and_then(|label| self.child(label, Name::DefinitionLabelText))
            .map(|string| normalize_label(self.text(string)))
            .unwrap_or_default();
        let dest = self
            .child(index, Name::DefinitionDestination)
            .map(|dest| self.destination(dest))
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
    fn destination(&self, index: usize) -> String {
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
                return self.destination(child);
            }
        }
        String::new()
    }

    fn table(&mut self, index: usize) -> Node {
        let columns = table_sides(self.events, index);
        let mut head = vec![];
        let mut content = vec![];
        for child in self.inside(index) {
            let into = match self.name(child) {
                Name::TableHead => &mut head,
                Name::TableRest => &mut content,
                _ => continue,
            };
            for row in self.inside(child) {
                if self.name(row) == Name::TableRow {
                    into.push(self.row(row, columns.len()));
                }
            }
        }
        Node::new(
            Kind::Table {
                columns,
                head,
                content,
            },
            self.runs(self.lead(index), self.end(index)),
        )
    }

    fn row(&mut self, index: usize, width: usize) -> Node {
        let mut cells = vec![];
        for child in self.inside(index) {
            if self.name(child) != Name::TableCell {
                continue;
            }
            self.cell = true;
            let content = match self.child(child, Name::TableCellText) {
                Some(text) => self.inlines(text),
                None => vec![],
            };
            self.cell = false;
            // A cell lies between its dividers.
            let mut start = self.start(child);
            let mut end = self.end(child);
            for part in self.inside(child) {
                if self.name(part) == Name::TableCellDivider {
                    if self.start(part) == start {
                        start = self.end(part);
                    } else {
                        end = self.start(part);
                    }
                }
            }
            cells.push(Node::new(
                Kind::TableCell { content },
                self.runs(start, end),
            ));
        }
        // A missing cell takes the last byte of its row.
        let end = self.end(index);
        let start = end.saturating_sub(1).max(self.start(index));
        while cells.len() < width {
            cells.push(Node::new(
                Kind::TableCell { content: vec![] },
                vec![Span::new(start, end)],
            ));
        }
        cells.truncate(width);
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
                    Name::Data | Name::SpaceOrTab | Name::LineEnd => {
                        value.push_str(self.text(index));
                        index = self.exits[index];
                    }
                    Name::CharacterEscape => {
                        value.push_str(&self.text(index)[1..]);
                        index = self.exits[index];
                    }
                    Name::CharacterReference => {
                        value.push_str(&self.reference(index));
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
    fn reference(&self, index: usize) -> String {
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

    /// The inline content inside the token at `index`.
    fn inlines(&mut self, index: usize) -> Vec<Node> {
        self.inline_span(index + 1, self.exits[index])
    }

    fn inline_span(&mut self, from: usize, to: usize) -> Vec<Node> {
        let mut nodes = vec![];
        let mut draft: Option<Draft> = None;
        let mut hard = false;
        let mut line_start = true;
        let mut skip_end = false;
        for index in self.children(from, to) {
            let name = self.name(index);
            let at_line_start = line_start;
            line_start = false;
            match name.role() {
                Role::Space(Space::Whitespace) if at_line_start => {}
                Role::Inline(Inline::Data) | Role::Space(Space::Whitespace) => {
                    let literal = self.value(index);
                    push_text(&mut draft, self.start(index), self.end(index), &literal);
                }
                Role::Inline(Inline::CharacterEscape) => {
                    let literal = self.text(index)[1..].to_string();
                    push_text(&mut draft, self.start(index), self.end(index), &literal);
                }
                Role::Inline(Inline::CharacterReference) => {
                    let literal = self.reference(index);
                    push_text(&mut draft, self.start(index), self.end(index), &literal);
                }
                Role::Inline(Inline::HardBreakTail) => {
                    push_text(&mut draft, self.start(index), self.end(index), "");
                    hard = true;
                }
                Role::Space(Space::LineEnd) => {
                    line_start = true;
                    if skip_end {
                        skip_end = false;
                        continue;
                    }
                    if let Some(text) = &mut draft {
                        let trimmed = text.literal.trim_end_matches([' ', '\t']).len();
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
                    let end = line_end(self.source, self.end(index));
                    nodes.push(Node::new(
                        Kind::LineBreak,
                        self.runs(self.start(index), end),
                    ));
                    skip_end = true;
                }
                Role::Inline(_)
                | Role::Space(Space::BlankLineEnd)
                | Role::Part
                | Role::Block(_) => {
                    self.flush(&mut nodes, &mut draft);
                    if let Some(node) = self.inline(index) {
                        nodes.push(node);
                    }
                }
            }
        }
        self.flush(&mut nodes, &mut draft);
        // Whitespace at the end of the content is not content.
        if let Some(Node {
            kind: Kind::Text { literal },
            runs,
            ..
        }) = nodes.last_mut()
        {
            let trimmed = literal.trim_end_matches([' ', '\t']).len();
            let cut = literal.len() - trimmed;
            if cut > 0 {
                literal.truncate(trimmed);
                let last = runs.len() - 1;
                runs[last].end -= cut.min(runs[last].end - runs[last].start);
            }
            if literal.is_empty() {
                nodes.pop();
            }
        }
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
                content: self.part(index, Name::EmphasisText),
            },
            Inline::Strong => Kind::Strong {
                content: self.part(index, Name::StrongText),
            },
            Inline::Strike => Kind::Strike {
                content: self.part(index, Name::StrikeText),
            },
            Inline::Insertion => Kind::Insertion {
                content: self.part(index, Name::InsertionText),
            },
            Inline::Mark => Kind::Mark {
                content: self.part(index, Name::MarkText),
            },
            Inline::Subscript => Kind::Subscript {
                content: self.part(index, Name::SubscriptText),
            },
            Inline::Superscript => Kind::Superscript {
                content: self.part(index, Name::SuperscriptText),
            },
            Inline::CodeText => Kind::Code {
                literal: self.code(index, Name::CodeTextData),
            },
            Inline::MathText => Kind::Formula {
                mode: Placement::Embedded,
                literal: self.code(index, Name::MathTextData),
            },
            Inline::HtmlText => {
                let literal = self.fused(index, Name::HtmlTextData);
                match html_comment(&literal) {
                    Some(inner) => Kind::Comment {
                        literal: inner.to_string(),
                    },
                    None => Kind::Html { literal },
                }
            }
            Inline::Comment => Kind::Comment {
                literal: self.fused(index, Name::CommentText),
            },
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
            Inline::Media => self.media(index),
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
            // Text, compiled by `inline_span`.
            Inline::Data
            | Inline::CharacterEscape
            | Inline::CharacterReference
            | Inline::HardBreakTail
            | Inline::HardBreakEscape => return None,
        };
        Some(Node::new(kind, runs))
    }

    /// The `data` and line endings inside the token at `index`, in order.
    fn fused(&self, index: usize, data: Name) -> String {
        let mut literal = String::new();
        for child in self.inside(index) {
            let name = self.name(child);
            if name == data || name == Name::LineEnd {
                literal.push_str(&self.value(child));
            }
        }
        literal
    }

    /// The inline content of the part `name` of the token at `index`.
    fn part(&mut self, index: usize, name: Name) -> Vec<Node> {
        match self.child(index, name) {
            Some(part) => self.inlines(part),
            None => vec![],
        }
    }

    /// The literal of a code span or inline formula.
    fn code(&self, index: usize, data: Name) -> String {
        let mut literal = String::new();
        for child in self.inside(index) {
            let name = self.name(child);
            if name == data && self.cell {
                literal.push_str(&self.text(child).replace("\\|", "|"));
            } else if name == data {
                literal.push_str(self.text(child));
            } else if name == Name::LineEnd {
                literal.push(' ');
            }
        }
        let source = literal.as_bytes();
        if source.len() > 2
            && source[0] == b' '
            && source[source.len() - 1] == b' '
            && !source.iter().all(|code| *code == b' ')
        {
            literal = literal[1..literal.len() - 1].to_string();
        }
        literal
    }

    /// A link or an image.
    fn media(&mut self, index: usize) -> Kind {
        let label = self.child(index, Name::Label);
        let text = label.and_then(|label| self.child(label, Name::LabelText));
        let content = match text {
            Some(text) => self.inlines(text),
            None => vec![],
        };
        let mut dest = Destination::Url(String::new());
        let mut title = None;
        if let Some(resource) = self.child(index, Name::Resource) {
            if let Some(found) = self.child(resource, Name::ResourceDestination) {
                dest = Destination::Url(self.destination(found));
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
            Kind::Embedded {
                dest,
                title,
                dimensions: None,
                content,
            }
        }
    }
}

/// Add text to the pending text.
/// The body of the HTML comment `html`, if it is one: the bytes between
/// `<!--` and `-->`, where `<!-->` and `<!--->` are empty.
fn html_comment(html: &str) -> Option<&str> {
    let rest = html.strip_prefix("<!--")?;
    if rest == ">" || rest == "->" {
        Some("")
    } else {
        rest.strip_suffix("-->")
    }
}

/// The body of the HTML block `html`, if it holds one comment that its own
/// `-->` closes, with only whitespace after that closer.
fn html_comment_block(html: &str) -> Option<&str> {
    let html = html.trim_start_matches([' ', '\t']);
    if !html.starts_with("<!--") {
        return None;
    }
    let close = html[2..].find("-->")? + 2;
    if !html[close + 3..].trim_end_matches([' ', '\t', '\n']).is_empty() {
        return None;
    }
    Some(if close < 4 { "" } else { &html[4..close] })
}

fn push_text(draft: &mut Option<Draft>, start: usize, end: usize, literal: &str) {
    match draft {
        Some(text) => {
            text.end = end;
            text.literal.push_str(literal);
        }
        None => {
            *draft = Some(Draft {
                start,
                end,
                literal: literal.to_string(),
            });
        }
    }
}

/// The end of the line ending at `index`, if one is there.
/// Where the line holding `index` ends, before its line ending.
fn line_stop(source: &[u8], mut index: usize) -> usize {
    while index < source.len() && !matches!(source[index], b'\r' | b'\n') {
        index += 1;
    }
    index
}

fn line_end(source: &[u8], index: usize) -> usize {
    match source.get(index) {
        Some(b'\r') if source.get(index + 1) == Some(&b'\n') => index + 2,
        Some(b'\r' | b'\n') => index + 1,
        _ => index,
    }
}

// Anchors.

/// Give every heading without an explicit anchor its automatic one, in
/// source order, after every explicit anchor of the document is reserved.
fn name_headings(document: &mut Node) {
    let mut taken = BTreeSet::new();
    reserve(document, &mut taken);
    allot(document, &mut taken);
}

fn reserve(node: &Node, taken: &mut BTreeSet<String>) {
    if let Some(anchor) = &node.anchor {
        taken.insert(anchor.clone());
    }
    for child in node.owned() {
        reserve(child, taken);
    }
}

fn allot(node: &mut Node, taken: &mut BTreeSet<String>) {
    if node.anchor.is_none() {
        if let Kind::Heading { content, .. } = &node.kind {
            let mut base = String::new();
            for child in content {
                flatten(child, &mut base);
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
        allot(child, taken);
    }
}

/// The anchor projection of an inline node, appended to `base`.
fn flatten(node: &Node, base: &mut String) {
    match &node.kind {
        Kind::Text { literal } | Kind::Code { literal } | Kind::Formula { literal, .. } => {
            base.push_str(&anchor(literal));
        }
        Kind::Topic { name } => {
            base.push_str(&anchor("#"));
            base.push_str(&anchor(name));
        }
        Kind::SoftBreak | Kind::LineBreak => base.push('-'),
        Kind::CrossLink { dest, label } | Kind::CrossEmbedded { dest, label, .. } => {
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
                        Referent::Bib { id, .. } => {
                            for child in prefix {
                                flatten(child, base);
                            }
                            base.push_str(&anchor("@"));
                            base.push_str(&anchor(id));
                            for child in suffix {
                                flatten(child, base);
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
        } => flatten(label, base),
        Kind::Emphasis { content }
        | Kind::Strong { content }
        | Kind::Strike { content }
        | Kind::Mark { content }
        | Kind::Insertion { content }
        | Kind::Span { content }
        | Kind::Superscript { content }
        | Kind::Subscript { content }
        | Kind::Link { content, .. }
        | Kind::Embedded { content, .. }
        | Kind::DirectiveLabel { content } => {
            for child in content {
                flatten(child, base);
            }
        }
        _ => {}
    }
}
