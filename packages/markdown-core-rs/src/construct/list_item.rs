//! List item occurs in the [document][] content type.
//!
//! ## Grammar
//!
//! List item forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! ; Restriction: if there is no space after the marker, the start must be followed by an `eol`.
//! ; Restriction: if the first line after the marker is not blank and starts with `5(space_or_tab)`,
//! ; only the first `space_or_tab` is part of the start.
//! ; Restriction: an ordered value is read in the variant of the list the
//! ; item would go on, else in its own; a value other than one interrupts no
//! ; paragraph and opens no list inside an item; a capital letter and `.`
//! ; with same-line content need two columns of whitespace.
//! list_item_start ::= ('*' | '+' | '-' | ordered) [1*4 space_or_tab]
//! ordered ::= '(' value ')' | value ('.' | ')')
//! value ::= '#' | 1*9(ascii_decimal) | ascii_alpha | roman
//! roman ::= 1*(roman_letter) ; one case, `M* [CM] [D] [CD] C* [XC] [L] [XL] X* [IX] [V] [IV] I*`
//!
//! ; Restriction: blank line allowed, except when this is the first continuation after a blank start.
//! ; Restriction: if not blank, the line must be indented, exactly `n` times.
//! list_item_cont ::= [n(space_or_tab)]
//! ```
//!
//! Further lines that are not prefixed with `list_item_cont` cause the list
//! item to be exited, except when those lines are lazy continuation or blank.
//! Like so many things in markdown, list items too are complex.
//! See [*§ Phase 1: block structure* in `CommonMark`][commonmark_block] for
//! more on parsing details.
//!
//! As list item is a container, it takes several bytes from the start of the
//! line, while the rest of the line includes more containers or flow.
//!
//! ## HTML
//!
//! List item relates to the `<li>`, `<ol>`, and `<ul>` elements in HTML.
//! See [*§ 4.4.8 The `li` element*][html_li],
//! [*§ 4.4.5 The `ol` element*][html_ol], and
//! [*§ 4.4.7 The `ul` element*][html_ul] in the HTML spec for more info.
//!
//! ## Recommendation
//!
//! Use a single space after a marker.
//! Never use lazy continuation.
//!
//! ## Tokens
//!
//! * [`ListItem`][Name::ListItem]
//! * [`ListItemMarker`][Name::ListItemMarker]
//! * [`ListItemPrefix`][Name::ListItemPrefix]
//! * [`ListItemValue`][Name::ListItemValue]
//! * [`ListOrdered`][Name::ListOrdered]
//! * [`ListUnordered`][Name::ListUnordered]
//!
//! ## References
//!
//! * [`list.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/list.js)
//! * [*§ 5.2 List items* in `CommonMark`](https://spec.commonmark.org/0.31/#list-items)
//! * [*§ 5.3 Lists* in `CommonMark`](https://spec.commonmark.org/0.31/#lists)
//!
//! [document]: crate::construct::document
//! [html_li]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-li-element
//! [html_ol]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-ol-element
//! [html_ul]: https://html.spec.whatwg.org/multipage/grouping-content.html#the-ul-element
//! [commonmark_block]: https://spec.commonmark.org/0.31/#phase-1-block-structure

use crate::construct::partial_space_or_tab::{
    space_or_tab_min_max, space_or_tab_with_options, Options as SpaceOrTabOptions,
};
use crate::construct::{blank_line, task_list_item_check, thematic_break};
use crate::event::{Event, Kind, Name};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::Container;
use crate::tokenizer::Tokenizer;
use crate::util::{
    constant::{LIST_ITEM_VALUE_SIZE_MAX, TAB_SIZE},
    skip,
    slice::{Position, Slice},
};
use alloc::{vec, vec::Vec};

/// How an item's marker reads: the facts its list is made of.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Facts {
    /// A bullet, by its byte.
    Bullet(u8),
    /// An ordered marker.
    Ordered {
        /// The variant its value is read in.
        numeral: Numeral,
        /// The delimiter after it.
        delimiter: Delimiter,
    },
}

/// The variant an ordered value is read in.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Numeral {
    /// Decimal digits.
    Decimal,
    /// One ASCII letter.
    Letter {
        /// Whether it is lowercase.
        lower: bool,
    },
    /// A Roman numeral.
    Roman {
        /// Whether it is lowercase.
        lower: bool,
    },
    /// The automatic marker `#`, which has no variant of its own.
    Auto,
}

/// The delimiter after an ordered value.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Delimiter {
    /// `.`.
    Period,
    /// `)`, or `(` and `)`.
    Parenthesis {
        /// Whether a `(` opens the value.
        closed: bool,
    },
    /// The `.` of an automatic marker that starts a list.
    Auto,
}

impl Numeral {
    /// The token of a value read in this variant.
    #[must_use]
    pub fn name(self) -> Name {
        match self {
            Numeral::Decimal => Name::ListItemValue,
            Numeral::Letter { lower: true } => Name::ListItemLetterLower,
            Numeral::Letter { lower: false } => Name::ListItemLetterUpper,
            Numeral::Roman { lower: true } => Name::ListItemRomanLower,
            Numeral::Roman { lower: false } => Name::ListItemRomanUpper,
            Numeral::Auto => Name::ListItemAuto,
        }
    }

    /// The variant a value token was read in.
    #[must_use]
    pub fn from_name(name: Name) -> Option<Numeral> {
        match name {
            Name::ListItemValue => Some(Numeral::Decimal),
            Name::ListItemLetterLower => Some(Numeral::Letter { lower: true }),
            Name::ListItemLetterUpper => Some(Numeral::Letter { lower: false }),
            Name::ListItemRomanLower => Some(Numeral::Roman { lower: true }),
            Name::ListItemRomanUpper => Some(Numeral::Roman { lower: false }),
            Name::ListItemAuto => Some(Numeral::Auto),
            _ => None,
        }
    }
}

/// Whether `byte` is a Roman letter, in either case.
fn roman(byte: u8) -> bool {
    matches!(
        byte.to_ascii_uppercase(),
        b'M' | b'D' | b'C' | b'L' | b'X' | b'V' | b'I'
    )
}

/// The value of `text` read in `numeral`, if they are one: `#` is one in
/// every variant.
#[must_use]
pub fn number(numeral: Numeral, text: &[u8]) -> Option<u32> {
    if text == b"#" {
        return Some(1);
    }
    match numeral {
        Numeral::Auto => None,
        Numeral::Decimal => {
            if text.is_empty() || text.len() > 9 || !text.iter().all(u8::is_ascii_digit) {
                return None;
            }
            Some(
                text.iter()
                    .fold(0, |value, byte| value * 10 + u32::from(byte - b'0')),
            )
        }
        Numeral::Letter { lower } => match text {
            [letter] if letter.is_ascii_alphabetic() && letter.is_ascii_lowercase() == lower => {
                Some(u32::from(letter.to_ascii_lowercase() - b'a') + 1)
            }
            _ => None,
        },
        Numeral::Roman { lower } => roman_value(text, lower),
    }
}

/// The value of a Roman numeral of one case, by the component order
/// `M* [CM] [D] [CD] C* [XC] [L] [XL] X* [IX] [V] [IV] I*`, at most
/// 999,999,999.
fn roman_value(text: &[u8], lower: bool) -> Option<u32> {
    const TERMS: [(&[u8], u32, bool); 13] = [
        (b"M", 1000, true),
        (b"CM", 900, false),
        (b"D", 500, false),
        (b"CD", 400, false),
        (b"C", 100, true),
        (b"XC", 90, false),
        (b"L", 50, false),
        (b"XL", 40, false),
        (b"X", 10, true),
        (b"IX", 9, false),
        (b"V", 5, false),
        (b"IV", 4, false),
        (b"I", 1, true),
    ];
    if text.is_empty() {
        return None;
    }
    let mut value = 0u32;
    let mut at = 0;
    for (term, worth, repeat) in TERMS {
        while at + term.len() <= text.len()
            && text[at..at + term.len()]
                .iter()
                .zip(term)
                .all(|(byte, want)| {
                    *byte
                        == if lower {
                            want.to_ascii_lowercase()
                        } else {
                            *want
                        }
                })
        {
            if value > 999_999_999 - worth {
                return None;
            }
            value += worth;
            at += term.len();
            if !repeat {
                break;
            }
        }
    }
    if at == text.len() {
        Some(value)
    } else {
        None
    }
}

/// The facts of the list item that `index`, a list item enter, opens.
#[must_use]
pub fn facts(events: &[Event], bytes: &[u8], index: usize) -> Option<Facts> {
    let mut index = index + 1;
    let mut closed = false;
    let mut numeral = None;
    while index < events.len() && events[index].name != Name::ListItemPrefix {
        index += 1;
    }
    while index < events.len() {
        let event = &events[index];
        if event.kind == Kind::Enter {
            match event.name {
                Name::ListItemMarker => {
                    let byte = bytes[event.point.index];
                    match (byte, numeral) {
                        (b'(', _) => closed = true,
                        (b'.', Some(numeral)) => {
                            return Some(Facts::Ordered {
                                numeral,
                                delimiter: Delimiter::Period,
                            })
                        }
                        (b')', Some(numeral)) => {
                            return Some(Facts::Ordered {
                                numeral,
                                delimiter: Delimiter::Parenthesis { closed },
                            })
                        }
                        _ => return Some(Facts::Bullet(byte)),
                    }
                }
                Name::ListItemAutoDelimiter => {
                    return numeral.map(|numeral| Facts::Ordered {
                        numeral,
                        delimiter: Delimiter::Auto,
                    })
                }
                name => {
                    if let Some(found) = Numeral::from_name(name) {
                        numeral = Some(found);
                    }
                }
            }
        }
        index += 1;
    }
    None
}

/// Start of list item.
///
/// ```markdown
/// > | * a
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::ListItem);

    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(State::Next(StateName::ListItem(Step::Before)), State::Nok);
        State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
    } else {
        State::Redo(StateName::ListItem(Step::Before))
    }
}

/// After optional whitespace, at list item prefix.
///
/// ```markdown
/// > | * a
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // Unordered.
        Some(b'*' | b'-') => {
            tokenizer.check(
                State::Nok,
                State::Next(StateName::ListItem(Step::BeforeUnordered)),
            );
            State::Redo(StateName::ThematicBreak(thematic_break::Step::Start))
        }
        Some(b'+') => State::Redo(StateName::ListItem(Step::BeforeUnordered)),
        // Ordered.
        Some(b'(' | b'#' | b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z') => {
            State::Redo(StateName::ListItem(Step::BeforeOrdered))
        }
        _ => State::Nok,
    }
}

/// At unordered list item marker.
///
/// The line is not a thematic break.
///
/// ```markdown
/// > | * a
///     ^
/// ```
fn before_unordered(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::ListItemPrefix);
    tokenizer.document.stack[tokenizer.document.continued].facts =
        tokenizer.current.map(Facts::Bullet);
    State::Redo(StateName::ListItem(Step::Marker))
}

/// At ordered list item value, or the parenthesis that opens it.
///
/// ```markdown
/// > | 1. a
///     ^
/// > | (i) b
///     ^
/// ```
fn before_ordered(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::ListItemPrefix);
    if tokenizer.current == Some(b'(') {
        tokenizer.enter(Name::ListItemMarker);
        tokenizer.consume();
        tokenizer.exit(Name::ListItemMarker);
        State::Next(StateName::ListItem(Step::Open { closed: true }))
    } else {
        State::Redo(StateName::ListItem(Step::Open { closed: false }))
    }
}

/// At the first byte of an ordered value: `#`, a byte, or a letter.
///
/// ```markdown
/// > | 1. a
///     ^
/// > | (i) b
///      ^
/// ```
fn open(tokenizer: &mut Tokenizer, closed: bool) -> State {
    match tokenizer.current {
        Some(first @ (b'#' | b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z')) => {
            tokenizer.enter(Name::ListItemValue);
            tokenizer.consume();
            State::Next(StateName::ListItem(Step::Value { closed, first }))
        }
        _ => State::Nok,
    }
}

/// In an ordered value, which goes on in the class its first byte names:
/// one `#`, up to ten digits, or one letter, or Roman letters of its case.
///
/// A run that leaves its class is refused at the delimiter.
///
/// ```markdown
/// > | 12. a
///      ^
/// > | iv. b
///      ^
/// ```
fn value(tokenizer: &mut Tokenizer, closed: bool, first: u8) -> State {
    let enter = tokenizer.events.len() - 1;
    let size = tokenizer.point.index - tokenizer.events[enter].point.index;
    let more = match (first, tokenizer.current) {
        (b'0'..=b'9', Some(b'0'..=b'9')) => size < LIST_ITEM_VALUE_SIZE_MAX,
        (b'A'..=b'Z' | b'a'..=b'z', Some(byte)) => {
            roman(first) && roman(byte) && byte.is_ascii_lowercase() == first.is_ascii_lowercase()
        }
        _ => false,
    };
    if more {
        tokenizer.consume();
        State::Next(StateName::ListItem(Step::Value { closed, first }))
    } else {
        State::Redo(StateName::ListItem(Step::Close { closed, first }))
    }
}

/// At the delimiter after an ordered value: the value is read here, in the
/// variant of the list it would go on, or else its own.
///
/// ```markdown
/// > | 1. a
///      ^
/// > | (i) b
///       ^
/// ```
fn close(tokenizer: &mut Tokenizer, closed: bool, first: u8) -> State {
    let delimiter = match tokenizer.current {
        Some(b')') => Delimiter::Parenthesis { closed },
        Some(b'.') if !closed => Delimiter::Period,
        _ => return State::Nok,
    };
    let enter = tokenizer.events.len() - 1;
    let from = tokenizer.events[enter].point.index;
    let text = &tokenizer.parse_state.bytes[from..tokenizer.point.index];
    let document = &tokenizer.document;
    let committed = if document.stack.len() > document.continued + 1 {
        document.stack.last().and_then(|container| container.facts)
    } else {
        None
    };
    let own = if first == b'#' {
        Numeral::Auto
    } else if first.is_ascii_digit() {
        Numeral::Decimal
    } else if text.len() == 1 && first != b'i' && first != b'I' {
        Numeral::Letter {
            lower: first.is_ascii_lowercase(),
        }
    } else {
        Numeral::Roman {
            lower: first.is_ascii_lowercase(),
        }
    };
    let inherited = match committed {
        Some(Facts::Ordered { numeral, .. }) => number(numeral, text).map(|value| (numeral, value)),
        _ => None,
    };
    let (numeral, value) = match inherited.or_else(|| number(own, text).map(|value| (own, value))) {
        Some(found) => found,
        None => return State::Nok,
    };
    let auto = first == b'#'
        && delimiter == Delimiter::Period
        && matches!(
            committed,
            None | Some(Facts::Ordered {
                delimiter: Delimiter::Auto,
                ..
            })
        );
    let facts = Facts::Ordered {
        numeral,
        delimiter: if auto { Delimiter::Auto } else { delimiter },
    };
    if value != 1 {
        // Only a first item may interrupt a paragraph, and a new list in an
        // item starts at one.
        let inside = document.stack[..document.continued]
            .iter()
            .any(|container| container.kind == Container::ListItem);
        if tokenizer.interrupt || (committed != Some(facts) && inside) {
            return State::Nok;
        }
    }
    // A capital letter and a period with same-line content need two
    // columns of whitespace: `B. Russell` is text.
    if text.len() == 1 && first.is_ascii_uppercase() && delimiter == Delimiter::Period {
        let bytes = tokenizer.parse_state.bytes;
        let start = tokenizer.point.column;
        let mut column = start;
        let mut at = tokenizer.point.index + 1;
        while column - start < 2 && at < bytes.len() && matches!(bytes[at], b'\t' | b' ') {
            column += if bytes[at] == b'\t' {
                TAB_SIZE - column % TAB_SIZE
            } else {
                1
            };
            at += 1;
        }
        if column - start < 2 && at < bytes.len() && !matches!(bytes[at], b'\n' | b'\r') {
            return State::Nok;
        }
    }
    // The value is named by the variant it was read in.
    tokenizer.exit(Name::ListItemValue);
    let exit = tokenizer.events.len() - 1;
    tokenizer.events[enter].name = numeral.name();
    tokenizer.events[exit].name = numeral.name();
    tokenizer.document.stack[tokenizer.document.continued].facts = Some(facts);
    let name = if auto {
        Name::ListItemAutoDelimiter
    } else {
        Name::ListItemMarker
    };
    tokenizer.enter(name);
    tokenizer.consume();
    tokenizer.exit(name);
    State::Next(StateName::ListItem(Step::MarkerAfter))
}

/// At list item marker.
///
/// ```markdown
/// > | * a
///     ^
/// > | 1. b
///      ^
/// ```
fn marker(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::ListItemMarker);
    tokenizer.consume();
    tokenizer.exit(Name::ListItemMarker);
    State::Next(StateName::ListItem(Step::MarkerAfter))
}

/// After list item marker.
///
/// ```markdown
/// > | * a
///      ^
/// > | 1. b
///       ^
/// ```
fn marker_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.check(
        State::Next(StateName::ListItem(Step::After { blank: true })),
        State::Next(StateName::ListItem(Step::MarkerAfterFilled)),
    );
    State::Redo(StateName::BlankLine(blank_line::Step::Start))
}

/// After list item marker.
///
/// The marker is not followed by a blank line.
///
/// ```markdown
/// > | * a
///      ^
/// ```
fn marker_after_filled(tokenizer: &mut Tokenizer) -> State {
    // Attempt to parse up to the largest allowed indent, `nok` if there is more whitespace.
    tokenizer.attempt(
        State::Next(StateName::ListItem(Step::After { blank: false })),
        State::Next(StateName::ListItem(Step::PrefixOther)),
    );
    State::Redo(StateName::ListItem(Step::Whitespace))
}

/// After marker, at whitespace.
///
/// ```markdown
/// > | * a
///      ^
/// ```
fn whitespace(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::ListItem(Step::WhitespaceAfter)),
        State::Nok,
    );
    State::Redo(space_or_tab_min_max(1, TAB_SIZE))
}

/// After acceptable whitespace.
///
/// ```markdown
/// > | * a
///      ^
/// ```
fn whitespace_after(tokenizer: &mut Tokenizer) -> State {
    if let Some(b'\t' | b' ') = tokenizer.current {
        State::Nok
    } else {
        State::Ok
    }
}

/// After marker, followed by no indent or more indent that needed.
///
/// ```markdown
/// > | * a
///      ^
/// ```
fn prefix_other(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.enter(Name::SpaceOrTab);
            tokenizer.consume();
            tokenizer.exit(Name::SpaceOrTab);
            State::Next(StateName::ListItem(Step::After { blank: false }))
        }
        _ => State::Nok,
    }
}

/// After list item prefix.
///
/// ```markdown
/// > | * a
///       ^
/// ```
fn after(tokenizer: &mut Tokenizer, blank: bool) -> State {
    if blank && tokenizer.interrupt {
        State::Nok
    } else {
        let start = skip::to_back(
            &tokenizer.events,
            tokenizer.events.len() - 1,
            &[Name::ListItem],
        );
        let mut prefix = Slice::from_position(
            tokenizer.parse_state.bytes,
            &Position {
                start: &tokenizer.events[start].point,
                end: &tokenizer.point,
            },
        )
        .len();

        if blank {
            prefix += 1;
        }

        let container = &mut tokenizer.document.stack[tokenizer.document.continued];

        container.blank_initial = blank;
        container.size = prefix;

        if blank {
            State::Redo(StateName::ListItem(Step::PrefixEnd))
        } else {
            // A task check ends the prefix, past the item's indent.
            tokenizer.attempt(
                State::Next(StateName::ListItem(Step::PrefixEnd)),
                State::Next(StateName::ListItem(Step::PrefixEnd)),
            );
            State::Redo(StateName::TaskListItemCheck(
                task_list_item_check::Step::Start,
            ))
        }
    }
}

/// At the end of the list item prefix.
///
/// ```markdown
/// > | * a
///       ^
/// > | * [x] b
///           ^
/// ```
fn prefix_end(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::ListItemPrefix);
    tokenizer.register_resolver_before(ResolveName::ListItem);
    State::Ok
}

/// Start of list item continuation.
///
/// ```markdown
///   | * a
/// > |   b
///     ^
/// ```
fn cont_start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.check(
        State::Next(StateName::ListItem(Step::ContBlank)),
        State::Next(StateName::ListItem(Step::ContFilled)),
    );
    State::Redo(StateName::BlankLine(blank_line::Step::Start))
}

/// Start of blank list item continuation.
///
/// ```markdown
///   | * a
/// > |
///     ^
///   |   b
/// ```
fn cont_blank(tokenizer: &mut Tokenizer) -> State {
    let container = &mut tokenizer.document.stack[tokenizer.document.continued];
    let size = container.size;

    if container.blank_initial {
        State::Nok
    } else if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        // Consume, optionally, at most `size`.
        State::Redo(indent(0, size))
    } else {
        State::Ok
    }
}

/// Start of non-blank list item continuation.
///
/// ```markdown
///   | * a
/// > |   b
///     ^
/// ```
fn cont_filled(tokenizer: &mut Tokenizer) -> State {
    let container = &mut tokenizer.document.stack[tokenizer.document.continued];
    let size = container.size;

    container.blank_initial = false;

    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        // Consume exactly `size`.
        State::Redo(indent(size, size))
    } else {
        State::Nok
    }
}

/// Find adjacent list items with the same marker.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    let mut lists_wip: Vec<(Option<Facts>, usize, usize, usize)> = vec![];
    let mut lists: Vec<(Option<Facts>, usize, usize, usize)> = vec![];
    let mut index = 0;
    let mut balance = 0;

    // Merge list items.
    while index < tokenizer.events.len() {
        let event = &tokenizer.events[index];

        if event.name == Name::ListItem {
            if event.kind == Kind::Enter {
                let end = skip::opt(&tokenizer.events, index, &[Name::ListItem]) - 1;
                let marker = facts(&tokenizer.events, tokenizer.parse_state.bytes, index);
                let current = (marker, balance, index, end);

                let mut list_index = lists_wip.len();
                let mut matched = false;

                while list_index > 0 {
                    list_index -= 1;
                    let previous = &lists_wip[list_index];
                    let before = skip::opt(
                        &tokenizer.events,
                        previous.3 + 1,
                        &[
                            Name::SpaceOrTab,
                            Name::ContainerIndent,
                            Name::LineEnding,
                            Name::BlankLineEnding,
                            Name::BlockQuotePrefix,
                        ],
                    );

                    if previous.0 == current.0 && previous.1 == current.1 && before == current.2 {
                        let previous_mut = &mut lists_wip[list_index];
                        previous_mut.3 = current.3;
                        lists.append(&mut lists_wip.split_off(list_index + 1));
                        matched = true;
                        break;
                    }
                }

                if !matched {
                    let mut index = lists_wip.len();
                    let mut exit = None;

                    while index > 0 {
                        index -= 1;

                        // If the current (new) item starts after where this
                        // item on the stack ends, we can remove it from the
                        // stack.
                        if current.2 > lists_wip[index].3 {
                            exit = Some(index);
                        } else {
                            break;
                        }
                    }

                    if let Some(exit) = exit {
                        lists.append(&mut lists_wip.split_off(exit));
                    }

                    lists_wip.push(current);
                }

                balance += 1;
            } else {
                balance -= 1;
            }
        }

        index += 1;
    }

    lists.append(&mut lists_wip);

    // Inject events.
    let mut index = 0;
    while index < lists.len() {
        let list_item = &lists[index];
        let mut list_start = tokenizer.events[list_item.2].clone();
        let mut list_end = tokenizer.events[list_item.3].clone();
        let name = match list_item.0 {
            Some(Facts::Ordered { .. }) => Name::ListOrdered,
            _ => Name::ListUnordered,
        };
        list_start.name = name;
        list_end.name = name;

        tokenizer.map.add(list_item.2, 0, vec![list_start]);
        tokenizer.map.add(list_item.3 + 1, 0, vec![list_end]);

        index += 1;
    }

    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// The indentation a continuation line consumes for the list item.
fn indent(min: usize, max: usize) -> StateName {
    space_or_tab_with_options(SpaceOrTabOptions {
        kind: Name::ContainerIndent,
        min,
        max,
        content: None,
        connect: false,
    })
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    BeforeOrdered,
    BeforeUnordered,
    Open { closed: bool },
    Value { closed: bool, first: u8 },
    Close { closed: bool, first: u8 },
    Marker,
    MarkerAfter,
    After { blank: bool },
    MarkerAfterFilled,
    Whitespace,
    WhitespaceAfter,
    PrefixOther,
    PrefixEnd,
    ContStart,
    ContBlank,
    ContFilled,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::ListItem(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::BeforeOrdered => before_ordered(tokenizer),
        Step::BeforeUnordered => before_unordered(tokenizer),
        Step::Open { closed } => open(tokenizer, closed),
        Step::Value { closed, first } => value(tokenizer, closed, first),
        Step::Close { closed, first } => close(tokenizer, closed, first),
        Step::Marker => marker(tokenizer),
        Step::MarkerAfter => marker_after(tokenizer),
        Step::After { blank } => after(tokenizer, blank),
        Step::MarkerAfterFilled => marker_after_filled(tokenizer),
        Step::Whitespace => whitespace(tokenizer),
        Step::WhitespaceAfter => whitespace_after(tokenizer),
        Step::PrefixOther => prefix_other(tokenizer),
        Step::PrefixEnd => prefix_end(tokenizer),
        Step::ContStart => cont_start(tokenizer),
        Step::ContBlank => cont_blank(tokenizer),
        Step::ContFilled => cont_filled(tokenizer),
    }
}
