//! The YAML reader of the properties envelope.
//!
//! It reads one language, YAML as the properties grammar takes it: block and
//! flow collections of any depth, names that are scalars or flow
//! collections on one line, plain and quoted scalars on one line, a
//! double-quoted scalar’s JSON escapes, a bare `|` literal, comments, and
//! the document markers.
//! A folded scalar, a scalar that goes on to another line, an explicit
//! name, an anchor, an alias, a tag and a directive are errors.
//! A NUL reads as U+FFFD, and a line ends at CR, LF or CRLF only.
//!
//! One read takes a text in one pass by recursive descent: each block
//! collection is a call that knows its column, each flow collection a call
//! that knows its depth, and a node that a `:` follows on its line becomes
//! a name once it is read, its mapping’s start written before it.
//! The readings are libyaml’s as the properties language takes them: a name
//! may start only where a block item may, or after a flow collection’s `[`,
//! `{` or `,`; a name at a block collection’s column must be one; an item or
//! a name deeper than its collection opens a nested one, and a token
//! shallower closes it; a value left out reads as an empty plain scalar.

use crate::util::stack::room;
use alloc::{vec, vec::Vec};

/// The deepest nesting of collections, block and flow together.
const DEPTH_MAX: usize = 1000;
/// The most characters a simple name and the spaces after it may hold.
const NAME_LIMIT: usize = 1024;

/// How a scalar is written.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Form {
    Plain,
    /// Between apostrophes.
    Apostrophe,
    /// Between double quotes.
    Quote,
    Literal,
}

/// What an event is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    StreamStart,
    StreamEnd,
    DocumentStart,
    DocumentEnd,
    Scalar,
    ListStart,
    ListEnd,
    MapStart,
    MapEnd,
}

/// An event.
///
/// A collection’s start says whether it is written in flow, between
/// brackets or braces; a document’s start or end whether a marker writes
/// it.
/// A scalar holds its text; `from` to `to` are its bytes as written,
/// without the quotes around them, and for a literal from its first line’s
/// text to its last line’s end; `line` is the line it starts on, counted
/// from 0 at the text’s start.
#[derive(Clone, Debug)]
pub struct Event {
    pub kind: Kind,
    pub flow: bool,
    pub explicit: bool,
    pub form: Form,
    pub text: Vec<u8>,
    pub from: usize,
    pub to: usize,
    pub line: usize,
}

impl Event {
    fn new(kind: Kind, line: usize) -> Event {
        Event {
            kind,
            flow: false,
            explicit: false,
            form: Form::Plain,
            text: vec![],
            from: 0,
            to: 0,
            line,
        }
    }
}

/// The token at `at`, which a pass has reached: what starts there.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Token {
    End,
    Bad,
    DocumentStart,
    DocumentEnd,
    ListOpen,
    MapOpen,
    ListClose,
    MapClose,
    Comma,
    /// A block item’s `-`.
    Item,
    /// A name’s `:`.
    Value,
    Literal,
    Quoted,
    Plain,
}

/// What a block node’s read found: the node; a name at the column of the
/// collection that holds it, so that the node is left out (`Name`); or the
/// node, and after it a name at that column (`After`).
/// Either name is read, its events from `index` on, and `at` is at its `:`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Found {
    Failed,
    Node,
    Name,
    After,
}

/// The reader’s state.
struct Reader<'a> {
    text: &'a [u8],
    at: usize,
    end: usize,
    /// Where the line `at` is on starts, and its number.
    head: usize,
    line: usize,
    /// Whether a block item or a name may start at `at`.
    allowed: bool,
    /// The flow depth, and the block collections open.
    depth: usize,
    open: usize,
    /// Whether the text is no YAML of the language.
    problem: bool,
    events: Vec<Event>,
}

/// Read the text from `start` to `end`, which starts a line: its events, or
/// `None` when it is no YAML of the language.
pub fn read(text: &[u8], start: usize, end: usize) -> Option<Vec<Event>> {
    if !printable(&text[start..end]) {
        return None;
    }
    let mut reader = Reader {
        text,
        at: start,
        end,
        head: start,
        line: 0,
        allowed: true,
        depth: 0,
        open: 0,
        problem: false,
        events: vec![],
    };
    if reader.stream() {
        Some(reader.events)
    } else {
        None
    }
}

fn blank(c: Option<u8>) -> bool {
    matches!(c, Some(b' ' | b'\t'))
}
fn newline(c: Option<u8>) -> bool {
    matches!(c, Some(b'\r' | b'\n'))
}
fn blankz(c: Option<u8>) -> bool {
    c.is_none() || blank(c) || newline(c)
}
fn breakz(c: Option<u8>) -> bool {
    c.is_none() || newline(c)
}
/// A flow collection’s punctuation.
fn punctuation(c: Option<u8>) -> bool {
    matches!(c, Some(b',' | b'[' | b']' | b'{' | b'}'))
}
/// A character that starts a token other than plain text.
fn indicator(c: Option<u8>) -> bool {
    c.is_some_and(|c| b"-?:,[]{}#&*!|>'\"%@`".contains(&c))
}
fn hex(c: Option<u8>) -> Option<u32> {
    match c? {
        c @ b'0'..=b'9' => Some(u32::from(c - b'0')),
        c @ b'a'..=b'f' => Some(u32::from(c - b'a') + 10),
        c @ b'A'..=b'F' => Some(u32::from(c - b'A') + 10),
        _ => None,
    }
}
fn is(c: Option<u8>, byte: u8) -> bool {
    c == Some(byte)
}

/// Whether each character of `text` is one YAML prints: a tab, a line
/// ending, or a printable scalar, with a NUL read as U+FFFD.
fn printable(text: &[u8]) -> bool {
    match core::str::from_utf8(text) {
        Ok(text) => text.chars().all(|c| {
            matches!(c, '\0' | '\t' | '\n' | '\r' | ' '..='~' | '\u{85}')
                || ('\u{a0}'..='\u{d7ff}').contains(&c)
                || ('\u{e000}'..='\u{fffd}').contains(&c)
                || c >= '\u{10000}'
        }),
        Err(_) => false,
    }
}

impl Reader<'_> {
    /// The byte `k` past `at`, or `None` past the end.
    fn peek(&self, k: usize) -> Option<u8> {
        let at = self.at + k;
        if at < self.end {
            Some(self.text[at])
        } else {
            None
        }
    }

    /// The column of `at`: the spaces, dashes and indicators before it on
    /// its line are ASCII, so bytes count characters.
    fn column(&self) -> usize {
        self.at - self.head
    }

    fn marker(&self, c: u8) -> bool {
        self.at == self.head
            && is(self.peek(0), c)
            && is(self.peek(1), c)
            && is(self.peek(2), c)
            && blankz(self.peek(3))
    }

    fn feed(&mut self) {
        self.at += if is(self.peek(0), b'\r') && is(self.peek(1), b'\n') {
            2
        } else {
            1
        };
        self.line += 1;
        self.head = self.at;
    }

    fn fail(&mut self) -> bool {
        self.problem = true;
        false
    }

    /// Insert an event at `index`.
    fn insert(&mut self, index: usize, kind: Kind) -> &mut Event {
        self.events.insert(index, Event::new(kind, self.line));
        &mut self.events[index]
    }

    fn emit(&mut self, kind: Kind) -> &mut Event {
        let index = self.events.len();
        self.insert(index, kind)
    }

    /// An empty plain scalar where a node is left out, at `at` on `line`.
    fn absent(&mut self, index: usize, at: usize, line: usize) {
        let event = self.insert(index, Kind::Scalar);
        event.from = at;
        event.to = at;
        event.line = line;
    }

    /// Open a collection, at most `DEPTH_MAX` deep.
    fn open(&mut self, kind: Kind, index: usize, flow: bool) -> bool {
        if self.open + self.depth >= DEPTH_MAX {
            return self.fail();
        }
        self.insert(index, kind).flow = flow;
        if flow {
            self.depth += 1;
        } else {
            self.open += 1;
        }
        true
    }

    fn shut(&mut self, kind: Kind, flow: bool) -> bool {
        self.emit(kind).flow = flow;
        true
    }

    /// Append the text from `from` to `to`, each NUL as U+FFFD.
    fn put(&self, text: &mut Vec<u8>, from: usize, to: usize) {
        for &byte in &self.text[from..to] {
            if byte == 0 {
                text.extend_from_slice("\u{fffd}".as_bytes());
            } else {
                text.push(byte);
            }
        }
    }

    fn scalar(&mut self, form: Form, from: usize, to: usize, line: usize, text: Vec<u8>) {
        let event = self.emit(Kind::Scalar);
        event.form = form;
        event.from = from;
        event.to = to;
        event.line = line;
        event.text = text;
    }

    /// Skip to the end of the line a comment starts on.
    fn comment(&mut self) {
        while !breakz(self.peek(0)) {
            self.at += 1;
        }
    }

    /// Whether only blanks and a comment follow `at` on its line.
    fn idle(&self) -> bool {
        let mut k = 0;
        while blank(self.peek(k)) {
            k += 1;
        }
        breakz(self.peek(k)) || is(self.peek(k), b'#')
    }

    /// Skip spaces, comments and line breaks to the next token.
    /// A `#` starts a comment where a blank or the line’s start comes before
    /// it.
    /// In a block collection a tab may only follow its line’s first token,
    /// or lead to a comment or the line’s end.
    fn pass(&mut self) {
        loop {
            while is(self.peek(0), b' ')
                || (is(self.peek(0), b'\t') && (self.depth > 0 || !self.allowed))
            {
                self.at += 1;
            }
            // A tab that leads to a comment or the line’s end: so does each
            // blank after it.
            if is(self.peek(0), b'\t') && self.idle() {
                while blank(self.peek(0)) {
                    self.at += 1;
                }
            }
            if is(self.peek(0), b'#')
                && (self.at == self.head || blank(Some(self.text[self.at - 1])))
            {
                self.comment();
            }
            if !newline(self.peek(0)) {
                return;
            }
            self.feed();
            if self.depth == 0 {
                self.allowed = true;
            }
        }
    }

    fn look(&mut self) -> Token {
        let c = self.peek(0);
        if c.is_none() {
            return Token::End;
        }
        if self.at == self.head {
            if is(c, b'%') {
                self.fail();
                return Token::Bad;
            }
            if is(c, b'-') && self.marker(b'-') {
                return Token::DocumentStart;
            }
            if is(c, b'.') && self.marker(b'.') {
                return Token::DocumentEnd;
            }
        }
        match c {
            Some(b'[') => return Token::ListOpen,
            Some(b'{') => return Token::MapOpen,
            Some(b']') => return Token::ListClose,
            Some(b'}') => return Token::MapClose,
            Some(b',') => return Token::Comma,
            // Aliases, anchors and tags are unsupported.
            Some(b'*' | b'&' | b'!') => {
                self.fail();
                return Token::Bad;
            }
            _ => {}
        }
        let next = self.peek(1);
        let flow = self.depth > 0;
        // A `- ` where no block item may begin, as after a name’s `:` on its
        // line, is plain text: `authors: - Ada` is the text "- Ada".
        if is(c, b'-') && blankz(next) && (flow || self.allowed) {
            return Token::Item;
        }
        // Explicit names are unsupported.
        if is(c, b'?') && (flow || blankz(next)) {
            self.fail();
            return Token::Bad;
        }
        if is(c, b':') && (flow || blankz(next)) {
            return Token::Value;
        }
        if is(c, b'|') && !flow {
            return Token::Literal;
        }
        // Folded scalars are unsupported.
        if is(c, b'>') && !flow {
            self.fail();
            return Token::Bad;
        }
        if is(c, b'\'') || is(c, b'"') {
            return Token::Quoted;
        }
        // A dash left here starts plain text: one no blank follows, or one
        // where no block item may begin.
        if !(blankz(c) || indicator(c))
            || is(c, b'-')
            || (!flow && (is(c, b'?') || is(c, b':')) && !blankz(next))
        {
            return Token::Plain;
        }
        self.fail();
        Token::Bad
    }

    /// Whether a token ends what holds a block node: the text’s end, a
    /// document marker, or a token left of `inset`.
    fn closes(&self, token: Token, inset: usize) -> bool {
        matches!(
            token,
            Token::End | Token::DocumentStart | Token::DocumentEnd
        ) || self.column() < inset
    }

    /// A scalar between apostrophes or double quotes, on one line.
    fn quoted(&mut self) -> bool {
        let line = self.line;
        let apostrophe = is(self.peek(0), b'\'');
        let quote = if apostrophe { b'\'' } else { b'"' };
        let mut text = vec![];
        self.at += 1;
        let from = self.at;
        loop {
            let c = self.peek(0);
            if c.is_none() || newline(c) {
                return self.fail();
            }
            if is(c, quote) && !(apostrophe && is(self.peek(1), b'\'')) {
                break;
            }
            if apostrophe && is(c, b'\'') {
                text.push(b'\'');
                self.at += 2;
            } else if !apostrophe && is(c, b'\\') {
                // JSON’s escapes, with a pair of halves written as two.
                let next = self.peek(1);
                let escape = match next {
                    Some(byte) if byte != 0 => {
                        b"btnfr\"/\\".iter().position(|known| *known == byte)
                    }
                    _ => None,
                };
                if let Some(index) = escape {
                    text.push(b"\x08\t\n\x0c\r\"/\\"[index]);
                    self.at += 2;
                    continue;
                }
                let mut code = 0;
                for half in 0..2 {
                    if !is(self.peek(0), b'\\') || !is(self.peek(1), b'u') {
                        return self.fail();
                    }
                    let mut unit = 0;
                    for k in 2..6 {
                        match hex(self.peek(k)) {
                            Some(nibble) => unit = (unit << 4) | nibble,
                            None => return self.fail(),
                        }
                    }
                    self.at += 6;
                    if half == 1 {
                        if !(0xDC00..=0xDFFF).contains(&unit) {
                            return self.fail();
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (unit - 0xDC00);
                        break;
                    }
                    code = unit;
                    if (0xDC00..=0xDFFF).contains(&unit) {
                        return self.fail();
                    }
                    if !(0xD800..=0xDBFF).contains(&unit) {
                        break;
                    }
                }
                let mut buffer = [0; 4];
                let character = char::from_u32(code).unwrap_or('\u{fffd}');
                text.extend_from_slice(character.encode_utf8(&mut buffer).as_bytes());
            } else {
                let at = self.at;
                self.at += 1;
                self.put(&mut text, at, self.at);
            }
        }
        let to = self.at;
        self.at += 1;
        self.allowed = false;
        let form = if apostrophe {
            Form::Apostrophe
        } else {
            Form::Quote
        };
        self.scalar(form, from, to, line, text);
        true
    }

    /// A plain scalar, which its line ends: a scalar that would go on to the
    /// next line is no scalar of the language, and the next line reads as
    /// the tokens it holds.
    /// It reads the blanks after it, as libyaml does.
    fn plain(&mut self) -> bool {
        let from = self.at;
        let mut to = from;
        let flow = self.depth > 0;
        while !is(self.peek(0), b'#') {
            loop {
                let c = self.peek(0);
                if blankz(c) {
                    break;
                }
                if flow && is(c, b':') && (punctuation(self.peek(1)) || is(self.peek(1), b'?')) {
                    return self.fail();
                }
                if (is(c, b':') && blankz(self.peek(1))) || (flow && punctuation(c)) {
                    break;
                }
                self.at += 1;
                to = self.at;
            }
            if !blank(self.peek(0)) {
                break;
            }
            while blank(self.peek(0)) {
                self.at += 1;
            }
        }
        self.allowed = false;
        let mut text = vec![];
        self.put(&mut text, from, to);
        let line = self.line;
        self.scalar(Form::Plain, from, to, line, text);
        true
    }

    /// A literal: a bare `|`, then the lines its first nonblank line’s
    /// indentation holds, which must be deeper than the block collection’s
    /// column `indent` and at least one space deep.
    /// Each line keeps what follows that indentation; a blank line is an
    /// empty one; the text ends with one line ending after its last
    /// nonblank line.
    /// A nonblank line indented less ends it.
    fn literal(&mut self, indent: Option<usize>) -> bool {
        let start = self.at;
        let line = self.line;
        self.at += 1;
        if !blankz(self.peek(0)) {
            return self.fail();
        }
        while blank(self.peek(0)) {
            self.at += 1;
        }
        if is(self.peek(0), b'#') {
            self.comment();
        }
        if !breakz(self.peek(0)) {
            return self.fail();
        }
        if newline(self.peek(0)) {
            self.feed();
        }
        let mut text = vec![];
        let mut depth = 0;
        let mut clipped = 0;
        let mut from = start;
        let mut to = start;
        let mut first = true;
        while self.peek(0).is_some() {
            let mut spaces = 0;
            while is(self.peek(spaces), b' ') {
                spaces += 1;
            }
            let mut content = spaces;
            while blank(self.peek(content)) {
                content += 1;
            }
            if breakz(self.peek(content)) {
                self.at += content;
                text.push(b'\n');
                if newline(self.peek(0)) {
                    self.feed();
                }
                continue;
            }
            if depth == 0 {
                if indent.is_some_and(|indent| spaces <= indent) || spaces == 0 {
                    break;
                }
                depth = spaces;
            }
            if spaces < depth {
                break;
            }
            self.at += depth;
            if first {
                from = self.at;
                first = false;
            }
            let at = self.at;
            while !breakz(self.peek(0)) {
                self.at += 1;
            }
            self.put(&mut text, at, self.at);
            to = self.at;
            text.push(b'\n');
            clipped = text.len();
            if newline(self.peek(0)) {
                self.feed();
            }
        }
        text.truncate(clipped);
        self.allowed = true;
        self.scalar(Form::Literal, from, to, line, text);
        true
    }

    /// A node of a flow collection: a scalar or a flow collection.
    fn element(&mut self, token: Token) -> bool {
        match token {
            Token::Quoted => self.quoted(),
            Token::Plain => self.plain(),
            Token::ListOpen | Token::MapOpen => self.flow(),
            Token::Bad => false,
            _ => self.fail(),
        }
    }

    /// Whether the node read from `start`, on line `line`, is a name: a `:`
    /// follows it on that line, within `NAME_LIMIT` characters of its start.
    /// Blanks before the `:` are read.
    fn named(&mut self, start: usize, line: usize) -> bool {
        while blank(self.peek(0)) {
            self.at += 1;
        }
        if self.line != line || !is(self.peek(0), b':') || !(self.depth > 0 || blankz(self.peek(1)))
        {
            return false;
        }
        // No more characters than bytes.
        if self.at - start > NAME_LIMIT {
            let characters = self.text[start..self.at]
                .iter()
                .filter(|byte| (**byte & 0xC0) != 0x80)
                .count();
            if characters > NAME_LIMIT {
                return self.fail();
            }
        }
        true
    }

    /// A flow collection, from its `[` or `{`.
    /// A node a `:` follows is a name: in a list it opens a mapping of one
    /// pair; in a mapping a node no `:` follows is a name whose value is
    /// left out.
    fn flow(&mut self) -> bool {
        room(|| self.flow_node())
    }

    /// A flow node, as [`Self::flow`][], on the stack [`room`][] made.
    fn flow_node(&mut self) -> bool {
        let map = is(self.peek(0), b'{');
        let close = if map {
            Token::MapClose
        } else {
            Token::ListClose
        };
        let count = self.events.len();
        if !self.open(
            if map { Kind::MapStart } else { Kind::ListStart },
            count,
            true,
        ) {
            return false;
        }
        self.at += 1;
        self.allowed = true;
        let mut first = true;
        loop {
            self.pass();
            let mut token = self.look();
            if token != close && !first {
                if token != Token::Comma {
                    return token != Token::Bad && self.fail();
                }
                self.at += 1;
                self.allowed = true;
                self.pass();
                token = self.look();
            }
            first = false;
            if token == close {
                break;
            }
            let index = self.events.len();
            let start = self.at;
            let line = self.line;
            if !self.element(token) {
                return false;
            }
            if self.named(start, line) {
                // A list’s pair is a mapping of the reader’s, not the text’s
                // flow depth.
                if !map {
                    self.insert(index, Kind::MapStart).flow = true;
                }
                self.at += 1;
                self.allowed = false;
                self.pass();
                let token = self.look();
                if token == Token::Comma || token == close {
                    let count = self.events.len();
                    self.absent(count, self.at, self.line);
                } else if !self.element(token) {
                    return false;
                }
                if !map {
                    self.shut(Kind::MapEnd, true);
                }
            } else if self.problem {
                return false;
            } else if map {
                self.pass();
                let count = self.events.len();
                self.absent(count, self.at, self.line);
            }
        }
        self.at += 1;
        self.depth -= 1;
        self.allowed = false;
        self.shut(if map { Kind::MapEnd } else { Kind::ListEnd }, true)
    }

    /// A block mapping at `inset`, whose first name is read and whose `:` is
    /// at `at`.
    /// Each name is at that column; a token left of it ends the mapping.
    fn pairs(&mut self, inset: usize) -> bool {
        loop {
            let value = self.at + 1;
            let line = self.line;
            self.at += 1;
            self.allowed = false;
            self.pass();
            let token = self.look();
            if token == Token::Bad {
                return false;
            }
            if self.closes(token, inset) {
                let count = self.events.len();
                self.absent(count, value, line);
                break;
            }
            let mut index = 0;
            let found = self.block(Some(inset), true, &mut index);
            match found {
                Found::Failed => return false,
                Found::Name => {
                    self.absent(index, value, line);
                    continue;
                }
                Found::After => continue,
                Found::Node => {}
            }
            self.pass();
            let token = self.look();
            if token == Token::Bad {
                return false;
            }
            if self.closes(token, inset) {
                break;
            }
            if self.column() > inset
                || !matches!(
                    token,
                    Token::Quoted | Token::Plain | Token::ListOpen | Token::MapOpen
                )
            {
                return self.fail();
            }
            let start = self.at;
            let at = self.line;
            if !self.node(token) {
                return false;
            }
            if !self.named(start, at) {
                return self.fail();
            }
        }
        self.open -= 1;
        self.shut(Kind::MapEnd, false)
    }

    /// A scalar or a flow collection.
    fn node(&mut self, token: Token) -> bool {
        match token {
            Token::Quoted => self.quoted(),
            Token::Plain => self.plain(),
            _ => self.flow(),
        }
    }

    /// A block list whose items’ `-` is at `inset`, from its first.
    /// An indentless list is a mapping’s value whose items are at the
    /// mapping’s column: it opens no block collection, and a name at that
    /// column ends it (`After`).
    fn items(&mut self, inset: usize, indentless: bool, index: &mut usize) -> Found {
        if indentless {
            self.emit(Kind::ListStart);
        } else {
            let count = self.events.len();
            if !self.open(Kind::ListStart, count, false) {
                return Found::Failed;
            }
        }
        loop {
            let item = self.at + 1;
            let line = self.line;
            self.at += 1;
            self.allowed = true;
            self.pass();
            let token = self.look();
            if token == Token::Bad {
                return Found::Failed;
            }
            if self.closes(token, inset) {
                let count = self.events.len();
                self.absent(count, item, line);
                break;
            }
            if self.column() == inset && token == Token::Item {
                let count = self.events.len();
                self.absent(count, item, line);
                continue;
            }
            let found = self.block(Some(inset), false, index);
            if found == Found::Failed {
                return Found::Failed;
            }
            if found != Found::Node {
                if !indentless {
                    self.fail();
                    return Found::Failed;
                }
                // The item is left out and the list ends before the name.
                self.absent(*index, item, line);
                self.insert(*index + 1, Kind::ListEnd);
                *index += 2;
                return Found::After;
            }
            self.pass();
            let token = self.look();
            if token == Token::Bad {
                return Found::Failed;
            }
            if token == Token::Item && self.column() == inset {
                continue;
            }
            if indentless || self.closes(token, inset) {
                break;
            }
            self.fail();
            return Found::Failed;
        }
        if !indentless {
            self.open -= 1;
        }
        self.shut(Kind::ListEnd, false);
        Found::Node
    }

    /// A block node at `at`, a token a pass reached, inside a block
    /// collection at column `indent` (`None` for a document’s root).
    fn block(&mut self, indent: Option<usize>, indentless: bool, index: &mut usize) -> Found {
        room(|| self.block_node(indent, indentless, index))
    }

    /// A block node, as [`Self::block`][], on the stack [`room`][] made.
    fn block_node(&mut self, indent: Option<usize>, indentless: bool, index: &mut usize) -> Found {
        let token = self.look();
        let at = self.column();
        *index = self.events.len();
        match token {
            Token::Item => {
                if indent.is_none_or(|indent| at > indent) {
                    self.items(at, false, index)
                } else if indentless {
                    self.items(at, true, index)
                } else {
                    self.fail();
                    Found::Failed
                }
            }
            Token::Literal => {
                if self.literal(indent) {
                    Found::Node
                } else {
                    Found::Failed
                }
            }
            Token::Quoted | Token::Plain | Token::ListOpen | Token::MapOpen => {
                let candidate = self.allowed;
                let required = candidate && indent == Some(at);
                let start = self.at;
                let line = self.line;
                if !self.node(token) {
                    return Found::Failed;
                }
                if candidate && self.named(start, line) {
                    if indent == Some(at) {
                        return Found::Name;
                    }
                    return if self.open(Kind::MapStart, *index, false) && self.pairs(at) {
                        Found::Node
                    } else {
                        Found::Failed
                    };
                }
                if self.problem || required {
                    self.fail();
                    return Found::Failed;
                }
                Found::Node
            }
            Token::Bad => Found::Failed,
            _ => {
                self.fail();
                Found::Failed
            }
        }
    }

    /// The stream: documents, each implicit or opened by `---`, closed by
    /// `...` or by the next document or the stream’s end.
    fn stream(&mut self) -> bool {
        self.emit(Kind::StreamStart);
        self.pass();
        let mut token = self.look();
        let mut implicit = true;
        loop {
            if token == Token::Bad || self.problem {
                return false;
            }
            if token == Token::End {
                break;
            }
            if !implicit && token != Token::DocumentStart {
                return self.fail();
            }
            let start = self.events.len();
            self.emit(Kind::DocumentStart);
            let mut index = 0;
            if token == Token::DocumentStart {
                self.events[start].explicit = true;
                self.at += 3;
                self.allowed = false;
                self.pass();
                token = self.look();
                if matches!(
                    token,
                    Token::DocumentStart | Token::DocumentEnd | Token::End
                ) {
                    // The stream’s end is on a line of its own, as libyaml
                    // marks it.
                    let line = self.line + usize::from(token == Token::End && self.column() != 0);
                    let count = self.events.len();
                    self.absent(count, self.at, line);
                } else if self.block(None, false, &mut index) == Found::Failed {
                    return false;
                }
            } else if self.block(None, false, &mut index) == Found::Failed {
                return false;
            }
            self.pass();
            token = self.look();
            let end = self.events.len();
            self.emit(Kind::DocumentEnd);
            while token == Token::DocumentEnd {
                self.events[end].explicit = true;
                self.at += 3;
                self.allowed = false;
                self.pass();
                token = self.look();
            }
            implicit = false;
        }
        self.emit(Kind::StreamEnd);
        !self.problem
    }
}
