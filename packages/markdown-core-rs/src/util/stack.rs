//! Room on the call stack for the parser's recursion.
//!
//! Nesting in the input (quotes, lists, emphasis, brackets, notes, values in
//! the properties envelope) is recursion in the parser, whose depth the input
//! sets. Each function that recurses once per level runs its level in
//! [`room`][], which moves onto a new piece of stack, on the heap, when
//! less than [`RED`][] is left: no input overflows the stack, whatever stack
//! the thread has.

/// The stack a level may use before the next one asks for room again.
pub const RED: usize = 128 * 1024;

/// The size of each new piece of stack.
pub const PIECE: usize = 2 * 1024 * 1024;

/// Run `level` with at least [`RED`][] of stack left.
#[inline]
pub fn room<R>(level: impl FnOnce() -> R) -> R {
    stacker::maybe_grow(RED, PIECE, level)
}
