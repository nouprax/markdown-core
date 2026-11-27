//! The canonical AST debug dump (`docs/specs/canonical-ast-dump.md`).

use crate::ast::{
    Attributes, Delimiter, Destination, Dimensions, Fields, Flavor, Flow, Item, Kind, Mode, Node,
    Note, Placement, Referent, Scalar, Span, Status, Value,
};
use alloc::{format, string::String, vec::Vec};
use core::fmt::Write;

/// Dump `document`, parsed from `source`, as the canonical file tree.
pub fn dump(document: &Node, source: &str) -> String {
    let mut out = Out {
        lines: Lines::new(source.as_bytes()),
        text: String::new(),
        prefix: String::new(),
    };
    out.node(document, None);
    out.text
}

/// Line starts of the source, to turn byte offsets into scopes.
struct Lines {
    starts: Vec<usize>,
}

impl Lines {
    fn new(bytes: &[u8]) -> Lines {
        let mut starts = alloc::vec![0];
        let mut index = 0;
        while index < bytes.len() {
            match bytes[index] {
                b'\n' => starts.push(index + 1),
                b'\r' => {
                    if index + 1 < bytes.len() && bytes[index + 1] == b'\n' {
                        index += 1;
                    }
                    starts.push(index + 1);
                }
                _ => {}
            }
            index += 1;
        }
        Lines { starts }
    }

    /// The line (from 1) holding `offset` and that line's start.
    fn line(&self, offset: usize) -> (usize, usize) {
        let line = match self.starts.binary_search(&offset) {
            Ok(index) => index,
            Err(index) => index - 1,
        };
        (line + 1, self.starts[line])
    }

    fn scope(&self, span: Span) -> String {
        let (start_line, start_base) = self.line(span.start);
        let (end_line, end_base) = self.line(span.end);
        format!(
            "{}:{}..{}:{}",
            start_line,
            span.start - start_base + 1,
            end_line,
            span.end - end_base
        )
    }
}

struct Out {
    lines: Lines,
    text: String,
    prefix: String,
}

/// One child line of a node: a node, or a group of nodes.
enum Row<'a> {
    Node(&'a Node),
    Group(&'static str, &'a [Node]),
    /// A group that holds a node besides its nodes (a citation's note).
    Note(&'a Node),
}

impl Out {
    /// Write `line` at the current depth, with `last` saying which connector.
    fn line(&mut self, connector: Option<bool>, line: &str) {
        self.text.push_str(&self.prefix);
        match connector {
            None => {}
            Some(true) => self.text.push_str("└── "),
            Some(false) => self.text.push_str("├── "),
        }
        self.text.push_str(line);
        self.text.push('\n');
    }

    fn entries(&mut self, connector: Option<bool>, entries: &[Row]) {
        let saved = self.prefix.len();
        match connector {
            None => {}
            Some(true) => self.prefix.push_str("    "),
            Some(false) => self.prefix.push_str("│   "),
        }
        let count = entries.len();
        for (index, entry) in entries.iter().enumerate() {
            let last = Some(index + 1 == count);
            match entry {
                Row::Node(node) | Row::Note(node) => self.node(node, last),
                Row::Group(name, nodes) => {
                    self.line(last, &format!("{} children={}", name, nodes.len()));
                    let inner: Vec<Row> = nodes.iter().map(Row::Node).collect();
                    self.entries(last, &inner);
                }
            }
        }
        self.prefix.truncate(saved);
    }

    fn node(&mut self, node: &Node, connector: Option<bool>) {
        let mut line = String::new();
        line.push_str(name(&node.kind));
        line.push_str(" scope=");
        let scopes: Vec<String> = node.runs.iter().map(|run| self.lines.scope(*run)).collect();
        line.push_str(&scopes.join(","));
        line.push_str(" anchor=");
        opt_text(&mut line, node.anchor.as_deref());
        line.push_str(" attributes=");
        attributes(&mut line, &node.attributes);
        let (count, entries) = fields(&mut line, &node.kind);
        let _ = write!(line, " children={}", count);
        self.line(connector, &line);
        self.entries(connector, &entries);
    }
}

fn name(kind: &Kind) -> &'static str {
    match kind {
        Kind::Document { .. } => "Document",
        Kind::Callout { .. } => "Callout",
        Kind::Paragraph { .. } => "Paragraph",
        Kind::Heading { .. } => "Heading",
        Kind::ThematicBreak => "ThematicBreak",
        Kind::List { .. } => "List",
        Kind::ListItem { .. } => "ListItem",
        Kind::CodeBlock { .. } => "CodeBlock",
        Kind::HtmlBlock { .. } => "HTMLBlock",
        Kind::FormulaBlock { .. } => "FormulaBlock",
        Kind::Table { .. } => "Table",
        Kind::TableRow { .. } => "TableRow",
        Kind::TableCell { .. } => "TableCell",
        Kind::DirectiveBlock { .. } => "DirectiveBlock",
        Kind::DirectiveLabel { .. } => "DirectiveLabel",
        Kind::Text { .. } => "Text",
        Kind::SoftBreak => "SoftBreak",
        Kind::LineBreak => "LineBreak",
        Kind::Code { .. } => "Code",
        Kind::Html { .. } => "HTML",
        Kind::CrossLink { .. } => "CrossLink",
        Kind::CrossEmbed { .. } => "CrossEmbed",
        Kind::Comment { .. } => "Comment",
        Kind::Formula { .. } => "Formula",
        Kind::Emphasis { .. } => "Emphasis",
        Kind::Strong { .. } => "Strong",
        Kind::Strikethrough { .. } => "Strikethrough",
        Kind::Mark { .. } => "Mark",
        Kind::Insertion { .. } => "Insertion",
        Kind::Span { .. } => "Span",
        Kind::Superscript { .. } => "Superscript",
        Kind::Subscript { .. } => "Subscript",
        Kind::Link { .. } => "Link",
        Kind::Embed { .. } => "Embed",
        Kind::Directive { .. } => "Directive",
        Kind::Cite { .. } => "Cite",
        Kind::Topic { .. } => "Topic",
        Kind::DefinitionList { .. } => "DefinitionList",
        Kind::Definition { .. } => "Definition",
        Kind::Citation { .. } => "Citation",
        Kind::Footnote { .. } => "Footnote",
        Kind::Metadata { .. } => "Metadata",
        Kind::Reference { .. } => "Reference",
    }
}

fn nodes(list: &[Node]) -> Vec<Row<'_>> {
    list.iter().map(Row::Node).collect()
}

/// Write the kind's fields; answer its `children` count and its child lines.
fn fields<'a>(line: &mut String, kind: &'a Kind) -> (usize, Vec<Row<'a>>) {
    match kind {
        Kind::Document { metadata, content } => {
            let mut entries = Vec::new();
            if let Some(metadata) = metadata {
                entries.push(Row::Node(metadata));
            }
            entries.extend(content.iter().map(Row::Node));
            (content.len(), entries)
        }
        Kind::Callout {
            variant,
            collapsed,
            title,
            content,
        } => {
            line.push_str(" variant=");
            opt_text(line, variant.as_deref());
            line.push_str(" collapsed=");
            option_bool(line, *collapsed);
            let mut entries = Vec::new();
            if let Some(title) = title {
                entries.push(Row::Group("Title", title));
            }
            entries.extend(content.iter().map(Row::Node));
            (content.len(), entries)
        }
        Kind::Paragraph { content }
        | Kind::TableCell { content }
        | Kind::DirectiveLabel { content }
        | Kind::Emphasis { content }
        | Kind::Strong { content }
        | Kind::Strikethrough { content }
        | Kind::Mark { content }
        | Kind::Insertion { content }
        | Kind::Span { content }
        | Kind::Superscript { content }
        | Kind::Subscript { content } => (content.len(), nodes(content)),
        Kind::Heading { level, content } => {
            let _ = write!(line, " level={}", level);
            (content.len(), nodes(content))
        }
        Kind::ThematicBreak | Kind::SoftBreak | Kind::LineBreak => (0, Vec::new()),
        Kind::List {
            flavor,
            start,
            delimiter,
            tight,
            items,
        } => {
            line.push_str(match flavor {
                Flavor::Bullet => " flavor=bullet",
                Flavor::Ordered => " flavor=ordered",
            });
            line.push_str(" start=");
            match start {
                Some(start) => {
                    let _ = write!(line, "{}", start);
                }
                None => line.push_str("null"),
            }
            line.push_str(match delimiter {
                None => " delimiter=null",
                Some(Delimiter::Period) => " delimiter=period",
                Some(Delimiter::Parenthesis) => " delimiter=parenthesis",
            });
            let _ = write!(line, " tight={}", tight);
            (items.len(), nodes(items))
        }
        Kind::ListItem { status, content } => {
            line.push_str(match status {
                None => " status=null",
                Some(Status::Open) => " status=open",
                Some(Status::Done) => " status=done",
                Some(Status::Question) => " status=question",
                Some(Status::Cancelled) => " status=cancelled",
            });
            (content.len(), nodes(content))
        }
        Kind::CodeBlock {
            info,
            language,
            literal,
            fenced,
            closed,
        } => {
            line.push_str(" info=");
            opt_text(line, info.as_deref());
            line.push_str(" language=");
            opt_text(line, language.as_deref());
            line.push_str(" literal=");
            string(line, literal);
            let _ = write!(line, " fenced={} closed={}", fenced, closed);
            (0, Vec::new())
        }
        Kind::HtmlBlock { literal }
        | Kind::FormulaBlock { literal }
        | Kind::Text { literal }
        | Kind::Code { literal }
        | Kind::Html { literal }
        | Kind::Comment { literal } => {
            line.push_str(" literal=");
            string(line, literal);
            (0, Vec::new())
        }
        Kind::Table {
            columns,
            head,
            content,
        } => {
            line.push_str(" columns=[");
            for (index, column) in columns.iter().enumerate() {
                if index > 0 {
                    line.push(',');
                }
                line.push_str(match column {
                    Flow::None => "none",
                    Flow::Left => "left",
                    Flow::Center => "center",
                    Flow::Right => "right",
                });
            }
            line.push(']');
            (
                head.len() + content.len(),
                alloc::vec![
                    Row::Group("TableHead", head),
                    Row::Group("TableBody", content)
                ],
            )
        }
        Kind::TableRow { cells } => (cells.len(), nodes(cells)),
        Kind::DirectiveBlock {
            name,
            label,
            content,
        } => {
            line.push_str(" name=");
            string(line, name);
            let mut entries = Vec::new();
            if let Some(label) = label {
                entries.push(Row::Node(label));
            }
            entries.extend(content.iter().map(Row::Node));
            (content.len(), entries)
        }
        Kind::CrossLink { dest, label } => {
            line.push_str(" dest=");
            destination(line, dest);
            line.push_str(" label=");
            opt_text(line, label.as_deref());
            (0, Vec::new())
        }
        Kind::CrossEmbed {
            dest,
            label,
            dimensions,
        } => {
            line.push_str(" dest=");
            destination(line, dest);
            line.push_str(" label=");
            opt_text(line, label.as_deref());
            line.push_str(" dimensions=");
            option_dimensions(line, *dimensions);
            (0, Vec::new())
        }
        Kind::Formula { mode, literal } => {
            line.push_str(match mode {
                Placement::Embedded => " mode=embedded",
                Placement::Standalone => " mode=standalone",
            });
            line.push_str(" literal=");
            string(line, literal);
            (0, Vec::new())
        }
        Kind::Link {
            dest,
            title,
            content,
        } => {
            line.push_str(" dest=");
            destination(line, dest);
            line.push_str(" title=");
            opt_text(line, title.as_deref());
            (content.len(), nodes(content))
        }
        Kind::Embed {
            dest,
            title,
            dimensions,
            content,
        } => {
            line.push_str(" dest=");
            destination(line, dest);
            line.push_str(" title=");
            opt_text(line, title.as_deref());
            line.push_str(" dimensions=");
            option_dimensions(line, *dimensions);
            (content.len(), nodes(content))
        }
        Kind::Directive { name, label } => {
            line.push_str(" name=");
            string(line, name);
            let mut entries = Vec::new();
            if let Some(label) = label {
                entries.push(Row::Node(label));
            }
            (0, entries)
        }
        Kind::Cite { citations } => (citations.len(), nodes(citations)),
        Kind::Topic { name } => {
            line.push_str(" name=");
            string(line, name);
            (0, Vec::new())
        }
        Kind::DefinitionList { definitions } => (definitions.len(), nodes(definitions)),
        Kind::Definition {
            term,
            bodies,
            compact,
        } => {
            let _ = write!(line, " compact={}", compact);
            let mut entries = alloc::vec![Row::Group("DefinitionTerm", term)];
            entries.extend(bodies.iter().map(|body| Row::Group("DefinitionBody", body)));
            (bodies.len(), entries)
        }
        Kind::Citation {
            referent,
            prefix,
            suffix,
        } => {
            line.push_str(" referent=");
            let mut entries = Vec::new();
            match referent {
                Referent::Bib { key, mode } => {
                    line.push_str("bib(key=");
                    string(line, key);
                    line.push_str(match mode {
                        Mode::Normal => ",mode=normal)",
                        Mode::AuthorInText => ",mode=authorInText)",
                        Mode::SuppressAuthor => ",mode=suppressAuthor)",
                    });
                }
                Referent::Footnote(Note::Label(label)) => {
                    line.push_str("footnote(label=");
                    string(line, label);
                    line.push(')');
                }
                Referent::Footnote(Note::Inline(note)) => {
                    line.push_str("footnote(note)");
                    entries.push(Row::Note(note));
                }
            }
            entries.push(Row::Group("CitationPrefix", prefix));
            entries.push(Row::Group("CitationSuffix", suffix));
            (0, entries)
        }
        Kind::Footnote { label, content } => {
            line.push_str(" label=");
            opt_text(line, label.as_deref());
            (content.len(), nodes(content))
        }
        Kind::Metadata { fields } => {
            metadata(line, fields);
            (0, Vec::new())
        }
        Kind::Reference { label, dest, title } => {
            line.push_str(" label=");
            string(line, label);
            line.push_str(" dest=");
            destination(line, dest);
            line.push_str(" title=");
            opt_text(line, title.as_deref());
            (0, Vec::new())
        }
    }
}

fn metadata(line: &mut String, fields: &Fields) {
    let Fields {
        name,
        title,
        subtitle,
        time,
        date,
        authors,
        keywords,
        abstract_,
        state,
        comment,
    } = fields;
    let all = [
        ("name", name),
        ("title", title),
        ("subtitle", subtitle),
        ("time", time),
        ("date", date),
        ("authors", authors),
        ("keywords", keywords),
        ("abstract", abstract_),
        ("state", state),
        ("comment", comment),
    ];
    for (field, value) in all {
        let _ = write!(line, " {}=", field);
        match value {
            None => line.push_str("null"),
            Some(Value::Scalar(scalar)) => {
                line.push_str("scalar(");
                match scalar {
                    Scalar::Null => line.push_str("null"),
                    Scalar::Bool(value) => {
                        let _ = write!(line, "bool({})", value);
                    }
                    Scalar::Number(value) => {
                        line.push_str("number(");
                        string(line, value);
                        line.push(')');
                    }
                    Scalar::Text(value) => {
                        line.push_str("text(");
                        string(line, value);
                        line.push(')');
                    }
                }
                line.push(')');
            }
            Some(Value::List(items)) => {
                line.push_str("list([");
                for (index, item) in items.iter().enumerate() {
                    if index > 0 {
                        line.push(',');
                    }
                    match item {
                        Item::Number(value) => {
                            line.push_str("number(");
                            string(line, value);
                        }
                        Item::Text(value) => {
                            line.push_str("text(");
                            string(line, value);
                        }
                    }
                    line.push(')');
                }
                line.push_str("])");
            }
        }
    }
}

fn destination(line: &mut String, dest: &Destination) {
    match dest {
        Destination::Url(url) => {
            line.push_str("url(");
            string(line, url);
            line.push(')');
        }
        Destination::Cross { path, anchor } => {
            line.push_str("cross(path=");
            string(line, path);
            line.push_str(",anchor=");
            opt_text(line, anchor.as_deref());
            line.push(')');
        }
        Destination::Reference(label) => {
            line.push_str("reference(");
            string(line, label);
            line.push(')');
        }
    }
}

fn option_dimensions(line: &mut String, dimensions: Option<Dimensions>) {
    match dimensions {
        None => line.push_str("null"),
        Some(Dimensions { width, height }) => {
            let _ = write!(line, "(width={},height=", width);
            match height {
                Some(height) => {
                    let _ = write!(line, "{}", height);
                }
                None => line.push_str("null"),
            }
            line.push(')');
        }
    }
}

fn option_bool(line: &mut String, value: Option<bool>) {
    match value {
        None => line.push_str("null"),
        Some(value) => {
            let _ = write!(line, "{}", value);
        }
    }
}

fn opt_text(line: &mut String, value: Option<&str>) {
    match value {
        None => line.push_str("null"),
        Some(value) => string(line, value),
    }
}

/// A string in the dump's canonical JSON form.
fn string(line: &mut String, value: &str) {
    line.push('"');
    for char in value.chars() {
        match char {
            '"' => line.push_str("\\\""),
            '\\' => line.push_str("\\\\"),
            '\u{8}' => line.push_str("\\b"),
            '\t' => line.push_str("\\t"),
            '\n' => line.push_str("\\n"),
            '\u{c}' => line.push_str("\\f"),
            '\r' => line.push_str("\\r"),
            char if (char as u32) < 0x20 => {
                let _ = write!(line, "\\u{:04x}", char as u32);
            }
            char => line.push(char),
        }
    }
    line.push('"');
}

fn attributes(line: &mut String, attributes: &Attributes) {
    line.push('{');
    let mut first = true;
    for class in &attributes.classes {
        if !first {
            line.push(' ');
        }
        first = false;
        line.push('.');
        let bare = !class.is_empty()
            && class.bytes().all(|byte| {
                (0x21..=0x7e).contains(&byte)
                    && !matches!(
                        byte,
                        b'"' | b'\\' | b'{' | b'}' | b'[' | b']' | b'(' | b')' | b'='
                    )
            });
        if bare {
            line.push_str(class);
        } else {
            string(line, class);
        }
    }
    for record in &attributes.records {
        if !first {
            line.push(' ');
        }
        first = false;
        line.push_str(&record.name);
        line.push('=');
        string(line, &record.value);
    }
    line.push('}');
}
