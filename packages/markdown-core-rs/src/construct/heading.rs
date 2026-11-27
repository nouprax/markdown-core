//! Headings before their text is parsed: the attribute tail their text gives
//! back, and the reference label they declare.
//!
//! ## Tail
//!
//! Attributes that end the last line of a heading are its own, unless a token
//! of its text crosses into them: a code span, HTML, or another token that
//! starts in the text and ends in the attributes.
//! Then the attributes are text:
//!
//! ```markdown
//! # `x{#y k=`}
//! ```
//!
//! The text is tokenized with the tail, unresolved, to see whether a token
//! crosses; which tokens form there depends on no definition.
//!
//! ## Label
//!
//! A heading whose text can be written as a reference label declares that
//! label, so `[First chapter]` refers to `# First chapter`, before or after
//! it.
//! The label is the authored text of the heading: without its sequences and
//! its attributes, with inline markup kept, and normalized as a reference
//! label is.
//!
//! The text can be written as a label when it holds at most
//! [`LINK_REFERENCE_SIZE_MAX`][] bytes and no unescaped bracket.
//! A heading with an unescaped bracket declares nothing, even when the bracket
//! is in a code span: the label could not be written.
//! So the label depends on no inline grammar, and therefore on no definition:
//! the labels are known before any text is parsed.
//!
//! An explicit link definition wins over a heading with the same label, and
//! repeated labels name the first heading: both are for the application that
//! resolves references, as the parser only records which labels are defined.
//!
//! [`LINK_REFERENCE_SIZE_MAX`]: crate::util::constant::LINK_REFERENCE_SIZE_MAX

use crate::construct::text;
use crate::event::{Event, Kind, Name};
use crate::parser::ParseState;
use crate::state::{Name as StateName, State};
use crate::subtokenize::feed;
use crate::tokenizer::{Definitions, Tokenizer};
use crate::util::constant::LINK_REFERENCE_SIZE_MAX;
use crate::util::edit_map::EditMap;
use crate::util::normalize_identifier::normalize_identifier;
use alloc::string::String;
use alloc::vec::Vec;

/// Settle the tail and declare the label of every heading in `events`, flow
/// events whose text is not parsed yet.
pub fn settle(events: &mut Vec<Event>, parse_state: &ParseState, definitions: &mut Definitions) {
    let mut map = EditMap::new();
    let mut index = 0;
    while index < events.len() {
        let event = &events[index];
        if event.kind == Kind::Enter
            && matches!(event.name, Name::HeadingAtxText | Name::HeadingSetextText)
        {
            let exit = close(events, index);
            tail(events, parse_state, &mut map, index, exit);
            if let Some(label) = label(events, parse_state.bytes, index, exit) {
                definitions.links.push(label);
            }
            index = exit;
        }
        index += 1;
    }
    map.consume(events);
}

/// The index of the exit of the event entered at `enter`.
fn close(events: &[Event], enter: usize) -> usize {
    let mut depth = 0;
    let mut index = enter + 1;
    loop {
        if events[index].kind == Kind::Exit {
            if depth == 0 {
                return index;
            }
            depth -= 1;
        } else {
            depth += 1;
        }
        index += 1;
    }
}

/// Give the attributes after the heading text from `enter` to `exit` back to
/// the text, when a token of the text crosses into them.
fn tail(
    events: &mut [Event],
    parse_state: &ParseState,
    map: &mut EditMap,
    enter: usize,
    exit: usize,
) {
    let mut start = exit + 1;
    while events[start].name == Name::SpaceOrTab {
        start += 2;
    }
    if events[start].kind != Kind::Enter || events[start].name != Name::Attributes {
        return;
    }
    let end = close(events, start);
    let last = (enter + 1..exit)
        .rev()
        .find(|index| events[*index].kind == Kind::Enter && events[*index].link.is_some());
    let last = match last {
        Some(last) => last,
        None => return,
    };
    let first = enter + 1;
    let saved = events[last + 1].point.clone();
    events[last + 1].point = events[end].point.clone();
    if crosses(events, parse_state, first, events[start].point.index) {
        events[exit].point = events[end].point.clone();
        map.add(exit + 1, end - exit, Vec::new());
    } else {
        events[last + 1].point = saved;
    }
}

/// Whether a token of the text whose first chunk is at `first` crosses `cut`.
fn crosses(events: &[Event], parse_state: &ParseState, first: usize, cut: usize) -> bool {
    let mut tokenizer = Tokenizer::new(events[first].point.clone(), parse_state);
    let state = feed(
        &mut tokenizer,
        events,
        first,
        State::Next(StateName::Text(text::Step::Start)),
    );
    tokenizer.flush(state, false);
    let list = &tokenizer.events;
    (0..list.len().saturating_sub(1)).any(|index| {
        let (enter, exit) = (&list[index], &list[index + 1]);
        enter.kind == Kind::Enter
            && exit.kind == Kind::Exit
            && enter.name != Name::Data
            && enter.point.index < cut
            && exit.point.index > cut
    })
}

/// The label of the heading text from `enter` to `exit`, if it can be
/// written.
fn label(events: &[Event], bytes: &[u8], enter: usize, exit: usize) -> Option<String> {
    let mut label = Vec::new();
    let mut depth = 0;
    for index in enter + 1..exit {
        let event = &events[index];
        if event.kind == Kind::Exit {
            depth -= 1;
        } else {
            if depth == 0 && matches!(event.name, Name::Data | Name::LineEnding) {
                let end = &events[index + 1].point;
                label.extend_from_slice(&bytes[event.point.index..end.index]);
            }
            depth += 1;
        }
    }
    if label.len() > LINK_REFERENCE_SIZE_MAX || bracket(&label) {
        return None;
    }
    core::str::from_utf8(&label)
        .ok()
        .map(normalize_identifier)
        .filter(|label| !label.is_empty())
}

/// Whether `bytes` hold an unescaped bracket.
fn bracket(bytes: &[u8]) -> bool {
    let mut index = 0;
    while index < bytes.len() {
        match bytes[index] {
            b'\\' if bytes.get(index + 1).map_or(false, u8::is_ascii_punctuation) => index += 1,
            b'[' | b']' => return true,
            _ => {}
        }
        index += 1;
    }
    false
}
