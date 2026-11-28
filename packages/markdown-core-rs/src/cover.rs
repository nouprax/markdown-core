//! Coverage of the state machine: which steps ran after which, and at which
//! bytes. Each step decides its successor, so the pairs are the branches of
//! the machine; the byte a step runs at is the value it decides on.
//!
//! With `scripts/correctness/cover.mjs`, which builds the crate with
//! LLVM's coverage guards (`-sanitizer-coverage-trace-pc-guard`), also the
//! edges of the code: every branch of every function, outside this module.
//!
//! Only with the `cover` feature, for the coverage tools in
//! `scripts/correctness`.

use crate::state::Name;
use core::cell::RefCell;
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::collections::{BTreeMap, BTreeSet};

/// What coverage has seen: each step once, by name and in order, the step
/// that ran last, the steps after steps, as indices into `steps`, and the
/// steps at bytes (256 for the end).
struct Seen {
    recent: Vec<(Name, usize)>,
    index: BTreeMap<String, usize>,
    steps: Vec<String>,
    last: usize,
    told: usize,
    after: BTreeSet<(usize, usize)>,
    at: BTreeSet<(usize, u16)>,
}

std::thread_local! {
    static SEEN: RefCell<Seen> = const { RefCell::new(Seen {
        recent: Vec::new(),
        index: BTreeMap::new(),
        steps: Vec::new(),
        last: usize::MAX,
        told: 0,
        after: BTreeSet::new(),
        at: BTreeSet::new(),
    }) };
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
    let _ = write!(head, "{name:?}");
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

/// Record that the step `name` runs at `current`, after the step that ran
/// last. Never inlined, so that its code is no code of the parser's.
#[inline(never)]
pub fn hit(name: &Name, current: Option<u8>) {
    QUIET.store(true, Ordering::Relaxed);
    SEEN.with(|seen| {
        let seen = &mut *seen.borrow_mut();
        let index = index(seen, name);
        seen.after.insert((seen.last, index));
        seen.at.insert((index, current.map_or(256, u16::from)));
        seen.last = index;
    });
    QUIET.store(false, Ordering::Relaxed);
}

/// Write what was recorded since the last call, and clear it: a
/// `last<TAB>step` line for each step after a step (`-` when it ran first),
/// and a `@step<TAB>byte` line for each step at a byte (256 for the end),
/// with steps by index, after a `=index<TAB>name` line for each step not
/// written before; then a `!number` line for each coverage guard that ran.
#[inline(never)]
pub fn take(out: &mut String) {
    QUIET.store(true, Ordering::Relaxed);
    SEEN.with(|seen| {
        let seen = &mut *seen.borrow_mut();
        seen.last = usize::MAX;
        while seen.told < seen.steps.len() {
            let _ = writeln!(out, "={}\t{}", seen.told, seen.steps[seen.told]);
            seen.told += 1;
        }
        for (last, step) in core::mem::take(&mut seen.after) {
            if last == usize::MAX {
                let _ = writeln!(out, "-\t{step}");
            } else {
                let _ = writeln!(out, "{last}\t{step}");
            }
        }
        for (step, byte) in core::mem::take(&mut seen.at) {
            let _ = writeln!(out, "@{step}\t{byte}");
        }
        let count = COUNT.load(Ordering::Relaxed).min(MOST);
        for (slot, number) in RAN.iter().zip(NUMBERS.iter()).take(count) {
            let number = number.load(Ordering::Relaxed);
            // Armed again, for the next input.
            unsafe { *(slot.load(Ordering::Relaxed) as *mut u32) = number };
            let _ = writeln!(out, "!{number}");
        }
        COUNT.store(0, Ordering::Relaxed);
    });
    QUIET.store(false, Ordering::Relaxed);
}

/// The most coverage guards an input can run.
const MOST: usize = 1 << 16;
/// The coverage guards numbered so far.
static NUMBERED: AtomicU32 = AtomicU32::new(0);
/// The guards that ran since the last `take`, as addresses, and their
/// numbers; a guard that ran is disarmed (0) until then.
#[allow(clippy::declare_interior_mutable_const)]
const NONE: AtomicUsize = AtomicUsize::new(0);
#[allow(clippy::declare_interior_mutable_const)]
const ZERO: AtomicU32 = AtomicU32::new(0);
static RAN: [AtomicUsize; MOST] = [NONE; MOST];
static NUMBERS: [AtomicU32; MOST] = [ZERO; MOST];
static COUNT: AtomicUsize = AtomicUsize::new(0);
/// Whether the code running is this module's, whose guards are no guards of
/// the parser's.
static QUIET: AtomicBool = AtomicBool::new(false);

/// Number the coverage guards of a module, from 1, after those of the
/// modules before.
///
/// # Safety
///
/// LLVM calls it with the bounds of a module's guards.
#[no_mangle]
pub unsafe extern "C" fn __sanitizer_cov_trace_pc_guard_init(start: *mut u32, stop: *mut u32) {
    if start == stop || *start != 0 {
        return;
    }
    let mut slot = start;
    while slot < stop {
        *slot = NUMBERED.fetch_add(1, Ordering::Relaxed) + 1;
        slot = slot.add(1);
    }
}

/// Record that the code of the guard at `slot` ran, and disarm it.
///
/// # Safety
///
/// LLVM calls it with a slot that the function above numbered.
#[no_mangle]
pub unsafe extern "C" fn __sanitizer_cov_trace_pc_guard(slot: *mut u32) {
    let number = *slot;
    if number == 0 || QUIET.load(Ordering::Relaxed) {
        return;
    }
    *slot = 0;
    let at = COUNT.fetch_add(1, Ordering::Relaxed);
    if at < MOST {
        RAN[at].store(slot as usize, Ordering::Relaxed);
        NUMBERS[at].store(number, Ordering::Relaxed);
    }
}
