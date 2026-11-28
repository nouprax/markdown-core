//! Block identifier occurs at the end of a [paragraph][paragraph], and in
//! the [flow][] content type on a line of its own.
//!
//! ## Grammar
//!
//! Block identifier forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: at the end of the text of a paragraph, after whitespace or
//! ; on a line of its own, but for spaces, tabs, and line endings.
//! ; Restriction: on a line of its own, at most 3 spaces of indent.
//! block_identifier ::= '#' 1*(ascii_alphanumeric | '-') '#'
//! ```
//!
//! An identifier ends the paragraph it is in, and is its anchor; when the
//! paragraph is the first block of a list item and the identifier is on the
//! first line of the item, it is the anchor of the item.
//! A paragraph of an identifier alone is text, unless it names the item.
//!
//! On a line of its own after a blank line, and before a blank line or the
//! end of its container, an identifier names the list, callout, or table
//! right before it, when that has no anchor; otherwise it is a paragraph.
//!
//! ## Tokens
//!
//! * [`BlockIdentifier`][Name::BlockIdentifier]
//! * [`BlockIdentifierLine`][Name::BlockIdentifierLine]
//! * [`BlockIdentifierTail`][Name::BlockIdentifierTail]
//!
//! [paragraph]: crate::construct::paragraph
//! [flow]: crate::construct::flow

use crate::choice::Starts;
use crate::construct::parts::space_or_tab::{space_or_tab, space_or_tab_min_max};
use crate::event::{Content, Event, Kind, Link, Name, Point};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::{constant::TAB_SIZE, slice::advance};
use alloc::{vec, vec::Vec};

/// Bytes an identifier line can start at.
pub const FLOW: Starts = Starts::Bytes(b"\t #");

/// Whether `byte` can be in an identifier.
fn inner(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'-'
}

/// Where an identifier is in `text`: the place of its first `#`, the end of
/// its last, and where the content before it ends; with whether it is on a
/// line of its own.
struct Found {
    start: usize,
    end: usize,
    content: usize,
    alone: bool,
}

/// The identifier `text` ends with.
fn scan(text: &[u8]) -> Option<Found> {
    let mut end = text.len();
    while end > 0 && matches!(text[end - 1], b'\n' | b'\r') {
        end -= 1;
    }
    while end > 0 && matches!(text[end - 1], b'\t' | b' ') {
        end -= 1;
    }
    if end < 3 || text[end - 1] != b'#' {
        return None;
    }
    let mut start = end - 1;
    while start > 0 && inner(text[start - 1]) {
        start -= 1;
    }
    if start == 0 || start == end - 1 || text[start - 1] != b'#' {
        return None;
    }
    let start = start - 1;
    let mut cut = start;
    while cut > 0 && matches!(text[cut - 1], b'\t' | b' ') {
        cut -= 1;
    }
    let alone = cut == 0 || matches!(text[cut - 1], b'\n' | b'\r');
    if !alone && cut == start {
        return None;
    }
    if alone && cut > 0 {
        if text[cut - 1] == b'\n' {
            cut -= 1;
        }
        if cut > 0 && text[cut - 1] == b'\r' {
            cut -= 1;
        }
    }
    Some(Found {
        start,
        end,
        content: cut,
        alone,
    })
}

/// At the end of a paragraph: cut the identifier its text ends with from
/// its data, as a block identifier after it.
pub fn tail(tokenizer: &mut Tokenizer) {
    let bytes = tokenizer.parse_state.bytes;
    // The data chunks of the paragraph, one per line: their enter indices.
    let mut chunks = Vec::new();
    let mut index = tokenizer.events.len();
    while index > 0 {
        index -= 1;
        let event = &tokenizer.events[index];
        if event.name == Name::Paragraph {
            break;
        }
        if event.name == Name::Data && event.kind == Kind::Enter {
            chunks.push(index);
        }
    }
    chunks.reverse();
    // The text, and where each chunk starts in it.
    let mut text = Vec::new();
    let mut starts = Vec::with_capacity(chunks.len());
    for enter in &chunks {
        starts.push(text.len());
        let from = tokenizer.events[*enter].point.index;
        let to = tokenizer.events[*enter + 1].point.index;
        text.extend_from_slice(&bytes[from..to]);
    }
    let found = match scan(&text) {
        Some(found) => found,
        None => return,
    };
    // The source place of an offset in the text.
    let place = |offset: usize| -> usize {
        let chunk = starts
            .iter()
            .rposition(|start| *start <= offset)
            .unwrap_or(0);
        tokenizer.events[chunks[chunk]].point.index + offset - starts[chunk]
    };
    let chunk_of = |offset: usize| {
        starts
            .iter()
            .rposition(|start| *start <= offset)
            .unwrap_or(0)
    };
    let mark = chunk_of(found.start);
    // On a line of its own, an identifier indented as code is text.
    if found.alone {
        // A line starts after its containers.
        let enter = &tokenizer.events[chunks[mark]].point;
        let at = advance(bytes, enter, place(found.start));
        if at.column - enter.column >= TAB_SIZE {
            return;
        }
    }
    let start = advance(
        bytes,
        &tokenizer.events[chunks[mark]].point,
        place(found.start),
    );
    let end = advance(bytes, &start, start.index + found.end - found.start);
    // Cut the data where the content ends: what follows is the tail, which
    // a setext heading takes back (`restore`).
    let keep = if found.content == 0 {
        0
    } else {
        chunk_of(found.content - 1) + 1
    };
    let from = chunks[if keep == 0 { 0 } else { keep - 1 }];
    let cut_index = if keep == 0 {
        0
    } else {
        place(found.content - 1) + 1
    };
    let old = tokenizer.events.split_off(from);
    let mut rest = &old[..];
    if keep > 0 {
        let cut = advance(bytes, &old[0].point, cut_index);
        let mut enter = old[0].clone();
        if let Some(link) = enter.link.as_mut() {
            link.next = None;
        }
        let mut exit = old[1].clone();
        exit.point = cut.clone();
        tokenizer.events.push(enter);
        tokenizer.events.push(exit);
        if cut.index < old[1].point.index {
            tokenizer.events.push(dormant(Kind::Enter, cut));
            tokenizer
                .events
                .push(dormant(Kind::Exit, old[1].point.clone()));
        }
        rest = &old[2..];
    }
    for event in rest {
        tokenizer
            .events
            .push(dormant(event.kind.clone(), event.point.clone()));
    }
    tokenizer.events.push(Event {
        kind: Kind::Enter,
        name: Name::BlockIdentifier,
        point: start,
        link: None,
    });
    tokenizer.events.push(Event {
        kind: Kind::Exit,
        name: Name::BlockIdentifier,
        point: end,
        link: None,
    });
}

/// An event of the tail of a paragraph after its identifier is cut.
fn dormant(kind: Kind, point: Point) -> Event {
    Event {
        kind,
        name: Name::BlockIdentifierTail,
        point,
        link: None,
    }
}

/// Give the paragraph from `enter` to `exit` its identifier back, as data:
/// a setext heading has no identifier.
pub fn restore(tokenizer: &mut Tokenizer, enter: usize, exit: usize) {
    let mut last = None;
    let mut index = enter + 1;
    while index < exit {
        let name = tokenizer.events[index].name;
        if name == Name::BlockIdentifier {
            tokenizer.map.add(index, 2, vec![]);
            index += 2;
            continue;
        }
        if name == Name::BlockIdentifierTail {
            tokenizer.events[index].name = Name::Data;
            tokenizer.events[index + 1].name = Name::Data;
            tokenizer.events[index].link = Some(Link {
                previous: last,
                next: None,
                content: Content::Text,
            });
        }
        if tokenizer.events[index].name == Name::Data && tokenizer.events[index].kind == Kind::Enter
        {
            if let Some(previous) = last {
                if let Some(link) = tokenizer.events[previous].link.as_mut() {
                    link.next = Some(index);
                }
                if let Some(link) = tokenizer.events[index].link.as_mut() {
                    link.previous = Some(previous);
                }
            }
            last = Some(index);
        }
        index += 1;
    }
}

/// Start of an identifier line.
///
/// ```markdown
///   | > a
///   |
/// > | #b#
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    // A line in a paragraph is the paragraph’s.
    if tokenizer.interrupt {
        return State::Nok;
    }
    tokenizer.enter(Name::BlockIdentifierLine);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::BlockIdentifier(Step::Before)),
            State::Nok,
        );
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::BlockIdentifier(Step::Before))
    }
}

/// At the first `#`.
///
/// ```markdown
/// > | #b#
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'#') {
        tokenizer.enter(Name::BlockIdentifier);
        tokenizer.consume();
        State::Next(StateName::BlockIdentifier(Step::Inside { size: 0 }))
    } else {
        State::Nok
    }
}

/// In the identifier; `size` is how much of it is read.
///
/// ```markdown
/// > | #b#
///      ^
/// ```
fn inside(tokenizer: &mut Tokenizer, size: usize) -> State {
    match tokenizer.current {
        Some(byte) if inner(byte) => {
            tokenizer.consume();
            State::Next(StateName::BlockIdentifier(Step::Inside { size: size + 1 }))
        }
        Some(b'#') if size > 0 => {
            tokenizer.consume();
            State::Next(StateName::BlockIdentifier(Step::Close))
        }
        _ => State::Nok,
    }
}

/// After the last `#`.
///
/// ```markdown
/// > | #b#
///        ^
/// ```
fn close(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::BlockIdentifier);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::BlockIdentifier(Step::After)),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::BlockIdentifier(Step::After))
    }
}

/// After the identifier and whitespace.
///
/// ```markdown
/// > | #b#
///        ^
/// ```
fn after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None if end(tokenizer) => State::Redo(StateName::BlockIdentifier(Step::Done)),
        Some(b'\n') => {
            tokenizer.check(
                State::Next(StateName::BlockIdentifier(Step::Done)),
                State::Nok,
            );
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            State::Next(StateName::BlockIdentifier(Step::Follow))
        }
        _ => State::Nok,
    }
}

/// In the check, at the line after: blank, in the same containers, or the end.
///
/// ```markdown
///   | #b#
/// > |
///     ^
/// ```
fn follow(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None if end(tokenizer) => State::Ok,
        // The end of a container, at a line that closes it or holds no
        // more of it, is content that follows, and so is a line of other
        // containers.
        None => State::Nok,
        _ if tokenizer.lazy || tokenizer.closing => State::Nok,
        Some(b'\n') => State::Ok,
        Some(b'\t' | b' ') => {
            tokenizer.attempt(
                State::Next(StateName::BlockIdentifier(Step::Blank)),
                State::Nok,
            );
            State::Redo(space_or_tab())
        }
        _ => State::Nok,
    }
}

/// Whether the cursor is at the end of the input, which may stand for the
/// blank line after an identifier line, rather than at the end of its
/// container.
fn end(tokenizer: &Tokenizer) -> bool {
    tokenizer.point.index >= tokenizer.parse_state.bytes.len()
}

/// In the check, after whitespace on the line after.
fn blank(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) {
        State::Ok
    } else {
        State::Nok
    }
}

/// At the end of the identifier line.
///
/// ```markdown
/// > | #b#
///        ^
/// ```
fn done(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::BlockIdentifierLine);
    // Feel free to interrupt.
    tokenizer.interrupt = false;
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    Inside { size: usize },
    Close,
    After,
    Follow,
    Blank,
    Done,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::BlockIdentifier(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::Inside { size } => inside(tokenizer, size),
        Step::Close => close(tokenizer),
        Step::After => after(tokenizer),
        Step::Follow => follow(tokenizer),
        Step::Blank => blank(tokenizer),
        Step::Done => done(tokenizer),
    }
}
