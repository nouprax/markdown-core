//! Formula occurs in the [text][] and [flow][] content types.
//! It forms formulas (`$a$`, `$$a$$`, `` $`a`$ ``, `\\(a\\)`, `\\[a\\]`) and
//! formula blocks (`$$` and `\\[` fences).
//!
//! ## Grammar
//!
//! Formula forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: `$` is not followed by whitespace.
//! ; Restriction: the body runs to the first closer of its form, which is not
//! ; in a `\` and ASCII punctuation pair; in the `$` form not in a `$$` pair.
//! ; Restriction: the `$` closer is not after whitespace or before a digit.
//! ; Restriction: a `$` body that starts with `` ` `` ends with another one.
//! formula ::= '$' body '$' | '$$' body '$$' | '\\(' body '\\)' | '\\[' body '\\]'
//!
//! ; Restriction: at most 3 spaces of indent before the opening fence.
//! formula_flow ::= fence_open *(eol line) [eol fence_close]
//! fence_open ::= ('$$' | '\\[') *space_or_tab
//! fence_close ::= *space_or_tab ('$$' | '\\]') *space_or_tab
//! ```
//!
//! `$` and `\\(` formulas are embedded in their text, `$$` and `\\[` ones
//! stand alone.
//! The literal of a formula is its body: without the backticks of the
//! `` $`a`$ `` form, with `\)` and `\]` unescaped in the backslash forms, and
//! with one space or line ending off each end when both ends have one and the
//! body is not all of them.
//! The literal of a formula block is its lines, trimmed of whitespace.
//!
//! ## Tokens
//!
//! * [`Formula`][Name::Formula]
//! * [`FormulaFlow`][Name::FormulaFlow]
//! * [`FormulaFence`][Name::FormulaFence]
//! * [`FormulaSequence`][Name::FormulaSequence]
//! * [`FormulaText`][Name::FormulaText]
//! * [`FormulaChunk`][Name::FormulaChunk]
//!
//! [text]: crate::construct::text
//! [flow]: crate::construct::flow

use crate::choice::Starts;
use crate::construct::parts;
use crate::construct::parts::space_or_tab::{space_or_tab, space_or_tab_min_max};
use crate::event::{Kind, Name};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::constant::TAB_SIZE;

/// Bytes a formula can start at.
pub const STARTS: Starts = Starts::Bytes(b"$\\");

/// Bytes a formula block can start at.
pub const FLOW: Starts = Starts::Bytes(b"\t $\\");

/// The delimiters of a formula.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Form {
    /// `$a$`.
    Dollar,
    /// `$$a$$`.
    Dollars,
    /// `\\(a\\)`.
    Round,
    /// `\\[a\\]`.
    Square,
}

impl Form {
    /// The bracket that closes a backslash form.
    fn bracket(self) -> u8 {
        if self == Form::Round {
            b')'
        } else {
            b']'
        }
    }
}

/// What a `$` body has read: whether it starts with a backtick, and its
/// size up to 2.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Seen {
    tick: bool,
    size: u8,
}

impl Seen {
    /// After `byte` of the body.
    fn read(self, byte: Option<u8>) -> Seen {
        Seen {
            tick: if self.size == 0 {
                byte == Some(b'`')
            } else {
                self.tick
            },
            size: if self.size < 2 { self.size + 1 } else { 2 },
        }
    }
}

/// Whether `byte` is whitespace to a `$`.
fn white(byte: Option<u8>) -> bool {
    matches!(byte, Some(b'\t' | b'\n' | b'\r' | b' '))
}

/// Start of a formula.
///
/// ```markdown
/// > | a $b$ c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'$') if dollars(tokenizer) % 2 == 0 => {
            tokenizer.enter(Name::Formula);
            tokenizer.enter(Name::FormulaSequence);
            tokenizer.consume();
            State::Next(StateName::Formula(Step::Dollar))
        }
        Some(b'\\') => {
            tokenizer.enter(Name::Formula);
            tokenizer.enter(Name::FormulaSequence);
            tokenizer.consume();
            State::Next(StateName::Formula(Step::Slash))
        }
        _ => State::Nok,
    }
}

/// How many dollars of text are right before the current byte.
///
/// An opener `$$` with no closer is text whole, as both its dollars: the
/// `$` after it opens nothing (`$$x$`), and the one after that may.
fn dollars(tokenizer: &Tokenizer) -> usize {
    let events = &tokenizer.events;
    let bytes = tokenizer.parse_state.bytes;
    let mut floor = tokenizer.point.index;
    let mut index = events.len();
    while index >= 2
        && events[index - 1].kind == Kind::Exit
        && events[index - 1].name == Name::Data
        && events[index - 1].point.index == floor
        && events[index - 2].kind == Kind::Enter
    {
        floor = events[index - 2].point.index;
        index -= 2;
    }
    let mut at = tokenizer.point.index;
    while at > floor && bytes[at - 1] == b'$' {
        at -= 1;
    }
    tokenizer.point.index - at
}

/// After the first `$`.
///
/// ```markdown
/// > | a $b$ c
///        ^
/// ```
fn dollar(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'$') {
        tokenizer.consume();
        tokenizer.exit(Name::FormulaSequence);
        State::Next(StateName::Formula(Step::Body {
            form: Form::Dollars,
            seen: Seen::default(),
            open: false,
        }))
    } else if tokenizer.current.is_none() || white(tokenizer.current) {
        State::Nok
    } else {
        tokenizer.exit(Name::FormulaSequence);
        State::Redo(StateName::Formula(Step::Body {
            form: Form::Dollar,
            seen: Seen::default(),
            open: false,
        }))
    }
}

/// After the first `\`.
///
/// ```markdown
/// > | a \\(b\\) c
///        ^
/// ```
fn slash(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'\\') {
        tokenizer.consume();
        State::Next(StateName::Formula(Step::Bracket))
    } else {
        State::Nok
    }
}

/// After `\\`, at its bracket.
///
/// ```markdown
/// > | a \\(b\\) c
///         ^
/// ```
fn bracket(tokenizer: &mut Tokenizer) -> State {
    let form = match tokenizer.current {
        Some(b'(') => Form::Round,
        Some(b'[') => Form::Square,
        _ => return State::Nok,
    };
    tokenizer.consume();
    tokenizer.exit(Name::FormulaSequence);
    State::Next(StateName::Formula(Step::Body {
        form,
        seen: Seen::default(),
        open: false,
    }))
}

/// In the body; `open` is whether a text token is open.
///
/// ```markdown
/// > | a $b$ c
///        ^
/// ```
fn body(tokenizer: &mut Tokenizer, form: Form, seen: Seen, open: bool) -> State {
    let special = match tokenizer.current {
        None | Some(b'\n' | b'\\') => true,
        Some(b'$') => matches!(form, Form::Dollar | Form::Dollars),
        _ => false,
    };
    if !special {
        if !open {
            tokenizer.enter(Name::FormulaText);
        }
        let seen = seen.read(tokenizer.current);
        tokenizer.consume();
        return State::Next(StateName::Formula(Step::Body {
            form,
            seen,
            open: true,
        }));
    }
    if open {
        tokenizer.exit(Name::FormulaText);
    }
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnding);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnding);
            State::Next(StateName::Formula(Step::Line {
                form,
                seen: seen.read(Some(b'\n')),
            }))
        }
        Some(b'\\') if matches!(form, Form::Round | Form::Square) => {
            tokenizer.attempt(
                State::Next(StateName::Formula(Step::Done)),
                State::Next(StateName::Formula(Step::Skip { form, seen })),
            );
            State::Redo(StateName::Formula(Step::Close { form, size: 0 }))
        }
        Some(b'$') => {
            // A `$` closer is not after whitespace, and a body that starts
            // with a backtick ends with another one.
            let fits = form == Form::Dollars
                || (!white(tokenizer.previous)
                    && (!seen.tick || (seen.size > 1 && tokenizer.previous == Some(b'`'))));
            if fits {
                tokenizer.attempt(
                    State::Next(StateName::Formula(Step::Done)),
                    State::Next(StateName::Formula(Step::Skip { form, seen })),
                );
                State::Redo(StateName::Formula(Step::Close { form, size: 0 }))
            } else {
                State::Redo(StateName::Formula(Step::Skip { form, seen }))
            }
        }
        _ => State::Redo(StateName::Formula(Step::Skip { form, seen })),
    }
}

/// At the start of a body line, after a line ending.
///
/// ```markdown
///   | a $b
/// > | c$ d
///     ^
/// ```
fn line(tokenizer: &mut Tokenizer, form: Form, seen: Seen) -> State {
    let body = StateName::Formula(Step::Body {
        form,
        seen,
        open: false,
    });
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(State::Next(body), State::Nok);
        State::Redo(space_or_tab())
    } else {
        State::Redo(body)
    }
}

/// At a `$` or `\` that is no closer: body bytes.
///
/// ```markdown
/// > | a $b$1$ c
///          ^
/// ```
fn skip(tokenizer: &mut Tokenizer, form: Form, seen: Seen) -> State {
    tokenizer.enter(Name::FormulaText);
    let marker = tokenizer.current;
    let seen = seen.read(marker);
    tokenizer.consume();
    State::Next(StateName::Formula(Step::Pair { form, seen, marker }))
}

/// After a skipped `$` or `\`: the `$` of a `$$` pair in the `$` form, or
/// the punctuation after a `\`, belongs with it.
///
/// ```markdown
/// > | a $b\$c$ d
///          ^
/// ```
fn pair(tokenizer: &mut Tokenizer, form: Form, seen: Seen, marker: Option<u8>) -> State {
    let held = match (marker, tokenizer.current) {
        (Some(b'$'), Some(b'$')) => form == Form::Dollar,
        (Some(b'\\'), Some(byte)) => byte.is_ascii_punctuation(),
        _ => false,
    };
    if held {
        let seen = seen.read(tokenizer.current);
        tokenizer.consume();
        State::Next(StateName::Formula(Step::Body {
            form,
            seen,
            open: true,
        }))
    } else {
        State::Redo(StateName::Formula(Step::Body {
            form,
            seen,
            open: true,
        }))
    }
}

/// In a closer; `size` is how much of it is read.
///
/// ```markdown
/// > | a $b$ c
///         ^
/// ```
fn close(tokenizer: &mut Tokenizer, form: Form, size: usize) -> State {
    if size == 0 {
        tokenizer.enter(Name::FormulaSequence);
    }
    let expected = match form {
        Form::Dollar => {
            if size == 1 {
                // Not a `$$` pair, and not before a digit.
                return if matches!(tokenizer.current, Some(b'$' | b'0'..=b'9')) {
                    State::Nok
                } else {
                    tokenizer.exit(Name::FormulaSequence);
                    State::Ok
                };
            }
            b'$'
        }
        Form::Dollars => b'$',
        Form::Round | Form::Square => {
            if size < 2 {
                b'\\'
            } else {
                form.bracket()
            }
        }
    };
    let width = if form == Form::Dollar {
        1
    } else if form == Form::Dollars {
        2
    } else {
        3
    };
    if size == width && form != Form::Dollar {
        tokenizer.exit(Name::FormulaSequence);
        State::Ok
    } else if tokenizer.current == Some(expected) {
        tokenizer.consume();
        State::Next(StateName::Formula(Step::Close {
            form,
            size: size + 1,
        }))
    } else {
        State::Nok
    }
}

/// After the closer.
///
/// ```markdown
/// > | a $b$ c
///          ^
/// ```
fn done(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::Formula);
    State::Ok
}

/// Start of a formula block.
///
/// ```markdown
/// > | $$
///     ^
///   | a
///   | $$
/// ```
fn flow_start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::FormulaFlow);
    tokenizer.attempt(State::Next(StateName::Formula(Step::FlowOpen)), State::Nok);
    State::Redo(StateName::Formula(Step::FenceStart {
        form: None,
        indent: TAB_SIZE - 1,
    }))
}

/// After the opening fence, at its line ending.
///
/// ```markdown
/// > | $$
///       ^
///   | a
///   | $$
/// ```
fn flow_open(tokenizer: &mut Tokenizer) -> State {
    let form = fence_form(tokenizer);
    // Do not form containers.
    tokenizer.concrete = true;
    State::Redo(StateName::Formula(Step::FlowLine { form }))
}

/// The form of the fence that the events end with.
fn fence_form(tokenizer: &Tokenizer) -> Form {
    let last = tokenizer
        .events
        .iter()
        .rev()
        .find(|event| event.name == Name::FormulaSequence)
        .expect("expected a fence sequence");
    // The exit of `$$` is after a `$`, that of `\\[` after a `[`.
    if tokenizer.parse_state.bytes[last.point.index - 1] == b'$' {
        Form::Dollars
    } else {
        Form::Square
    }
}

/// At the end of a line in the block: a line ending that the block goes on
/// past, or its end.
///
/// ```markdown
/// > | $$
///       ^
/// > | a
///      ^
///   | $$
/// ```
fn flow_line(tokenizer: &mut Tokenizer, form: Form) -> State {
    match tokenizer.current {
        None => State::Redo(StateName::Formula(Step::FlowAfter)),
        Some(b'\n') => {
            tokenizer.check(
                State::Next(StateName::Formula(Step::FlowBreak { form })),
                State::Next(StateName::Formula(Step::FlowAfter)),
            );
            State::Redo(StateName::NonLazyContinuation(
                parts::non_lazy_continuation::Step::Start,
            ))
        }
        _ => {
            tokenizer.enter(Name::FormulaChunk);
            State::Redo(StateName::Formula(Step::FlowChunk { form }))
        }
    }
}

/// At a line ending in the block, before a closing fence or a body line.
///
/// ```markdown
/// > | $$
///       ^
///   | a
///   | $$
/// ```
fn flow_break(tokenizer: &mut Tokenizer, form: Form) -> State {
    tokenizer.attempt(
        State::Next(StateName::Formula(Step::FlowAfter)),
        State::Next(StateName::Formula(Step::FlowEol { form })),
    );
    tokenizer.enter(Name::LineEnding);
    tokenizer.consume();
    tokenizer.exit(Name::LineEnding);
    State::Next(StateName::Formula(Step::FenceStart {
        form: Some(form),
        indent: usize::MAX,
    }))
}

/// At a line ending, before a body line.
///
/// ```markdown
/// > | $$
///       ^
///   | a
///   | $$
/// ```
fn flow_eol(tokenizer: &mut Tokenizer, form: Form) -> State {
    tokenizer.enter(Name::LineEnding);
    tokenizer.consume();
    tokenizer.exit(Name::LineEnding);
    State::Next(StateName::Formula(Step::FlowLine { form }))
}

/// In a body line.
///
/// ```markdown
///   | $$
/// > | a
///     ^
///   | $$
/// ```
fn flow_chunk(tokenizer: &mut Tokenizer, form: Form) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) {
        tokenizer.exit(Name::FormulaChunk);
        State::Redo(StateName::Formula(Step::FlowLine { form }))
    } else {
        tokenizer.consume();
        State::Next(StateName::Formula(Step::FlowChunk { form }))
    }
}

/// After the block.
///
/// ```markdown
///   | $$
///   | a
/// > | $$
///       ^
/// ```
fn flow_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::FormulaFlow);
    // No longer concrete.
    tokenizer.concrete = false;
    // Feel free to interrupt.
    tokenizer.interrupt = false;
    State::Ok
}

/// Start of a fence line; `form` is that of the opening fence for a
/// closing one, and `indent` the most indent it can have.
///
/// ```markdown
/// > | $$
///     ^
/// ```
fn fence_start(tokenizer: &mut Tokenizer, form: Option<Form>, indent: usize) -> State {
    tokenizer.enter(Name::FormulaFence);
    let next = State::Next(StateName::Formula(Step::FenceBefore { form }));
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(next, State::Nok);
        State::Redo(space_or_tab_min_max(0, indent))
    } else {
        State::Redo(StateName::Formula(Step::FenceBefore { form }))
    }
}

/// In a fence line, at its sequence.
///
/// ```markdown
/// > | $$
///     ^
/// ```
fn fence_before(tokenizer: &mut Tokenizer, form: Option<Form>) -> State {
    let found = match tokenizer.current {
        Some(b'$') => Form::Dollars,
        Some(b'\\') => Form::Square,
        _ => return State::Nok,
    };
    if form.map_or(false, |form| form != found) {
        return State::Nok;
    }
    tokenizer.enter(Name::FormulaSequence);
    tokenizer.consume();
    State::Next(StateName::Formula(Step::FenceSequence {
        form: found,
        size: 1,
        open: form.is_none(),
    }))
}

/// In a fence sequence; `size` is how much of it is read.
///
/// ```markdown
/// > | \\[
///      ^
/// ```
fn fence_sequence(tokenizer: &mut Tokenizer, form: Form, size: usize, open: bool) -> State {
    let (width, expected) = match form {
        Form::Dollars => (2, b'$'),
        _ => (
            3,
            if size < 2 {
                b'\\'
            } else if open {
                b'['
            } else {
                b']'
            },
        ),
    };
    if size == width {
        tokenizer.exit(Name::FormulaSequence);
        if matches!(tokenizer.current, Some(b'\t' | b' ')) {
            tokenizer.attempt(
                State::Next(StateName::Formula(Step::FenceAfter)),
                State::Nok,
            );
            State::Redo(space_or_tab())
        } else {
            State::Redo(StateName::Formula(Step::FenceAfter))
        }
    } else if tokenizer.current == Some(expected) {
        tokenizer.consume();
        State::Next(StateName::Formula(Step::FenceSequence {
            form,
            size: size + 1,
            open,
        }))
    } else {
        State::Nok
    }
}

/// In a fence line, after its sequence and whitespace.
///
/// ```markdown
/// > | $$
///       ^
/// ```
fn fence_after(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, None | Some(b'\n')) {
        tokenizer.exit(Name::FormulaFence);
        State::Ok
    } else {
        State::Nok
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Dollar,
    Slash,
    Bracket,
    Body {
        form: Form,
        seen: Seen,
        open: bool,
    },
    Line {
        form: Form,
        seen: Seen,
    },
    Skip {
        form: Form,
        seen: Seen,
    },
    Pair {
        form: Form,
        seen: Seen,
        marker: Option<u8>,
    },
    Close {
        form: Form,
        size: usize,
    },
    Done,
    FlowStart,
    FlowOpen,
    FlowLine {
        form: Form,
    },
    FlowBreak {
        form: Form,
    },
    FlowEol {
        form: Form,
    },
    FlowChunk {
        form: Form,
    },
    FlowAfter,
    FenceStart {
        form: Option<Form>,
        indent: usize,
    },
    FenceBefore {
        form: Option<Form>,
    },
    FenceSequence {
        form: Form,
        size: usize,
        open: bool,
    },
    FenceAfter,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Formula(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Dollar => dollar(tokenizer),
        Step::Slash => slash(tokenizer),
        Step::Bracket => bracket(tokenizer),
        Step::Body { form, seen, open } => body(tokenizer, form, seen, open),
        Step::Line { form, seen } => line(tokenizer, form, seen),
        Step::Skip { form, seen } => skip(tokenizer, form, seen),
        Step::Pair { form, seen, marker } => pair(tokenizer, form, seen, marker),
        Step::Close { form, size } => close(tokenizer, form, size),
        Step::Done => done(tokenizer),
        Step::FlowStart => flow_start(tokenizer),
        Step::FlowOpen => flow_open(tokenizer),
        Step::FlowLine { form } => flow_line(tokenizer, form),
        Step::FlowBreak { form } => flow_break(tokenizer, form),
        Step::FlowEol { form } => flow_eol(tokenizer, form),
        Step::FlowChunk { form } => flow_chunk(tokenizer, form),
        Step::FlowAfter => flow_after(tokenizer),
        Step::FenceStart { form, indent } => fence_start(tokenizer, form, indent),
        Step::FenceBefore { form } => fence_before(tokenizer, form),
        Step::FenceSequence { form, size, open } => fence_sequence(tokenizer, form, size, open),
        Step::FenceAfter => fence_after(tokenizer),
    }
}
