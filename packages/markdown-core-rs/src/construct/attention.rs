//! Attention (emphasis, strong, optionally GFM strikethrough) occurs in the
//! [text][] content type.
//!
//! ## Grammar
//!
//! Attention sequences form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! attention_sequence ::= 1*'*' | 1*'_'
//! gfm_attention_sequence ::= 1*'~'
//! ```
//!
//! Sequences are matched together to form attention based on which character
//! they contain, how long they are, and what character occurs before and after
//! each sequence.
//! Otherwise they are turned into data.
//!
//! ## HTML
//!
//! When asterisk/underscore sequences match, and two markers can be “taken”
//! from them, they together relate to the `<strong>` element in HTML.
//! When one marker can be taken, they relate to the `<em>` element.
//! See [*§ 4.5.2 The `em` element*][html-em] and
//! [*§ 4.5.3 The `strong` element*][html-strong] in the HTML spec for more
//! info.
//!
//! When tilde sequences match, they together relate to the `<del>` element in
//! HTML.
//! See [*§ 4.7.2 The `del` element*][html-del] in the HTML spec for more info.
//!
//! ## Recommendation
//!
//! It is recommended to use asterisks for emphasis/strong attention when
//! writing markdown.
//!
//! There are some small differences in whether sequences can open and/or close
//! based on whether they are formed with asterisks or underscores.
//! Because underscores also frequently occur in natural language inside words,
//! while asterisks typically never do, `CommonMark` prohibits underscore
//! sequences from opening or closing when *inside* a word.
//!
//! Because asterisks can be used to form the most markdown constructs, using
//! them has the added benefit of making it easier to gloss over markdown: you
//! can look for asterisks to find syntax while not worrying about other
//! characters.
//!
//! For strikethrough attention, it is recommended to use two markers.
//! While `github.com` allows single tildes too, it technically prohibits it in
//! their spec.
//!
//! ## Tokens
//!
//! * [`Emphasis`][Name::Emphasis]
//! * [`EmphasisSequence`][Name::EmphasisSequence]
//! * [`EmphasisText`][Name::EmphasisText]
//! * [`Strikethrough`][Name::Strikethrough]
//! * [`StrikethroughSequence`][Name::StrikethroughSequence]
//! * [`StrikethroughText`][Name::StrikethroughText]
//! * [`Strong`][Name::Strong]
//! * [`StrongSequence`][Name::StrongSequence]
//! * [`StrongText`][Name::StrongText]
//!
//! > 👉 **Note**: while parsing, [`AttentionSequence`][Name::AttentionSequence]
//! > is used, which is later compiled away.
//!
//! ## References
//!
//! * [`attention.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/attention.js)
//! * [`micromark-extension-gfm-strikethrough`](https://github.com/micromark/micromark-extension-gfm-strikethrough)
//! * [*§ 6.2 Emphasis and strong emphasis* in `CommonMark`](https://spec.commonmark.org/0.31/#emphasis-and-strong-emphasis)
//! * [*§ 6.5 Strikethrough (extension)* in `GFM`](https://github.github.com/gfm/#strikethrough-extension-)
//!
//! [text]: crate::construct::text
//! [html-em]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-em-element
//! [html-bold]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-strong-element
//! [html-del]: https://html.spec.whatwg.org/multipage/edits.html#the-del-element

use crate::choice::Starts;
use crate::event::{Event, Kind, Name, Point};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::Tokenizer;
use crate::util::char::{
    after_index as char_after_index, before_index as char_before_index, class_opt,
    Kind as CharacterKind,
};
use alloc::{vec, vec::Vec};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b"*+=^_~");

/// Byte that is transparent when looking at the neighbours of a run: the
/// character before or after a run is found past any of these.
const TRANSPARENT: u8 = b'~';

/// What a run of markers can pair into.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Rule {
    /// `*`: emphasis, strong.
    Asterisk,
    /// `_`: emphasis, strong.
    Underscore,
    /// `~~`: strikethrough.
    Strikethrough,
    /// `~`: subscript.
    Subscript,
    /// `^`: superscript.
    Superscript,
    /// `++`: insertion.
    Insertion,
    /// `==`: mark.
    Mark,
}

/// Which neighbours let a run open or close.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Flank {
    /// Left-flanking opens, right-flanking closes.
    Free,
    /// Like [`Flank::Free`][], but a run flanking both sides only opens
    /// after punctuation and only closes before punctuation.
    Bound,
}

/// What a body may contain.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Inner {
    /// Any inline content; the neighbours decide (see [`Flank`][]).
    Inline,
    /// No whitespace; every run can open and close.
    Word,
}

/// Event names of a node formed by a rule.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Names {
    whole: Name,
    sequence: Name,
    text: Name,
}

/// The grammar of a rule.
#[derive(Clone, Copy, Debug)]
struct Spec {
    /// Fewest markers a pair takes from each run.
    min: usize,
    /// Most markers a pair takes from each run.
    max: usize,
    /// Whether the rule of three applies.
    three: bool,
    /// Whether a run must be exactly `min` markers.
    exact: bool,
    flank: Flank,
    inner: Inner,
    /// Node of a pair taking one marker of each run.
    lone: Option<Names>,
    /// Node of a pair taking two markers of each run.
    double: Option<Names>,
}

const EMPHASIS: Names = Names {
    whole: Name::Emphasis,
    sequence: Name::EmphasisSequence,
    text: Name::EmphasisText,
};

const STRONG: Names = Names {
    whole: Name::Strong,
    sequence: Name::StrongSequence,
    text: Name::StrongText,
};

impl Rule {
    /// The rule of a run of `size` markers `marker`.
    fn of(marker: u8, size: usize) -> Rule {
        match marker {
            b'*' => Rule::Asterisk,
            b'_' => Rule::Underscore,
            b'~' if size == 1 => Rule::Subscript,
            b'~' => Rule::Strikethrough,
            b'^' => Rule::Superscript,
            b'+' => Rule::Insertion,
            b'=' => Rule::Mark,
            _ => unreachable!("expected attention marker"),
        }
    }

    /// The grammar of this rule.
    fn spec(self) -> Spec {
        let pair = |names| Spec {
            min: 2,
            max: 2,
            three: false,
            exact: false,
            flank: Flank::Free,
            inner: Inner::Inline,
            lone: None,
            double: Some(names),
        };
        let word = |names| Spec {
            min: 1,
            max: 1,
            three: false,
            exact: false,
            flank: Flank::Free,
            inner: Inner::Word,
            lone: Some(names),
            double: None,
        };
        match self {
            Rule::Asterisk | Rule::Underscore => Spec {
                min: 1,
                max: 2,
                three: true,
                exact: false,
                flank: if self == Rule::Asterisk {
                    Flank::Free
                } else {
                    Flank::Bound
                },
                inner: Inner::Inline,
                lone: Some(EMPHASIS),
                double: Some(STRONG),
            },
            Rule::Strikethrough => Spec {
                exact: true,
                ..pair(Names {
                    whole: Name::Strikethrough,
                    sequence: Name::StrikethroughSequence,
                    text: Name::StrikethroughText,
                })
            },
            Rule::Insertion => pair(Names {
                whole: Name::Insertion,
                sequence: Name::InsertionSequence,
                text: Name::InsertionText,
            }),
            Rule::Mark => pair(Names {
                whole: Name::Mark,
                sequence: Name::MarkSequence,
                text: Name::MarkText,
            }),
            Rule::Subscript => word(Names {
                whole: Name::Subscript,
                sequence: Name::SubscriptSequence,
                text: Name::SubscriptText,
            }),
            Rule::Superscript => word(Names {
                whole: Name::Superscript,
                sequence: Name::SuperscriptSequence,
                text: Name::SuperscriptText,
            }),
        }
    }
}

/// Attention sequence that we can take markers from.
#[derive(Debug)]
struct Sequence {
    rule: Rule,
    /// We track whether sequences are in balanced events, and where those
    /// events start, so that one attention doesn’t start in say, one link, and
    /// end in another.
    stack: Vec<usize>,
    /// Whitespace seen in text before this sequence; a word body cannot
    /// pair across whitespace.
    spaces: usize,
    /// The index into events where this sequence’s `Enter` currently resides.
    index: usize,
    /// The (shifted) point where this sequence starts.
    start_point: Point,
    /// The (shifted) point where this sequence end.
    end_point: Point,
    /// The number of markers we can still use.
    size: usize,
    /// Whether this sequence can open attention.
    open: bool,
    /// Whether this sequence can close attention.
    close: bool,
}

/// At start of attention.
///
/// ```markdown
/// > | **
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(marker @ (b'*' | b'+' | b'=' | b'^' | b'_' | b'~')) => {
            tokenizer.enter(Name::AttentionSequence);
            State::Redo(StateName::Attention(Step::Inside { marker, size: 0 }))
        }
        _ => State::Nok,
    }
}

/// In sequence.
///
/// A `^` is a sequence on its own: superscripts pair single markers.
///
/// ```markdown
/// > | **
///     ^^
/// ```
fn inside(tokenizer: &mut Tokenizer, marker: u8, size: usize) -> State {
    if tokenizer.current == Some(marker) && !(marker == b'^' && size == 1) {
        tokenizer.consume();
        State::Next(StateName::Attention(Step::Inside {
            marker,
            size: size + 1,
        }))
    } else {
        tokenizer.exit(Name::AttentionSequence);
        tokenizer.register_resolver(ResolveName::Attention);
        State::Ok
    }
}

/// Resolve sequences.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    // Find all sequences, gather info about them.
    let mut sequences = read_sequences(tokenizer);

    // Now walk through them and match them.
    let mut close = 0;

    while close < sequences.len() {
        let sequence_close = &sequences[close];
        let mut next_index = close + 1;

        // Find a sequence that can close.
        if sequence_close.close {
            let spec = sequence_close.rule.spec();
            let mut open = close;

            // Now walk back to find an opener.
            while open > 0 {
                open -= 1;

                let sequence_open = &sequences[open];

                // A word body cannot reach past whitespace.
                if spec.inner == Inner::Word && sequence_open.spaces != sequence_close.spaces {
                    break;
                }

                // An opener matching our closer:
                if sequence_open.open
                    && sequence_close.rule == sequence_open.rule
                    && sequence_close.stack == sequence_open.stack
                {
                    // If the opening can close or the closing can open,
                    // and the close size *is not* a multiple of three,
                    // but the sum of the opening and closing size *is*
                    // multiple of three, then **don’t** match.
                    if spec.three
                        && (sequence_open.close || sequence_close.open)
                        && sequence_close.size % 3 != 0
                        && (sequence_open.size + sequence_close.size) % 3 == 0
                    {
                        continue;
                    }

                    // We found a match!
                    next_index = match_sequences(tokenizer, &mut sequences, open, close);

                    break;
                }
            }
        }

        close = next_index;
    }

    // Mark remaining sequences as data.
    let mut index = 0;
    while index < sequences.len() {
        let sequence = &sequences[index];
        tokenizer.events[sequence.index].name = Name::Data;
        tokenizer.events[sequence.index + 1].name = Name::Data;
        index += 1;
    }

    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// Classify the character next to a run, past transparent bytes.
fn near(bytes: &[u8], index: usize, after: bool) -> CharacterKind {
    let mut index = index;
    if after {
        while index < bytes.len() && bytes[index] == TRANSPARENT {
            index += 1;
        }
        class_opt(char_after_index(bytes, index))
    } else {
        while index > 0 && bytes[index - 1] == TRANSPARENT {
            index -= 1;
        }
        class_opt(char_before_index(bytes, index))
    }
}

/// Whether a run with these neighbours can open and close.
fn flanks(flank: Flank, before: &CharacterKind, after: &CharacterKind) -> (bool, bool) {
    let space_before = *before == CharacterKind::Whitespace;
    let space_after = *after == CharacterKind::Whitespace;
    let punct_before = *before == CharacterKind::Punctuation;
    let punct_after = *after == CharacterKind::Punctuation;
    let left = !space_after && (!punct_after || space_before || punct_before);
    let right = !space_before && (!punct_before || space_after || punct_after);
    match flank {
        Flank::Free => (left, right),
        Flank::Bound => (
            left && (!right || punct_before),
            right && (!left || punct_after),
        ),
    }
}

/// Count whitespace in the text of an event: data with a space, tab, or
/// line ending, and line endings.
fn spaces(tokenizer: &Tokenizer, index: usize) -> usize {
    let event = &tokenizer.events[index];
    match event.name {
        Name::LineEnding => 1,
        Name::Data => {
            let end = tokenizer.events[index + 1].point.index;
            usize::from(
                tokenizer.parse_state.bytes[event.point.index..end]
                    .iter()
                    .any(|byte| matches!(byte, b'\t' | b'\n' | b' ')),
            )
        }
        _ => 0,
    }
}

/// Get sequences.
fn read_sequences(tokenizer: &mut Tokenizer) -> Vec<Sequence> {
    let bytes = tokenizer.parse_state.bytes;
    let mut index = 0;
    let mut stack = vec![];
    let mut sequences = vec![];
    let mut seen = 0;
    // Inside a resource or reference, data is not text.
    let mut hidden = 0;

    while index < tokenizer.events.len() {
        let enter = &tokenizer.events[index];

        if enter.name == Name::AttentionSequence {
            if enter.kind == Kind::Enter {
                let end = index + 1;
                let exit = &tokenizer.events[end];
                let marker = bytes[enter.point.index];
                let size = exit.point.index - enter.point.index;
                let rule = Rule::of(marker, size);
                let spec = rule.spec();
                let (open, close) = if spec.inner == Inner::Word {
                    (true, true)
                } else if size < spec.min || (spec.exact && size != spec.min) {
                    (false, false)
                } else {
                    let before = near(bytes, enter.point.index, false);
                    let after = near(bytes, exit.point.index, true);
                    flanks(spec.flank, &before, &after)
                };

                sequences.push(Sequence {
                    rule,
                    index,
                    stack: stack.clone(),
                    spaces: seen,
                    start_point: enter.point.clone(),
                    end_point: exit.point.clone(),
                    size,
                    open,
                    close,
                });
            }
        } else if enter.kind == Kind::Enter {
            if matches!(enter.name, Name::Resource | Name::Reference) {
                hidden += 1;
            } else if hidden == 0 {
                seen += spaces(tokenizer, index);
            }
            stack.push(index);
        } else {
            if matches!(enter.name, Name::Resource | Name::Reference) {
                hidden -= 1;
            }
            stack.pop();
        }

        index += 1;
    }

    sequences
}

/// Match two sequences.
#[allow(clippy::too_many_lines)]
fn match_sequences(
    tokenizer: &mut Tokenizer,
    sequences: &mut Vec<Sequence>,
    open: usize,
    close: usize,
) -> usize {
    // Where to move to next.
    // Stay on this closing sequence for the next iteration: it
    // might close more things.
    // It’s changed if sequences are removed.
    let mut next = close;
    let spec = sequences[close].rule.spec();

    // Number of markers to use from the sequence.
    let take = if sequences[open].size >= spec.max && sequences[close].size >= spec.max {
        spec.max
    } else {
        spec.min
    };

    // We’re *on* a closing sequence, with a matching opening
    // sequence.
    // Now we make sure that we can’t have misnested attention:
    //
    // ```html
    // <em>a <strong>b</em> c</strong>
    // ```
    //
    // Do that by marking everything between it as no longer
    // possible to open anything.
    // Theoretically we should mark as `close: false` too, but
    // we don’t look for closers backwards, so it’s not needed.
    let mut between = open + 1;

    while between < close {
        sequences[between].open = false;
        between += 1;
    }

    let names = if take == 1 { spec.lone } else { spec.double }
        .expect("expected a node for the markers taken");
    let (group_name, seq_name, text_name) = (names.whole, names.sequence, names.text);
    let open_index = sequences[open].index;
    let close_index = sequences[close].index;
    let open_exit = sequences[open].end_point.clone();
    let close_enter = sequences[close].start_point.clone();

    // No need to worry about `VS`, because sequences are only actual characters.
    sequences[open].size -= take;
    sequences[close].size -= take;
    sequences[open].end_point.column -= take;
    sequences[open].end_point.index -= take;
    sequences[close].start_point.column += take;
    sequences[close].start_point.index += take;

    // A remainder too short to pair again is text.
    if sequences[close].size > 0 && sequences[close].size < spec.min {
        sequences[close].open = false;
        sequences[close].close = false;
    }

    if sequences[open].size > 0 && sequences[open].size < spec.min {
        sequences[open].open = false;
        sequences[open].close = false;
    }

    // Opening.
    tokenizer.map.add_before(
        // Add after the current sequence (it might remain).
        open_index + 2,
        0,
        vec![
            Event {
                kind: Kind::Enter,
                name: group_name,
                point: sequences[open].end_point.clone(),
                link: None,
            },
            Event {
                kind: Kind::Enter,
                name: seq_name,
                point: sequences[open].end_point.clone(),
                link: None,
            },
            Event {
                kind: Kind::Exit,
                name: seq_name,
                point: open_exit.clone(),
                link: None,
            },
            Event {
                kind: Kind::Enter,
                name: text_name,
                point: open_exit,
                link: None,
            },
        ],
    );
    // Closing.
    tokenizer.map.add(
        close_index,
        0,
        vec![
            Event {
                kind: Kind::Exit,
                name: text_name,
                point: close_enter.clone(),
                link: None,
            },
            Event {
                kind: Kind::Enter,
                name: seq_name,
                point: close_enter,
                link: None,
            },
            Event {
                kind: Kind::Exit,
                name: seq_name,
                point: sequences[close].start_point.clone(),
                link: None,
            },
            Event {
                kind: Kind::Exit,
                name: group_name,
                point: sequences[close].start_point.clone(),
                link: None,
            },
        ],
    );

    // Remove closing sequence if fully used.
    if sequences[close].size == 0 {
        sequences.remove(close);
        tokenizer.map.add(close_index, 2, vec![]);
    } else {
        // Shift remaining closing sequence forward.
        // Do it here because a sequence can open and close different
        // other sequences, and the remainder can be on any side or
        // somewhere in the middle.
        tokenizer.events[close_index].point = sequences[close].start_point.clone();
    }

    if sequences[open].size == 0 {
        sequences.remove(open);
        tokenizer.map.add(open_index, 2, vec![]);
        // Everything shifts one to the left, account for it in next iteration.
        next -= 1;
    } else {
        tokenizer.events[open_index + 1].point = sequences[open].end_point.clone();
    }

    next
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Inside { marker: u8, size: usize },
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Attention(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Inside { marker, size } => inside(tokenizer, marker, size),
    }
}
