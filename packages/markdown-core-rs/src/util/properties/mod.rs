//! The properties envelope: its payload, decoded into metadata fields.
//!
//! The payload is decoded member by member, in order.
//! A member is a line that holds a name at its start, with the lines it
//! owns: its indented continuation, its block list, and a bracketed or
//! quoted value left open (`member_end`).
//! The YAML [`reader`] reads each member on its own, so an error ends the
//! member, not the payload: the next member begins where the lines say, and
//! its valid neighbours are kept.
//! A member the reader reads as one name and one value of the properties
//! grammar sets its field, the first time only; any other member is skipped
//! as a whole.
//! See `docs/specs/dialect/properties.md`.

pub mod reader;

use crate::ast::{Fields, Item, Scalar, Value};
use alloc::{string::String, vec::Vec};
use reader::{Event, Form, Kind};

/// The field names the envelope reads, in the order of the dialect spec.
const NAMES: [&str; 10] = [
    "name", "title", "subtitle", "time", "date", "authors", "keywords", "abstract", "state",
    "comment",
];

/// The field of `fields` that `name` names.
fn field<'a>(fields: &'a mut Fields, name: &[u8]) -> Option<&'a mut Option<Value>> {
    let index = NAMES.iter().position(|known| known.as_bytes() == name)?;
    Some(match index {
        0 => &mut fields.name,
        1 => &mut fields.title,
        2 => &mut fields.subtitle,
        3 => &mut fields.time,
        4 => &mut fields.date,
        5 => &mut fields.authors,
        6 => &mut fields.keywords,
        7 => &mut fields.abstract_,
        8 => &mut fields.state,
        _ => &mut fields.comment,
    })
}

fn space_or_tab(byte: u8) -> bool {
    matches!(byte, b'\t' | b' ')
}

fn whitespace(byte: u8) -> bool {
    matches!(byte, b'\t' | b'\n' | b'\x0b' | b'\x0c' | b'\r' | b' ')
}

/// Decode the payload of the envelope in `text`, from `start` to `end`,
/// where its closing fence’s line starts.
pub fn decode(text: &[u8], start: usize, end: usize) -> Fields {
    let mut envelope = Envelope {
        text,
        end,
        fields: Fields::default(),
        line: 0,
        first: 0,
        colon: 0,
        stop: 0,
        noted: false,
    };
    envelope.read(start);
    envelope.fields
}

/// Whether a plain scalar may start at `start`.
fn plain_start(text: &[u8], start: usize, end: usize) -> bool {
    if start == end || space_or_tab(text[start]) || b",[]{}#&*!|>'\"%@`".contains(&text[start]) {
        return false;
    }
    !((text[start] == b'?' || text[start] == b':')
        && (start + 1 == end || whitespace(text[start + 1])))
}

/// Where the line after the line ending at `at` starts.
fn next_line(text: &[u8], mut at: usize, end: usize) -> usize {
    if at < end && text[at] == b'\r' {
        at += 1;
    }
    if at < end && text[at] == b'\n' {
        at += 1;
    }
    at
}

/// Whether `text` spells a number.
fn number(text: &[u8]) -> bool {
    let size = text.len();
    let mut index = 0;
    if index < size && text[index] == b'-' {
        index += 1;
    }
    if index == size {
        return false;
    }
    if text[index] == b'0' {
        index += 1;
    } else {
        if !(b'1'..=b'9').contains(&text[index]) {
            return false;
        }
        while index < size && text[index].is_ascii_digit() {
            index += 1;
        }
    }
    if index < size && text[index] == b'.' {
        index += 1;
        while index < size && text[index].is_ascii_digit() {
            index += 1;
        }
    }
    if index < size && (text[index] == b'e' || text[index] == b'E') {
        index += 1;
        if index < size && (text[index] == b'+' || text[index] == b'-') {
            index += 1;
        }
        let exponent = index;
        while index < size && text[index].is_ascii_digit() {
            index += 1;
        }
        if exponent == index {
            return false;
        }
    }
    index == size
}

/// A scalar’s value: quoted text is text; plain text is null when empty or
/// `null`, a boolean when `true` or `false`, a number when it spells one,
/// and text otherwise.
/// A value on one line holds no decoded line ending.
fn scalar(event: &Event) -> Option<Scalar> {
    let text = &event.text[..];
    if event.form != Form::Literal && text.iter().any(|byte| matches!(byte, b'\n' | b'\r')) {
        return None;
    }
    let string = || String::from_utf8_lossy(text).into_owned();
    Some(if event.form != Form::Plain {
        Scalar::Text(string())
    } else if text.is_empty() || text == b"null" {
        Scalar::Null
    } else if text == b"true" || text == b"false" {
        Scalar::Bool(text == b"true")
    } else if number(text) {
        Scalar::Number(string())
    } else {
        Scalar::Text(string())
    })
}

/// Where a directly authored block name’s colon ends, or 0.
/// The same lexical rule finds where members resume.
/// Flow punctuation is ordinary content inside a block plain name; only a
/// separated `#` is a comment.
fn name_end(text: &[u8], mut start: usize, end: usize) -> usize {
    if start == end {
        return 0;
    }
    if text[start] == b'\'' || text[start] == b'"' {
        let quote = text[start];
        start += 1;
        let mut closed = false;
        while start < end {
            let byte = text[start];
            start += 1;
            if quote == b'"' && byte == b'\\' && start < end {
                start += 1;
            } else if byte == quote {
                if quote == b'\'' && start < end && text[start] == b'\'' {
                    start += 1;
                } else {
                    closed = true;
                    break;
                }
            }
        }
        while start < end && space_or_tab(text[start]) {
            start += 1;
        }
        return if closed
            && start < end
            && text[start] == b':'
            && (start + 1 == end || space_or_tab(text[start + 1]))
        {
            start + 1
        } else {
            0
        };
    }
    if !plain_start(text, start, end) {
        return 0;
    }
    for index in start..end {
        if text[index] == b':' && (index + 1 == end || space_or_tab(text[index + 1])) {
            return index + 1;
        }
        if text[index] == b'#' && (index == start || space_or_tab(text[index - 1])) {
            return 0;
        }
    }
    0
}

/// The form of a member’s value, as far as its lines tell.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Shape {
    Prefix,
    Scalar,
    Quoted,
    Items,
    Bracketed,
}

/// The envelope’s state.
struct Envelope<'a> {
    text: &'a [u8],
    /// Where the envelope ends: the closing fence’s line.
    end: usize,
    fields: Fields,
    /// One line of lookahead: `line` starts it, `first` is its first byte
    /// that is no space, `colon` the byte after a name’s colon, or 0, and
    /// `stop` where its line ending starts.
    line: usize,
    first: usize,
    colon: usize,
    stop: usize,
    noted: bool,
}

impl Envelope<'_> {
    /// Note the facts of the line at `line`.
    fn note_line(&mut self, line: usize) {
        if self.noted && self.line == line {
            return;
        }
        let text = self.text;
        let stop = text[line..self.end]
            .iter()
            .position(|byte| matches!(byte, b'\n' | b'\r'))
            .map_or(self.end, |offset| line + offset);
        self.noted = true;
        self.line = line;
        self.stop = stop;
        let mut first = line;
        while first < stop && text[first] == b' ' {
            first += 1;
        }
        self.first = first;
        self.colon = name_end(text, first, stop);
    }

    /// Where the first line after `line` that the member starting there does
    /// not own starts, or the envelope’s end when it runs to the closing
    /// fence.
    fn member_end(&mut self, mut line: usize, indent: usize) -> usize {
        let text = self.text;
        let mut shape = Shape::Prefix;
        let mut list_depth = 0_usize;
        let mut map_depth = 0_usize;
        let mut quote = 0;
        let mut first = true;
        let mut node = true;
        while line < self.end {
            self.note_line(line);
            let cursor = line;
            let stop = self.stop;
            let nonspace = self.first;
            if !first && nonspace < stop && nonspace - cursor <= indent {
                let list_line = nonspace - cursor == indent
                    && text[nonspace] == b'-'
                    && (nonspace + 1 == stop || space_or_tab(text[nonspace + 1]));
                let resumes = self.colon != 0;
                let mut content = nonspace;
                while content < stop && space_or_tab(text[content]) {
                    content += 1;
                }
                let separation = content == stop || text[content] == b'#';
                let continuation = content == stop
                    || ((shape == Shape::Prefix || shape == Shape::Items)
                        && (list_line || separation))
                    || list_depth != 0
                    || map_depth != 0
                    || (quote != 0 && !resumes);
                if !continuation {
                    return line;
                }
            }
            let token = if first { self.colon } else { 0 };
            let mut index = if token == 0 { nonspace } else { token };
            while index < stop {
                let byte = text[index];
                if shape == Shape::Prefix {
                    if space_or_tab(byte) {
                        index += 1;
                        continue;
                    }
                    if byte == b'#' {
                        break;
                    }
                    // Only the value’s first token can open a bracketed
                    // collection.
                    // Brackets and quotes within block plain scalars,
                    // including block list items, never extend the member.
                    shape = if byte == b'[' || byte == b'{' {
                        Shape::Bracketed
                    } else if byte == b'\'' || byte == b'"' {
                        Shape::Quoted
                    } else if !first
                        && byte == b'-'
                        && (index + 1 == stop || space_or_tab(text[index + 1]))
                    {
                        Shape::Items
                    } else {
                        Shape::Scalar
                    };
                }
                if shape != Shape::Bracketed && shape != Shape::Quoted {
                    break;
                }
                if quote != 0 {
                    if quote == b'"' && byte == b'\\' && index + 1 < stop {
                        index += 1;
                    } else if byte == quote {
                        if quote == b'\'' && index + 1 < stop && text[index + 1] == b'\'' {
                            index += 1;
                        } else {
                            quote = 0;
                            if shape == Shape::Quoted {
                                shape = Shape::Scalar;
                            }
                        }
                    }
                } else if space_or_tab(byte) {
                    index += 1;
                    continue;
                } else if (byte == b'\'' || byte == b'"') && node {
                    // A quote opens a scalar only where a node starts:
                    // inside a plain scalar it is the scalar’s.
                    quote = byte;
                } else if byte == b'#' && (index == nonspace || space_or_tab(text[index - 1])) {
                    break;
                } else if byte == b'[' {
                    list_depth += 1;
                } else if byte == b'{' {
                    map_depth += 1;
                } else if byte == b']' || byte == b'}' {
                    // A mismatched closer cannot release an unfinished
                    // member.
                    let depth = if byte == b']' {
                        &mut list_depth
                    } else {
                        &mut map_depth
                    };
                    if *depth > 0 {
                        *depth -= 1;
                    }
                    if list_depth == 0 && map_depth == 0 {
                        shape = Shape::Scalar;
                    }
                }
                node = quote == 0 && matches!(byte, b'[' | b'{' | b',' | b':' | b'?');
                index += 1;
            }
            first = false;
            line = next_line(text, self.stop, self.end);
        }
        self.end
    }

    /// Read the member that starts its line at `start`, its name at `first`
    /// and the byte after the name’s colon at `colon`, and ends at `end`.
    /// A member without a name, and one whose plain name is no field or a
    /// field already set, is skipped without reading its value.
    fn member(&mut self, start: usize, first: usize, colon: usize, end: usize) {
        let text = self.text;
        if colon == 0 {
            return;
        }
        if text[first] != b'"' && text[first] != b'\'' {
            let mut stop = colon - 1;
            while stop > first && space_or_tab(text[stop - 1]) {
                stop -= 1;
            }
            match field(&mut self.fields, &text[first..stop]) {
                Some(slot) if slot.is_none() => {}
                _ => return,
            }
        }
        if let Some((name, value)) = self.value(start, end) {
            if let Some(slot) = field(&mut self.fields, &name) {
                if slot.is_none() {
                    *slot = Some(value);
                }
            }
        }
    }

    /// The name and value of the member from `start` to `end`: a block
    /// mapping of one name, whose value is a scalar on the name’s line, a
    /// literal there on a prose field, or a list of text and numbers.
    fn value(&mut self, start: usize, end: usize) -> Option<(Vec<u8>, Value)> {
        let events = reader::read(self.text, start, end)?;
        let count = events.len();
        // The stream, its implicit document and its block mapping, around
        // one name and its value.
        let valid = count >= 8
            && events[0].kind == Kind::StreamStart
            && events[1].kind == Kind::DocumentStart
            && !events[1].explicit
            && events[2].kind == Kind::MapStart
            && !events[2].flow
            && events[count - 1].kind == Kind::StreamEnd
            && events[count - 2].kind == Kind::DocumentEnd
            && !events[count - 2].explicit
            && events[count - 3].kind == Kind::MapEnd;
        if !valid {
            return None;
        }
        let events = &events[3..count - 3];
        // The name.
        let name = &events[0];
        if name.kind != Kind::Scalar
            || name.form == Form::Literal
            || name.text.is_empty()
            || name.text.iter().any(|byte| matches!(byte, b'\n' | b'\r'))
        {
            return None;
        }
        let line = name.line;
        let prose = name.text == b"abstract" || name.text == b"comment";
        match field(&mut self.fields, &name.text) {
            Some(slot) if slot.is_none() => {}
            _ => return None,
        }
        // The value.
        let value = &events[1];
        if value.kind == Kind::Scalar {
            if events.len() != 2 || value.line != line || (value.form == Form::Literal && !prose) {
                return None;
            }
            return Some((name.text.clone(), Value::Scalar(scalar(value)?)));
        }
        if value.kind != Kind::ListStart || events[events.len() - 1].kind != Kind::ListEnd {
            return None;
        }
        let mut items = Vec::new();
        for event in &events[2..events.len() - 1] {
            if event.kind != Kind::Scalar || event.form == Form::Literal {
                return None;
            }
            items.push(match scalar(event)? {
                Scalar::Text(text) => Item::Text(text),
                Scalar::Number(number) => Item::Number(number),
                _ => return None,
            });
        }
        Some((name.text.clone(), Value::List(items)))
    }

    /// Decode the envelope from `start` to `end`.
    /// A blank line, a comment line, a `%` line and a `...` line between
    /// members are no member.
    fn read(&mut self, start: usize) {
        let text = self.text;
        let mut line = start;
        while line < self.end {
            self.note_line(line);
            let first = self.first;
            let stop = self.stop;
            let mut content = first;
            while content < stop && space_or_tab(text[content]) {
                content += 1;
            }
            if content == stop
                || text[content] == b'#'
                || text[first] == b'%'
                || (stop - first >= 3
                    && &text[first..first + 3] == b"..."
                    && (stop == first + 3 || space_or_tab(text[first + 3])))
            {
                line = next_line(text, stop, self.end);
                continue;
            }
            let colon = self.colon;
            let end = self.member_end(line, first - line);
            self.member(line, first, colon, end);
            line = end;
        }
    }
}
