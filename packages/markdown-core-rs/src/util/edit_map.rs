//! Deal with several changes in events, batching them together.
//!
//! Preferably, changes should be kept to a minimum.
//! Sometimes, it’s needed to change the list of events, because parsing can be
//! messy, and it helps to expose a cleaner interface of events to the compiler
//! and other users.
//! It can also help to merge many adjacent similar events.
//! And, in other cases, it’s needed to parse subcontent: pass some events
//! through another tokenizer and inject the result.

use crate::event::Event;
use std::collections::{BTreeMap, VecDeque};

/// Shift `previous` and `next` links according to `leaps`.
///
/// This fixes links in case there are events removed or added between them.
fn shift_links(events: &mut [Event], leaps: &[(usize, usize, usize)]) {
    let mut leap_index = 0;
    let mut index = 0;
    let mut add = 0;
    let mut rm = 0;

    while index < events.len() {
        let rm_curr = rm;

        while leap_index < leaps.len() && leaps[leap_index].0 <= index {
            add = leaps[leap_index].2;
            rm = leaps[leap_index].1;
            leap_index += 1;
        }

        // Ignore items that will be removed.
        if rm > rm_curr {
            index += rm - rm_curr;
        } else {
            if let Some(link) = &events[index].link {
                if let Some(next) = link.next {
                    events[next].link.as_mut().unwrap().previous = Some(index + add - rm);

                    while leap_index < leaps.len() && leaps[leap_index].0 <= next {
                        add = leaps[leap_index].2;
                        rm = leaps[leap_index].1;
                        leap_index += 1;
                    }

                    events[index].link.as_mut().unwrap().next = Some(next + add - rm);
                    index = next;
                    continue;
                }
            }

            index += 1;
        }
    }
}

/// The edit at one index: how many events it removes, and the lists of
/// events it adds there, in order.
#[derive(Debug, Default)]
struct Edit {
    remove: usize,
    count: usize,
    lists: VecDeque<Vec<Event>>,
}

/// Tracks a bunch of edits, by index: each edit is found and extended,
/// at either end, in time logarithmic in their number and constant in what
/// it holds.
#[derive(Debug)]
pub struct EditMap {
    /// Record of changes.
    map: BTreeMap<usize, Edit>,
}

impl EditMap {
    /// Create a new edit map.
    pub fn new() -> EditMap {
        EditMap {
            map: BTreeMap::new(),
        }
    }
    /// Create an edit: a remove and/or add at a certain place.
    pub fn add(&mut self, index: usize, remove: usize, add: Vec<Event>) {
        add_impl(self, index, remove, add, false);
    }
    /// Create an edit: but insert `add` before existing additions.
    pub fn add_before(&mut self, index: usize, remove: usize, add: Vec<Event>) {
        add_impl(self, index, remove, add, true);
    }
    /// Done, change the events.
    pub fn consume(&mut self, events: &mut Vec<Event>) {
        if self.map.is_empty() {
            return;
        }

        // Calculate jumps: where items in the current list move to.
        let mut leaps = Vec::with_capacity(self.map.len());
        let mut add_acc = 0;
        let mut remove_acc = 0;
        for (at, edit) in &self.map {
            remove_acc += edit.remove;
            add_acc += edit.count;
            leaps.push((*at, remove_acc, add_acc));
        }

        shift_links(events, &leaps);

        let mut old = core::mem::take(events).into_iter();
        let mut done = 0;
        events.reserve(old.len() + add_acc - remove_acc);
        for (at, edit) in core::mem::take(&mut self.map) {
            events.extend(old.by_ref().take(at - done));
            for list in edit.lists {
                events.extend(list);
            }
            old.by_ref().take(edit.remove).for_each(drop);
            done = at + edit.remove;
        }
        events.extend(old);
    }
}

/// Create an edit.
fn add_impl(edit_map: &mut EditMap, at: usize, remove: usize, add: Vec<Event>, before: bool) {
    if remove == 0 && add.is_empty() {
        return;
    }

    let edit = edit_map.map.entry(at).or_default();
    edit.remove += remove;
    edit.count += add.len();
    if add.is_empty() {
        return;
    }
    if before {
        edit.lists.push_front(add);
    } else {
        edit.lists.push_back(add);
    }
}
