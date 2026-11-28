//! Citation key occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Citation key forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! citation_key ::= ['-'] '@' (bare | '{' braced '}')
//! bare ::= key_char *key_char *(joiner 1*key_char)
//! joiner ::= ':' | '.' | '#' | '$' | '%' | '&' | '-' | '+' | '?' | '<' | '>' | '~' | '/'
//! braced ::= 1*(brace_byte | opaque | '{' braced '}')
//! brace_byte ::= byte - (whitespace | '{' | '}')
//! opaque ::= code_text | html_text | autolink
//!
//! ; Restriction: the first byte (`-` or `@`) is not after a key character.
//! ; Restriction: an opaque span holds no whitespace.
//! ```
//!
//! Braces in a code span, in HTML or in an autolink inside a braced key do not
//! count: those spans are opaque, and the key keeps their bytes as they are.
//!
//! A key character is a letter, a number, or `_`, by Unicode category, as in
//! Pandoc: a bare key has no delimiter after it, so the class of the next
//! character ends it.
//! A joiner joins two runs of key characters, so a final period stays text.
//!
//! A key alone is an author-in-text citation (`-@` suppresses the author).
//! Brackets make groups of citations, and a bracket after a key is its tail:
//! both are decided where the bracket closes, in
//! [`label_end`][crate::construct::label_end].
//!
//! ## Tokens
//!
//! * [`CitationKey`][Name::CitationKey]
//! * [`CitationMarker`][Name::CitationMarker]
//! * [`CitationBrace`][Name::CitationBrace]
//! * [`CitationKeyText`][Name::CitationKeyText]
//!
//! ## References
//!
//! * [*Citations* in Pandoc’s manual](https://pandoc.org/MANUAL.html#citation-syntax)
//!
//! [text]: crate::construct::text

use crate::choice::Starts;
use crate::construct::{autolink, html_text, raw_text};
use crate::event::{Event, Kind, Name, Point};
use crate::state::{Name as StateName, State};
use crate::tokenizer::{Label, LabelKind, Tokenizer};
use crate::util::char::{after_index, before_index};
use crate::util::classes::{classes, LETTER, NUMBER};
use crate::util::slice::advance;
use alloc::vec::Vec;

/// Bytes this construct may start at.
pub const STARTS: Starts = Starts::Bytes(b"-@");

/// Whether `char` is a key character: a letter, a number, or `_`.
pub fn key_char(char: Option<char>) -> bool {
    char.map_or(false, |char| {
        char == '_' || classes(char) & (LETTER | NUMBER) != 0
    })
}

/// The width of the key character at the current byte, or 0.
fn key_width(tokenizer: &Tokenizer) -> usize {
    let char = after_index(tokenizer.parse_state.bytes, tokenizer.point.index);
    if key_char(char) {
        char.map_or(0, char::len_utf8)
    } else {
        0
    }
}

/// Start of a citation key.
///
/// ```markdown
/// > | a @b c
///       ^
/// > | a -@b c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if !matches!(tokenizer.current, Some(b'-' | b'@'))
        || key_char(before_index(
            tokenizer.parse_state.bytes,
            tokenizer.point.index,
        ))
    {
        return State::Nok;
    }
    tokenizer.enter(Name::CitationKey);
    tokenizer.enter(Name::CitationMarker);
    if tokenizer.current == Some(b'-') {
        tokenizer.consume();
        return State::Next(StateName::Citation(Step::At));
    }
    State::Redo(StateName::Citation(Step::At))
}

/// At `@`.
///
/// ```markdown
/// > | a -@b c
///        ^
/// ```
fn at(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'@') {
        tokenizer.consume();
        State::Next(StateName::Citation(Step::MarkerAfter))
    } else {
        State::Nok
    }
}

/// After the marker, at the key.
///
/// ```markdown
/// > | a @b c
///        ^
/// > | a @{b} c
///        ^
/// ```
fn marker_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::CitationMarker);
    if tokenizer.current == Some(b'{') {
        tokenizer.enter(Name::CitationBrace);
        tokenizer.consume();
        tokenizer.exit(Name::CitationBrace);
        tokenizer.enter(Name::CitationKeyText);
        return State::Next(StateName::Citation(Step::Braced { depth: 0 }));
    }
    if key_width(tokenizer) == 0 {
        return State::Nok;
    }
    tokenizer.enter(Name::CitationKeyText);
    State::Redo(StateName::Citation(Step::Bare))
}

/// In a bare key, at a key character, a joiner, or its end.
///
/// ```markdown
/// > | a @b.c d
///        ^^^
/// ```
fn bare(tokenizer: &mut Tokenizer) -> State {
    let width = key_width(tokenizer);
    if width > 0 {
        tokenizer.consume();
        return State::Next(StateName::Citation(Step::Rest { left: width - 1 }));
    }
    if matches!(
        tokenizer.current,
        Some(
            b':' | b'.'
                | b'#'
                | b'$'
                | b'%'
                | b'&'
                | b'-'
                | b'+'
                | b'?'
                | b'<'
                | b'>'
                | b'~'
                | b'/'
        )
    ) {
        tokenizer.attempt(
            State::Next(StateName::Citation(Step::Bare)),
            State::Next(StateName::Citation(Step::End)),
        );
        return State::Redo(StateName::Citation(Step::Joiner));
    }
    State::Redo(StateName::Citation(Step::End))
}

/// In a key character of more than one byte.
///
/// ```markdown
/// > | a @é b
///        ^
/// ```
fn rest(tokenizer: &mut Tokenizer, left: usize) -> State {
    if left == 0 {
        return State::Redo(StateName::Citation(Step::Bare));
    }
    tokenizer.consume();
    State::Next(StateName::Citation(Step::Rest { left: left - 1 }))
}

/// At a joiner, which a key character must follow.
///
/// ```markdown
/// > | a @b.c d
///         ^
/// ```
fn joiner(tokenizer: &mut Tokenizer) -> State {
    tokenizer.consume();
    State::Next(StateName::Citation(Step::JoinerAfter))
}

/// After a joiner.
///
/// ```markdown
/// > | a @b.c d
///          ^
/// ```
fn joiner_after(tokenizer: &mut Tokenizer) -> State {
    if key_width(tokenizer) > 0 {
        State::Ok
    } else {
        State::Nok
    }
}

/// In a braced key.
///
/// ```markdown
/// > | a @{b{c}} d
///         ^^^^
/// > | a @{b`}`} d
///         ^^^^
/// ```
fn braced(tokenizer: &mut Tokenizer, depth: usize) -> State {
    match tokenizer.current {
        None | Some(b'\t' | b'\n' | b'\r' | b' ') => State::Nok,
        // An empty pair of braces holds no key.
        Some(b'}') if tokenizer.previous == Some(b'{') => State::Nok,
        Some(b'}') if depth == 0 => {
            tokenizer.exit(Name::CitationKeyText);
            tokenizer.enter(Name::CitationBrace);
            tokenizer.consume();
            tokenizer.exit(Name::CitationBrace);
            tokenizer.exit(Name::CitationKey);
            State::Ok
        }
        Some(byte @ (b'`' | b'<')) if !escaped(tokenizer) => {
            let span = Span {
                depth,
                mark: tokenizer.events.len(),
                from: tokenizer.point.index,
            };
            if byte == b'`' {
                tokenizer.attempt(
                    State::Next(StateName::Citation(Step::Opaque(span))),
                    State::Next(StateName::Citation(Step::Byte { depth })),
                );
                State::Redo(StateName::RawText(raw_text::Step::Start))
            } else {
                tokenizer.attempt(
                    State::Next(StateName::Citation(Step::Opaque(span))),
                    State::Next(StateName::Citation(Step::Tag(span))),
                );
                State::Redo(StateName::HtmlText(html_text::Step::Start))
            }
        }
        Some(_) => State::Redo(StateName::Citation(Step::Byte { depth })),
    }
}

/// In a braced key, at a byte that is part of the key.
///
/// ```markdown
/// > | a @{b{c}} d
///         ^^^^
/// ```
fn byte(tokenizer: &mut Tokenizer, depth: usize) -> State {
    let depth = match tokenizer.current {
        Some(b'{') => depth + 1,
        Some(b'}') => depth - 1,
        _ => depth,
    };
    tokenizer.consume();
    State::Next(StateName::Citation(Step::Braced { depth }))
}

/// In a braced key, at a `<` that starts no HTML: maybe an autolink.
///
/// ```markdown
/// > | a @{b<c:d>} e
///          ^
/// ```
fn tag(tokenizer: &mut Tokenizer, span: Span) -> State {
    tokenizer.attempt(
        State::Next(StateName::Citation(Step::Opaque(span))),
        State::Next(StateName::Citation(Step::Byte { depth: span.depth })),
    );
    State::Redo(StateName::Autolink(autolink::Step::Start))
}

/// In a braced key, after an opaque span: it is key bytes, so its events
/// go, and whitespace in it ends the key.
///
/// ```markdown
/// > | a @{b`}`} d
///             ^
/// ```
fn opaque(tokenizer: &mut Tokenizer, span: Span) -> State {
    let bytes = &tokenizer.parse_state.bytes[span.from..tokenizer.point.index];
    if bytes
        .iter()
        .any(|byte| matches!(byte, b'\t' | b'\n' | b'\r' | b' '))
    {
        return State::Nok;
    }
    tokenizer.events.truncate(span.mark);
    State::Redo(StateName::Citation(Step::Braced { depth: span.depth }))
}

/// Whether the byte at the cursor follows an odd run of backslashes.
fn escaped(tokenizer: &Tokenizer) -> bool {
    let bytes = &tokenizer.parse_state.bytes[..tokenizer.point.index];
    bytes
        .iter()
        .rev()
        .take_while(|byte| **byte == b'\\')
        .count()
        % 2
        == 1
}

/// After a bare key.
///
/// ```markdown
/// > | a @b. c
///         ^
/// ```
fn end(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::CitationKeyText);
    tokenizer.exit(Name::CitationKey);
    State::Ok
}

/// A place between events, or in a data event before a byte inside it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cursor {
    /// The event the place is before, or the data event it is in.
    pub event: usize,
    /// The byte inside the data event.
    pub byte: Option<usize>,
}

/// A section of a bracket, between `;`s: where its content starts and
/// ends, past the whitespace around it, and its first key.
#[derive(Debug)]
pub struct Section {
    /// The content, or `None` when the section is empty.
    pub content: Option<(Cursor, Cursor)>,
    /// The enter index of the first key at its top level.
    pub key: Option<usize>,
    /// Where the content before the key ends, before whitespace.
    pub before: Option<Cursor>,
    /// Where the content after the key starts, past whitespace.
    pub after: Option<Cursor>,
}

/// The exit index of the token entered at `index`.
fn exit_of(events: &[Event], index: usize) -> usize {
    let mut depth = 0;
    let mut at = index;
    loop {
        if events[at].kind == Kind::Enter {
            depth += 1;
        } else {
            depth -= 1;
            if depth == 0 {
                return at;
            }
        }
        at += 1;
    }
}

/// The sections of the bracket whose start exits at `open` and whose end
/// enters at `close`, with `pairs` the brackets closed before it: those in
/// it are opaque, so their `;`s and keys are not its own.
pub fn sections(
    events: &[Event],
    bytes: &[u8],
    pairs: &[Label],
    open: usize,
    close: usize,
) -> Vec<Section> {
    let mut nested: Vec<(usize, usize)> = pairs
        .iter()
        .rev()
        .take_while(|pair| pair.end.1 > open)
        .map(|pair| (pair.start.0, pair.end.1))
        .collect();
    // In the order they start: a pair closes before one around it does.
    nested.sort_unstable();
    let mut list = Vec::new();
    let mut section = Section {
        content: None,
        key: None,
        before: None,
        after: None,
    };
    let mut next = 0;
    // The brackets open in the bracket that no label closed, whose `;` are
    // their own.
    let mut depth = 0;
    let mut index = open + 1;
    while index < close {
        while next < nested.len() && nested[next].0 < index {
            next += 1;
        }
        if next < nested.len() && nested[next].0 == index {
            let end = nested[next].1 + 1;
            extend(&mut section, whole(index), whole(end));
            index = end;
            continue;
        }
        let exit = exit_of(events, index);
        match events[index].name {
            Name::Data => {
                let from = events[index].point.index;
                let to = events[exit].point.index;
                for at in from..to {
                    match bytes[at] {
                        b';' if depth == 0 => {
                            list.push(section);
                            section = Section {
                                content: None,
                                key: None,
                                before: None,
                                after: None,
                            };
                        }
                        // A space after a backslash is content (a nonbreaking
                        // space in Pandoc).
                        b'\t' | b' ' if at == from || bytes[at - 1] != b'\\' => {}
                        byte => {
                            if byte == b']' && depth > 0 {
                                depth -= 1;
                            }
                            let start = if at == from {
                                whole(index)
                            } else {
                                Cursor {
                                    event: index,
                                    byte: Some(at),
                                }
                            };
                            let end = if at + 1 == to {
                                whole(exit + 1)
                            } else {
                                Cursor {
                                    event: index,
                                    byte: Some(at + 1),
                                }
                            };
                            extend(&mut section, start, end);
                        }
                    }
                }
            }
            // The line ending of a hard break (escape) is the break’s.
            Name::LineEnding if index > 0 && events[index - 1].name == Name::HardBreakEscape => {
                extend(&mut section, whole(index), whole(exit + 1));
            }
            Name::LineEnding | Name::SpaceOrTab => {}
            // The first key is neither prefix nor suffix.
            Name::CitationKey if section.key.is_none() => {
                section.key = Some(index);
                section.content = Some(match section.content {
                    Some((first, _)) => (first, whole(exit + 1)),
                    None => (whole(index), whole(exit + 1)),
                });
            }
            name => {
                if matches!(
                    name,
                    Name::LabelLink | Name::LabelImage | Name::LabelNote | Name::FootnoteCallLabel
                ) {
                    depth += 1;
                } else if name == Name::LabelEnd && depth > 0 {
                    depth -= 1;
                }
                extend(&mut section, whole(index), whole(exit + 1));
            }
        }
        index = exit + 1;
    }
    list.push(section);
    list
}

/// The place before the event at `event`.
fn whole(event: usize) -> Cursor {
    Cursor { event, byte: None }
}

/// Take the content from `start` to `end` into `section`.
fn extend(section: &mut Section, start: Cursor, end: Cursor) {
    if section.key.is_none() {
        section.before = Some(end);
    } else if section.after.is_none() {
        section.after = Some(start);
    }
    section.content = Some(match section.content {
        Some((first, _)) => (first, end),
        None => (start, end),
    });
}

/// The enter index of the key right before the bracket that enters at
/// `start`, past spaces, tabs, and at most one line ending.
pub fn author(events: &[Event], bytes: &[u8], start: usize) -> Option<usize> {
    let mut index = start;
    let mut breaks = 0;
    while index > 0 {
        let event = &events[index - 1];
        if event.kind != Kind::Exit {
            return None;
        }
        let mut enter = index - 2;
        while events[enter].kind != Kind::Enter || events[enter].name != event.name {
            enter -= 1;
        }
        match event.name {
            Name::CitationKey => return Some(enter),
            Name::LineEnding => {
                breaks += 1;
                if breaks > 1 {
                    return None;
                }
            }
            Name::SpaceOrTab => {}
            Name::Data => {
                if !bytes[events[enter].point.index..event.point.index]
                    .iter()
                    .all(|byte| matches!(byte, b'\t' | b' '))
                {
                    return None;
                }
            }
            _ => return None,
        }
        index = enter;
    }
    None
}

/// Whether a bracket with `sections` is a group: each holds a key.
pub fn group(sections: &[Section]) -> bool {
    sections.iter().all(|section| section.key.is_some())
}

/// Whether a bracket with `sections` is a tail: its first section is the
/// suffix of the key before it when it holds no key, an empty one included
/// (`@k []`), and each later section holds a key.
pub fn tail(sections: &[Section]) -> bool {
    group(&sections[1..])
}

/// Insert the events of the group or the tail `label`, with `author` the
/// key a tail follows, and `pairs` the brackets closed before it.
pub fn insert(tokenizer: &mut Tokenizer, label: &Label, author: Option<usize>, pairs: &[Label]) {
    let events = &tokenizer.events;
    let bytes = tokenizer.parse_state.bytes;
    let all = sections(events, bytes, pairs, label.start.1, label.end.0);
    let mut inserts: Vec<(Cursor, Vec<Event>)> = Vec::new();
    let mut add = |cursor: Cursor, names: &[(Kind, Name)]| {
        let point = match cursor.byte {
            Some(byte) => advance(bytes, &events[cursor.event].point, byte),
            // Before an event, or after the last one.
            None => match events.get(cursor.event) {
                Some(event) => event.point.clone(),
                None => events[events.len() - 1].point.clone(),
            },
        };
        let mut made: Vec<Event> = names
            .iter()
            .map(|(kind, name)| Event {
                kind: kind.clone(),
                name: *name,
                point: point.clone(),
                link: None,
            })
            .collect();
        match inserts.last_mut() {
            Some((last, events)) if *last == cursor => events.append(&mut made),
            _ => inserts.push((cursor, made)),
        }
    };
    let mut rest = &all[..];
    match author {
        Some(key) => {
            add(
                whole(key),
                &[(Kind::Enter, Name::Cite), (Kind::Enter, Name::Citation)],
            );
            if all[0].key.is_none() {
                // An empty suffix is where the bracket opens.
                let open = whole(label.start.1 + 1);
                let (start, end) = all[0].content.unwrap_or((open, open));
                add(start, &[(Kind::Enter, Name::CitationSuffix)]);
                add(
                    end,
                    &[
                        (Kind::Exit, Name::CitationSuffix),
                        (Kind::Exit, Name::Citation),
                    ],
                );
                rest = &all[1..];
            } else {
                add(
                    whole(exit_of(events, key) + 1),
                    &[(Kind::Exit, Name::Citation)],
                );
            }
        }
        None => add(whole(label.start.0), &[(Kind::Enter, Name::Cite)]),
    }
    for section in rest {
        debug_assert!(section.key.is_some(), "expected a key");
        let (start, end) = section.content.expect("expected content");
        add(start, &[(Kind::Enter, Name::Citation)]);
        if let Some(before) = section.before {
            add(start, &[(Kind::Enter, Name::CitationPrefix)]);
            add(before, &[(Kind::Exit, Name::CitationPrefix)]);
        }
        if let Some(after) = section.after {
            add(after, &[(Kind::Enter, Name::CitationSuffix)]);
            add(end, &[(Kind::Exit, Name::CitationSuffix)]);
        }
        add(end, &[(Kind::Exit, Name::Citation)]);
    }
    add(whole(label.end.1 + 1), &[(Kind::Exit, Name::Cite)]);
    let nested: Vec<usize> = pairs
        .iter()
        .rev()
        .take_while(|pair| pair.end.1 > label.start.1)
        .map(|pair| match pair.kind {
            LabelKind::Tail { author, .. } => author,
            _ => pair.start.0,
        })
        .collect();
    place(tokenizer, &inserts, &nested);
}

/// Place `inserts`: between events, or cutting a data event in two.
/// The brackets that enter at `nested` are inside, and were inserted
/// before: what enters where one of them does goes around it.
fn place(tokenizer: &mut Tokenizer, inserts: &[(Cursor, Vec<Event>)], nested: &[usize]) {
    let mut index = 0;
    while index < inserts.len() {
        let (cursor, _) = &inserts[index];
        if cursor.byte.is_none() {
            let (cursor, list) = inserts[index].clone();
            if nested.contains(&cursor.event) {
                tokenizer.map.add_before(cursor.event, 0, list);
            } else {
                tokenizer.map.add(cursor.event, 0, list);
            }
            index += 1;
            continue;
        }
        // All the cuts of one data event.
        let data = cursor.event;
        let enter = tokenizer.events[data].clone();
        let exit = tokenizer.events[data + 1].clone();
        let mut list = Vec::new();
        let mut from: Point = enter.point.clone();
        while index < inserts.len()
            && inserts[index].0.event == data
            && inserts[index].0.byte.is_some()
        {
            let (cursor, events) = inserts[index].clone();
            let point = advance(
                tokenizer.parse_state.bytes,
                &enter.point,
                cursor.byte.expect("expected a byte"),
            );
            list.push(Event {
                kind: Kind::Enter,
                name: Name::Data,
                point: from,
                link: None,
            });
            list.push(Event {
                kind: Kind::Exit,
                name: Name::Data,
                point: point.clone(),
                link: None,
            });
            list.extend(events);
            from = point;
            index += 1;
        }
        list.push(Event {
            kind: Kind::Enter,
            name: Name::Data,
            point: from,
            link: None,
        });
        list.push(exit);
        tokenizer.map.add(data, 2, list);
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    At,
    MarkerAfter,
    Bare,
    Rest { left: usize },
    Joiner,
    JoinerAfter,
    Braced { depth: usize },
    Byte { depth: usize },
    Tag(Span),
    Opaque(Span),
    End,
}

/// An opaque span in a braced key: the depth of braces around it, the events
/// before it, and its first byte.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Span {
    depth: usize,
    mark: usize,
    from: usize,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Citation(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::At => at(tokenizer),
        Step::MarkerAfter => marker_after(tokenizer),
        Step::Bare => bare(tokenizer),
        Step::Rest { left } => rest(tokenizer, left),
        Step::Joiner => joiner(tokenizer),
        Step::JoinerAfter => joiner_after(tokenizer),
        Step::Braced { depth } => braced(tokenizer, depth),
        Step::Byte { depth } => byte(tokenizer, depth),
        Step::Tag(span) => tag(tokenizer, span),
        Step::Opaque(span) => opaque(tokenizer, span),
        Step::End => end(tokenizer),
    }
}
