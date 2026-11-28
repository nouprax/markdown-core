//! A tokenizer glues states from the state machine together.
//!
//! It facilitates everything needed to turn bytes into events with a state
//! machine.
//! It also enables the logic needed for parsing markdown, such as an
//! [`attempt`][] to try and parse something, which can succeed or, when
//! unsuccessful, revert the attempt.
//!
//! [`attempt`]: Tokenizer::attempt

use crate::event::{Event, Kind, Link, Name, Point, VOID_EVENTS};
use crate::parser::ParseState;
use crate::resolve::{call as call_resolve, Name as ResolveName};
use crate::state::{call, Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::util::stack::room;

use crate::util::{constant::TAB_SIZE, edit_map::EditMap};
use std::collections::BTreeMap;

/// Containers.
///
/// Containers are found when tokenizing
/// [document content][crate::construct::document].
/// They parse a portion at the start of one or more lines.
/// The rest of those lines is a different content type (specifically, flow),
/// which they “contain”.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Container {
    /// [Block quote][crate::construct::block_quote].
    BlockQuote,
    /// [List item][crate::construct::list_item].
    ListItem,
    /// [GFM: Footnote definition][crate::construct::footnote_definition].
    FootnoteDefinition,
    /// [Directive (container)][crate::construct::directive_container].
    Directive,
    /// [Definition body][crate::construct::definition_body].
    DefinitionBody,
}

/// Info used to tokenize a container.
///
/// Practically, these fields are only used for list items.
#[derive(Clone, Copy, Debug)]
pub struct ContainerState {
    /// Kind.
    pub kind: Container,
    /// Whether the first line was blank.
    pub blank_initial: bool,
    /// Size.
    pub size: usize,
    /// Whether the closing line was seen, after which the container ends.
    pub closed: bool,
}

/// How to handle a byte.
#[derive(Debug, PartialEq)]
enum ByteAction {
    /// This is a normal byte.
    ///
    /// Includes replaced bytes.
    Normal(u8),
    /// This byte must be ignored.
    Ignore,
    /// This is a new byte.
    Insert(u8),
}

/// Label start kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LabelKind {
    /// Label (image) start.
    ///
    /// ```markdown
    /// > | a ![b] c
    ///       ^^
    /// ```
    ///
    /// Construct: [Label start (image)][crate::construct::label_start_image].
    Image,
    /// Label (image) link.
    ///
    /// ```markdown
    /// > | a [b] c
    ///       ^
    /// ```
    ///
    /// Construct: [Label start (link)][crate::construct::label_start_link].
    Link,
    /// GFM: Label (footnote) link.
    ///
    /// ```markdown
    /// > | a [^b] c
    ///       ^^
    /// ```
    ///
    /// Construct: [GFM: Label start (footnote)][crate::construct::label_start_footnote].
    Footnote,
    /// GFM: Label (footnote) link, not matching a footnote definition, so
    /// handled as a label (link) start.
    ///
    /// ```markdown
    /// > | a [^b](c) d
    ///       ^^
    /// ```
    ///
    /// Construct: [Label end][crate::construct::label_end].
    UndefinedFootnote,
    /// Label (note) start.
    ///
    /// ```markdown
    /// > | a ^[b] c
    ///       ^^
    /// ```
    ///
    /// Construct: [Label start (note)][crate::construct::label_start_note].
    Note,
    /// Label (link) start that attributes follow, as a bracketed span.
    ///
    /// ```markdown
    /// > | a [b]{.c} d
    ///       ^
    /// ```
    ///
    /// Construct: [Label end][crate::construct::label_end].
    Span,
    /// Label (link) start whose sections each hold a citation key, as a
    /// citation group.
    ///
    /// ```markdown
    /// > | a [see @b; @c] d
    ///       ^
    /// ```
    ///
    /// Construct: [Label end][crate::construct::label_end].
    Group,
    /// Label (link) start right after a citation key, as the key’s tail;
    /// `fallback` is what it is when the key turns out to be the referent of
    /// a group around it.
    ///
    /// ```markdown
    /// > | a @b [p. 1] c
    ///          ^
    /// ```
    ///
    /// Construct: [Label end][crate::construct::label_end].
    Tail {
        /// Enter index of the key.
        author: usize,
        /// What the bracket is without the key.
        fallback: Fallback,
    },
}

/// What a citation tail is when its key is the referent of a group.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fallback {
    /// A shortcut reference.
    Link,
    /// A citation group.
    Group,
    /// Brackets, as text.
    Data,
}

/// Label start, looking for an end.
#[derive(Debug)]
pub struct LabelStart {
    /// Kind of start.
    pub kind: LabelKind,
    /// Indices of where the label starts and ends in `events`.
    pub start: (usize, usize),
    /// A boolean used internally to figure out if a (link) label start can’t
    /// be used anymore (because it would contain another link).
    /// That link start is still looking for a balanced closing bracket though,
    /// so we can’t remove it just yet.
    pub inactive: bool,
}

/// Valid label.
#[derive(Debug)]
pub struct Label {
    pub kind: LabelKind,
    /// Indices of label start.
    pub start: (usize, usize),
    /// Indices of label end.
    pub end: (usize, usize),
}

/// Different kinds of attempts.
#[derive(Debug, PartialEq)]
enum AttemptKind {
    /// Discard what was tokenized when unsuccessful.
    Attempt,
    /// Discard always.
    Check,
}

/// How to handle [`State::Ok`][] or [`State::Nok`][].
#[derive(Debug)]
struct Attempt {
    /// Where to go to when successful.
    ok: State,
    /// Where to go to when unsuccessful.
    nok: State,
    /// Kind of attempt.
    kind: AttemptKind,
    /// If needed, the progress to revert to.
    ///
    /// It is not needed to discard an [`AttemptKind::Attempt`] that has a
    /// `nok` of [`State::Nok`][], because that means it is used in *another*
    /// attempt, which will receive that `Nok`, and has to handle it.
    spot: Option<Spot>,
}

/// The internal state of a tokenizer.
///
/// Not to be confused with states from the state machine, this instead is all
/// the information on where we currently are and what’s going on.
#[derive(Clone, Debug)]
struct Spot {
    /// Length of `events`.
    ///
    /// It’s not allowed to remove events, so reverting will just pop stuff off.
    events_len: usize,
    /// Length of the stack.
    ///
    /// It’s not allowed to decrease the stack in an attempt.
    stack_len: usize,
    /// Previous code.
    previous: Option<u8>,
    /// Current code.
    current: Option<u8>,
    /// Current place in the file.
    point: Point,
    /// Where the current line starts, past its line ending.
    line_start: Point,
}

/// Container state of the document tokenizer.
#[derive(Debug, Default)]
pub struct Document<'a> {
    /// Tokenizer of the flow inside the containers.
    pub child: Option<Box<Tokenizer<'a>>>,
    /// State of the child tokenizer.
    pub state: Option<State>,
    /// Stack of currently open containers.
    pub stack: Vec<ContainerState>,
    /// How many open containers continued on this line.
    pub continued: usize,
    /// The open directive (container) whose closing fence this line is, by
    /// its index in `stack`: it ends unless a container inside it goes on
    /// on the line, or flow that is concrete.
    pub closer: Option<usize>,
    /// Index of the last `data`.
    pub data: Option<usize>,
    /// Container exits by line number.
    pub exits: Vec<Option<Vec<Event>>>,
    /// Whether the previous flow was a paragraph or a definition, so a lazy
    /// line can continue it.
    pub open: bool,
}

/// State of a lookahead over later lines ([`ahead`][crate::ahead]).
#[derive(Debug, Default)]
pub struct Ahead {
    /// Containers around the flow of the current line, set by the document
    /// before it feeds the line to its flow.
    pub stack: Vec<ContainerState>,
    /// State a later line is tested with, in a lookahead tokenizer.
    pub test: Option<StateName>,
    /// Whether a later line passed the test, in a lookahead tokenizer.
    pub found: bool,
    /// How many more lines a lookahead tokenizer may try.
    pub lines: usize,
}

/// Label starts and matched labels of text.
#[derive(Debug, Default)]
pub struct Labels {
    /// Usable label starts.
    pub starts: Vec<LabelStart>,
    /// Unusable label starts.
    pub loose: Vec<LabelStart>,
    /// Matched images, links, and footnote calls.
    pub pairs: Vec<Label>,
    /// Brackets whose `]` matched nothing, in the order they close.
    pub shut: Vec<Shut>,
    /// The event index and byte index of the `]` tried last.
    pub close: (usize, usize),
}

/// A bracket whose `]` matched nothing.
///
/// Its `]` starts the data at `end`, after the attempt at it is undone.
#[derive(Debug)]
pub struct Shut {
    /// Enter index of its start.
    pub start: usize,
    /// Event index of the data that starts with its `]`.
    pub end: usize,
    /// Byte index of its `]`.
    pub byte: usize,
}

/// Normalized identifiers defined so far.
#[derive(Debug, Default)]
pub struct Definitions {
    /// Link definitions.
    pub links: Vec<String>,
    /// GFM footnote definitions.
    pub notes: Vec<String>,
}

impl Definitions {
    /// Move all identifiers of `other` into `self`.
    pub fn append(&mut self, other: &mut Definitions) {
        self.notes.append(&mut other.notes);
        self.links.append(&mut other.links);
    }
}

/// A tokenizer itself.
#[allow(clippy::struct_excessive_bools)]
#[derive(Debug)]
pub struct Tokenizer<'a> {
    /// Jump between line endings.
    column_start: Vec<(usize, usize)>,
    // First line where this tokenizer starts.
    first_line: usize,
    /// Current point after the last line ending (excluding jump).
    line_start: Point,
    /// Track whether the current byte is already consumed (`true`) or expected
    /// to be consumed (`false`).
    ///
    /// Tracked to make sure everything’s valid.
    consumed: bool,
    /// Stack of how to handle attempts.
    attempts: Vec<Attempt>,
    /// Current byte.
    pub current: Option<u8>,
    /// Previous byte.
    pub previous: Option<u8>,
    /// Current relative and absolute place in the file.
    pub point: Point,
    /// Semantic labels.
    pub events: Vec<Event>,
    /// Hierarchy of semantic labels.
    ///
    /// Tracked to make sure everything’s valid.
    pub stack: Vec<Name>,
    /// Edit map, to batch changes.
    pub map: EditMap,
    /// List of resolvers.
    pub resolvers: Vec<ResolveName>,
    /// Shared parsing state across tokenizers.
    pub parse_state: &'a ParseState<'a>,
    /// Container state, used by the document tokenizer.
    pub document: Document<'a>,
    /// Lookahead state, used by the flow tokenizer.
    pub ahead: Ahead,
    /// Label state, used by the text tokenizer.
    pub labels: Labels,
    /// Identifiers this tokenizer defined.
    pub definitions: Definitions,
    /// Whether we would be interrupting something.
    ///
    /// Used when tokenizing [flow content][crate::construct::flow].
    pub interrupt: bool,
    /// Whether containers cannot “pierce” into the current construct.
    ///
    /// Used when tokenizing [document content][crate::construct::document].
    pub concrete: bool,
    /// Whether this row is piercing into the current construct with more
    /// containers.
    ///
    /// Used when tokenizing [document content][crate::construct::document].
    pub pierce: bool,
    /// Whether this line is lazy: there are less containers than before.
    pub lazy: bool,
    /// Whether the current line closes a container of the flow, so that
    /// nothing on it is the flow’s but its line ending.
    pub closing: bool,
    /// The line a definition marker is due on, after a
    /// [definition term][crate::construct::definition_term]: a definition
    /// body opens there.
    ///
    /// Used when tokenizing [document content][crate::construct::document].
    pub term: Option<usize>,
    /// An index a construct's check leaves for the attempt after it: where
    /// the check found its construct ends.
    pub mark: usize,
    /// Whether this is the text of a table cell, where each `\\|` is a `|`
    /// before the text is read.
    pub cell: bool,
    /// Per thing a construct scans for, by a key the construct chooses, the
    /// places a scan for it can start and find none, and where those scans
    /// stop: a construct whose scan does not depend on where it starts reads
    /// a stretch once, not once per start in it.
    barren: BTreeMap<u16, (usize, usize)>,
    /// Where the scan of the construct that uses `barren` started.
    scan: usize,
    /// The last run of spaces and tabs [`Tokenizer::blank`][] read: where
    /// it was asked from and where it ends.
    run: Option<(usize, usize)>,
    /// Where the bytes fed so far end, and whether no more follow.
    fed: (usize, bool),
    /// How many checks are open: what is read then is undone whatever it
    /// finds.
    checks: usize,
}

impl<'a> Tokenizer<'a> {
    /// Create a new tokenizer.
    pub fn new(point: Point, parse_state: &'a ParseState) -> Tokenizer<'a> {
        Tokenizer {
            previous: None,
            current: None,
            // To do: reserve size when feeding?
            column_start: vec![],
            first_line: point.line,
            line_start: point.clone(),
            consumed: true,
            attempts: vec![],
            point,
            stack: vec![],
            events: vec![],
            parse_state,
            document: Document::default(),
            ahead: Ahead::default(),
            labels: Labels::default(),
            definitions: Definitions::default(),
            map: EditMap::new(),
            barren: BTreeMap::new(),
            scan: 0,
            run: None,
            fed: (0, false),
            checks: 0,
            interrupt: false,
            pierce: false,
            concrete: false,
            lazy: false,
            closing: false,
            term: None,
            mark: 0,
            cell: false,
            resolvers: vec![],
        }
    }

    /// Start a scan for `key` here: whether it finds none, as a scan for it
    /// from here or earlier that stopped past here did.
    pub fn scan(&mut self, key: u16) -> bool {
        self.scan = self.point.index;
        self.barren
            .get(&key)
            .is_some_and(|(from, to)| *from <= self.point.index && self.point.index < *to)
    }

    /// Whether the rest of the line is blank: spaces and tabs, then a line
    /// ending or the end; `None` when the bytes fed so far do not tell. A run
    /// is read once, however many containers ask from in it.
    pub fn blank(&mut self) -> Option<bool> {
        let bytes = self.parse_state.bytes;
        let (fed, last) = self.fed;
        let at = self.point.index;
        let end = match self.run {
            Some((from, to)) if from <= at && at <= to => to,
            _ => {
                let mut end = at;
                while end < fed && matches!(bytes[end], b'\t' | b' ') {
                    end += 1;
                }
                self.run = Some((at, end));
                end
            }
        };
        if end < fed {
            Some(matches!(bytes[end], b'\n' | b'\r'))
        } else if last {
            Some(true)
        } else {
            None
        }
    }

    /// Whether a check is open, so that what is read now is undone.
    pub fn tentative(&self) -> bool {
        self.checks > 0
    }

    /// The scan for `key` found none, here: a scan for it from where it
    /// started up to here finds none too.
    pub fn barren(&mut self, key: u16) {
        let to = self.point.index;
        let from = match self.barren.get(&key) {
            Some((from, end)) if *end == to => (*from).min(self.scan),
            _ => self.scan,
        };
        self.barren.insert(key, (from, to));
    }

    /// Register a resolver.
    pub fn register_resolver(&mut self, name: ResolveName) {
        if !self.resolvers.contains(&name) {
            self.resolvers.push(name);
        }
    }

    /// Whether the text is right after the opener of an inline note.
    pub fn at_note_start(&self) -> bool {
        matches!(
            self.labels.starts.last(),
            Some(start) if start.kind == LabelKind::Note
                && self.events[start.start.1].point.index == self.point.index
        )
    }

    /// Whether the text is in the label of a link, image, or footnote call
    /// that is still open: a label start after the innermost note start.
    pub fn in_label(&self) -> bool {
        self.labels
            .starts
            .iter()
            .rev()
            .take_while(|start| start.kind != LabelKind::Note)
            .next()
            .is_some()
    }

    /// Register a resolver, before others.
    pub fn register_resolver_before(&mut self, name: ResolveName) {
        if !self.resolvers.contains(&name) {
            self.resolvers.insert(0, name);
        }
    }

    /// Define a jump between two places.
    ///
    /// This defines to which future index we move after a line ending.
    pub fn define_skip(&mut self, mut point: Point) {
        move_point_back(self, &mut point);

        let at = point.line - self.first_line;
        self.line_set(at, (point.index, point.vs));

        // A tokenizer first fed at a later line moves to it.
        if self.point.line < point.line {
            self.move_to((point.index, point.vs));
        } else {
            self.account_for_potential_skip();
        }
    }

    /// Set where the line `at` lines after the first starts.
    ///
    /// A line is at its offset from the first, also when the lines before
    /// it were never started (as a tokenizer fed from a later line, or one
    /// no skip is defined in, which reverts across lines): those hold a
    /// start no move goes back to.
    fn line_set(&mut self, at: usize, info: (usize, usize)) {
        if at < self.column_start.len() {
            self.column_start[at] = info;
        } else {
            self.column_start.resize(at, (0, 0));
            self.column_start.push(info);
        }
    }

    /// Increment the current positional info if we’re right after a line
    /// ending, which has a skip defined.
    fn account_for_potential_skip(&mut self) {
        let at = self.point.line - self.first_line;

        if self.point.column == 1 && at != self.column_start.len() {
            self.move_to(self.column_start[at]);
        }
    }

    /// Prepare for a next byte to get consumed.
    fn expect(&mut self, byte: Option<u8>) {
        debug_assert!(self.consumed, "expected previous byte to be consumed");
        self.consumed = false;
        self.current = byte;
    }

    /// Consume the current byte.
    /// Each state function is expected to call this to signal that this code is
    /// used, or call a next function.
    pub fn consume(&mut self) {
        debug_assert!(!self.consumed, "expected code to *not* have been consumed: this might be because `State::Retry(x)` instead of `State::Next(x)` was returned");
        self.move_one();

        self.previous = self.current;
        // While we’re not at eof, it is at least better to not have the
        // same current code as `previous` *and* `current`.
        self.current = None;
        // Mark as consumed.
        self.consumed = true;
    }

    /// Move to the next (virtual) byte.
    fn move_one(&mut self) {
        match byte_action(self.parse_state.bytes, &self.point) {
            ByteAction::Ignore => {
                self.point.index += 1;
            }
            ByteAction::Insert(byte) => {
                self.previous = Some(byte);
                self.point.column += 1;
                self.point.vs += 1;
            }
            ByteAction::Normal(byte) => {
                self.previous = Some(byte);
                self.point.vs = 0;
                self.point.index += 1;

                if byte == b'\n' {
                    self.point.line += 1;
                    self.point.column = 1;

                    let at = self.point.line - self.first_line;
                    if at >= self.column_start.len() {
                        self.line_set(at, (self.point.index, self.point.vs));
                    }

                    self.line_start = self.point.clone();

                    self.account_for_potential_skip();
                } else if !continues(byte) {
                    self.point.column += 1;
                }
            }
        }
    }

    /// Move (virtual) bytes.
    fn move_to(&mut self, to: (usize, usize)) {
        let (to_index, to_vs) = to;
        while self.point.index < to_index || self.point.index == to_index && self.point.vs < to_vs {
            self.move_one();
        }
    }

    /// Mark the start of a semantic label.
    pub fn enter(&mut self, name: Name) {
        enter_impl(self, name, None);
    }

    /// Enter with a link.
    pub fn enter_link(&mut self, name: Name, link: Link) {
        enter_impl(self, name, Some(link));
    }

    /// Mark the end of a semantic label.
    pub fn exit(&mut self, name: Name) {
        let current = self.stack.pop().expect("cannot close w/o open tokens");

        debug_assert_eq!(current, name, "expected exit event to match current event");

        let previous = self.events.last().expect("cannot close w/o open event");
        let mut point = self.point.clone();

        debug_assert!(
            current != previous.name
                || previous.point.index != point.index
                || previous.point.vs != point.vs,
            "expected non-empty event"
        );

        if VOID_EVENTS.iter().any(|d| d == &name) {
            debug_assert!(
                current == previous.name,
                "expected event to be void, instead of including something"
            );
        }

        // A bit weird, but if we exit right after a line ending, we *don’t* want to consider
        // potential skips.
        if matches!(self.previous, Some(b'\n')) {
            point = self.line_start.clone();
        } else {
            move_point_back(self, &mut point);
        }

        let event = Event {
            kind: Kind::Exit,
            name,
            point,
            link: None,
        };
        self.events.push(event);
    }

    /// Capture the tokenizer progress.
    fn capture(&mut self) -> Spot {
        Spot {
            previous: self.previous,
            current: self.current,
            point: self.point.clone(),
            line_start: self.line_start.clone(),
            events_len: self.events.len(),
            stack_len: self.stack.len(),
        }
    }

    /// Apply tokenizer progress.
    fn free(&mut self, previous: Spot) {
        self.previous = previous.previous;
        self.current = previous.current;
        self.point = previous.point;
        self.line_start = previous.line_start;
        debug_assert!(
            self.events.len() >= previous.events_len,
            "expected to restore less events than before"
        );
        self.events.truncate(previous.events_len);
        debug_assert!(
            self.stack.len() >= previous.stack_len,
            "expected to restore less stack items than before"
        );
        self.stack.truncate(previous.stack_len);
    }

    /// Stack an attempt, moving to `ok` on [`State::Ok`][] and `nok` on
    /// [`State::Nok`][], reverting in both cases.
    pub fn check(&mut self, ok: State, nok: State) {
        // Always capture (and restore) when checking.
        // No need to capture (and restore) when `nok` is `State::Nok`, because the
        // parent attempt will do it.
        let spot = Some(self.capture());
        let attempt = Attempt {
            kind: AttemptKind::Check,
            spot,
            ok,
            nok,
        };
        self.attempts.push(attempt);
        self.checks += 1;
    }

    /// Stack an attempt, moving to `ok` on [`State::Ok`][] and `nok` on
    /// [`State::Nok`][], reverting in the latter case.
    pub fn attempt(&mut self, ok: State, nok: State) {
        // Always capture (and restore) when checking.
        // No need to capture (and restore) when `nok` is `State::Nok`, because the
        // parent attempt will do it.
        let spot = if nok == State::Nok {
            None
        } else {
            Some(self.capture())
        };

        let attempt = Attempt {
            kind: AttemptKind::Attempt,
            spot,
            ok,
            nok,
        };
        self.attempts.push(attempt);
    }

    /// Tokenize.
    pub fn push(&mut self, from: (usize, usize), to: (usize, usize), state: State) -> State {
        push_impl(self, from, to, state, false)
    }

    /// Tokenize `from..to` as all there is, ending in [`State::Ok`][] or
    /// [`State::Nok`][], and without resolving.
    pub fn run(&mut self, from: (usize, usize), to: (usize, usize), state: State) -> State {
        push_impl(self, from, to, state, true)
    }

    /// Flush.
    pub fn flush(&mut self, state: State, resolve: bool) -> Subresult {
        let to = (self.point.index, self.point.vs);
        let state = push_impl(self, to, to, state, true);

        debug_assert_eq!(state, State::Ok, "expected a final state");

        let mut value = Subresult {
            done: false,
            definitions: core::mem::take(&mut self.definitions),
        };

        if resolve {
            let resolvers = self.resolvers.split_off(0);
            let mut index = 0;
            while index < resolvers.len() {
                if let Some(mut result) = call_resolve(self, resolvers[index]) {
                    value.definitions.append(&mut result.definitions);
                }
                index += 1;
            }

            self.map.consume(&mut self.events);
        }

        value
    }
}

/// Move back past ignored bytes.
fn move_point_back(tokenizer: &mut Tokenizer, point: &mut Point) {
    while point.index > 0 {
        point.index -= 1;
        let action = byte_action(tokenizer.parse_state.bytes, point);
        if !matches!(action, ByteAction::Ignore) {
            point.index += 1;
            break;
        }
    }
}

/// Enter.
fn enter_impl(tokenizer: &mut Tokenizer, name: Name, link: Option<Link>) {
    let mut point = tokenizer.point.clone();
    move_point_back(tokenizer, &mut point);

    tokenizer.stack.push(name);
    tokenizer.events.push(Event {
        kind: Kind::Enter,
        name,
        point,
        link,
    });
}

/// Run the tokenizer, with room on the stack: content tokenized inside
/// content, and containers in containers, run it again, once a level.
fn push_impl(
    tokenizer: &mut Tokenizer,
    from: (usize, usize),
    to: (usize, usize),
    state: State,
    flush: bool,
) -> State {
    room(|| push_steps(tokenizer, from, to, state, flush))
}

/// Run the tokenizer.
fn push_steps(
    tokenizer: &mut Tokenizer,
    from: (usize, usize),
    to: (usize, usize),
    mut state: State,
    flush: bool,
) -> State {
    debug_assert!(
        from.0 > tokenizer.point.index
            || (from.0 == tokenizer.point.index && from.1 >= tokenizer.point.vs),
        "cannot move backwards"
    );

    tokenizer.move_to(from);
    if tokenizer.fed != (to.0, flush) {
        tokenizer.fed = (to.0, flush);
        tokenizer.run = None;
    }

    loop {
        match state {
            State::Ok | State::Nok => {
                if let Some(attempt) = tokenizer.attempts.pop() {
                    if attempt.kind == AttemptKind::Check {
                        tokenizer.checks -= 1;
                    }
                    if attempt.kind == AttemptKind::Check || state == State::Nok {
                        if let Some(spot) = attempt.spot {
                            tokenizer.free(spot);
                        }
                    }

                    tokenizer.consumed = true;

                    let next = if state == State::Ok {
                        attempt.ok
                    } else {
                        attempt.nok
                    };

                    state = next;
                } else {
                    break;
                }
            }
            State::Next(name) => {
                let action = if tokenizer.point.index < to.0
                    || (tokenizer.point.index == to.0 && tokenizer.point.vs < to.1)
                {
                    Some(byte_action(tokenizer.parse_state.bytes, &tokenizer.point))
                } else if flush {
                    None
                } else {
                    break;
                };

                if let Some(ByteAction::Ignore) = action {
                    tokenizer.move_one();
                } else {
                    let byte =
                        if let Some(ByteAction::Insert(byte) | ByteAction::Normal(byte)) = action {
                            Some(byte)
                        } else {
                            None
                        };

                    tokenizer.expect(byte);
                    state = call(tokenizer, name);
                }
            }
            State::Redo(name) => {
                state = call(tokenizer, name);
            }
        }
    }

    tokenizer.consumed = true;

    if flush {
        debug_assert!(matches!(state, State::Ok | State::Nok), "must be done");
    } else {
        debug_assert!(matches!(state, State::Next(_)), "must have a next state");
    }

    state
}

/// Whether `byte` continues a scalar: columns count scalars, so tab stops
/// fall where characters put them.
pub fn continues(byte: u8) -> bool {
    (0x80..0xC0).contains(&byte)
}

/// Figure out how to handle a byte.
fn byte_action(bytes: &[u8], point: &Point) -> ByteAction {
    if point.index < bytes.len() {
        let byte = bytes[point.index];

        if byte == b'\r' {
            // CRLF.
            if point.index < bytes.len() - 1 && bytes[point.index + 1] == b'\n' {
                ByteAction::Ignore
            }
            // CR.
            else {
                ByteAction::Normal(b'\n')
            }
        } else if byte == b'\t' {
            let remainder = point.column % TAB_SIZE;
            let vs = if remainder == 0 {
                0
            } else {
                TAB_SIZE - remainder
            };

            // On the tab itself, first send it.
            if point.vs == 0 {
                if vs == 0 {
                    ByteAction::Normal(byte)
                } else {
                    ByteAction::Insert(byte)
                }
            } else if vs == 0 {
                ByteAction::Normal(b' ')
            } else {
                ByteAction::Insert(b' ')
            }
        } else {
            ByteAction::Normal(byte)
        }
    } else {
        unreachable!("out of bounds")
    }
}
