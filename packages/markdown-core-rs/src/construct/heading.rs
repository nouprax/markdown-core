//! Headings before their text is parsed: the attribute tail their text gives
//! back.
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

use crate::construct::text;
use crate::event::{Event, Kind, Name};
use crate::parser::ParseState;
use crate::state::{Name as StateName, State};
use crate::subtokenize::feed;
use crate::tokenizer::Tokenizer;
use crate::util::edit_map::EditMap;

/// Settle the tail of every heading in `events`, flow events whose text is not
/// parsed yet.
pub fn settle(events: &mut Vec<Event>, parse_state: &ParseState) {
    let mut map = EditMap::new();
    let mut index = 0;
    while index < events.len() {
        let event = &events[index];
        if event.kind == Kind::Enter
            && matches!(event.name, Name::HeadingAtxText | Name::HeadingSetextText)
        {
            let exit = close(events, index);
            tail(events, parse_state, &mut map, index, exit);
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
    let Some(last) = last else {
        return;
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
        State::Next(StateName::Text(text::Step::Before)),
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
