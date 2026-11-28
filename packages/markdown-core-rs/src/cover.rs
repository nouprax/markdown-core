//! Coverage of the state machine: which steps ran after which. Each step
//! decides its successor, so these pairs are the branches of the machine.
//!
//! Only with the `cover` feature, for the coverage tools in
//! `scripts/correctness`.

extern crate std;

use crate::state::Name;
use alloc::collections::{BTreeMap, BTreeSet};
use alloc::string::String;
use alloc::vec::Vec;
use core::cell::RefCell;
use core::fmt::{self, Write};

/// What coverage has seen: each step once, by name and in order, the step
/// that ran last, and the steps after steps, as indices into `steps`.
struct Seen {
    recent: Vec<(Name, usize)>,
    index: BTreeMap<String, usize>,
    steps: Vec<String>,
    last: usize,
    told: usize,
    after: BTreeSet<(usize, usize)>,
}

std::thread_local! {
    static SEEN: RefCell<Seen> = RefCell::new(Seen {
        recent: Vec::new(),
        index: BTreeMap::new(),
        steps: Vec::new(),
        last: usize::MAX,
        told: 0,
        after: BTreeSet::new(),
    });
}

/// The start of a step's debug form: its name, up to the values it carries.
struct Head {
    bytes: [u8; 96],
    size: usize,
    open: usize,
}

impl fmt::Write for Head {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        for byte in text.bytes() {
            if byte == b' ' || byte == b'{' || self.size == self.bytes.len() {
                return Err(fmt::Error);
            }
            match byte {
                b'(' => self.open += 1,
                b')' => self.open -= 1,
                _ => {}
            }
            self.bytes[self.size] = byte;
            self.size += 1;
        }
        Ok(())
    }
}

/// The step of `name`, without the values it carries, such as
/// `LabelEnd(Ok)`.
fn plain(name: &Name) -> Head {
    let mut head = Head {
        bytes: [0; 96],
        size: 0,
        open: 0,
    };
    let _ = write!(head, "{:?}", name);
    while head.open > 0 && head.size < head.bytes.len() {
        head.bytes[head.size] = b')';
        head.size += 1;
        head.open -= 1;
    }
    head
}

/// The index of the step of `name` in `seen.steps`.
fn index(seen: &mut Seen, name: &Name) -> usize {
    // Steps repeat: look among the last few first.
    if let Some(at) = seen.recent.iter().position(|(known, _)| known == name) {
        let found = seen.recent[at];
        seen.recent.remove(at);
        seen.recent.push(found);
        return found.1;
    }
    let found = (*name, named(seen, name));
    if seen.recent.len() == 16 {
        seen.recent.remove(0);
    }
    seen.recent.push(found);
    found.1
}

/// The index of the step of `name` in `seen.steps`, by its name.
fn named(seen: &mut Seen, name: &Name) -> usize {
    let head = plain(name);
    let step = core::str::from_utf8(&head.bytes[..head.size]).unwrap_or("");
    if let Some(index) = seen.index.get(step) {
        return *index;
    }
    seen.steps.push(step.into());
    seen.index.insert(step.into(), seen.steps.len() - 1);
    seen.steps.len() - 1
}

/// Record that the step `name` runs, after the step that ran last.
pub fn hit(name: &Name) {
    SEEN.with(|seen| {
        let seen = &mut *seen.borrow_mut();
        let index = index(seen, name);
        seen.after.insert((seen.last, index));
        seen.last = index;
    });
}

/// Write what was recorded since the last call, and clear it: a
/// `last<TAB>step` line for each step after a step (`-` when it ran first),
/// with steps by index, after a `=index<TAB>name` line for each step not
/// written before.
pub fn take(out: &mut String) {
    SEEN.with(|seen| {
        let seen = &mut *seen.borrow_mut();
        seen.last = usize::MAX;
        while seen.told < seen.steps.len() {
            let _ = writeln!(out, "={}\t{}", seen.told, seen.steps[seen.told]);
            seen.told += 1;
        }
        for (last, step) in core::mem::take(&mut seen.after) {
            if last == usize::MAX {
                let _ = writeln!(out, "-\t{}", step);
            } else {
                let _ = writeln!(out, "{}\t{}", last, step);
            }
        }
    });
}
