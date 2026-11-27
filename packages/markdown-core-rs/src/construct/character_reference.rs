//! Character references occur in the [string][string] and [text][] content types.
//!
//! ## Grammar
//!
//! Character references form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! character_reference ::= '&' (numeric | named) ';'
//!
//! numeric ::= '#' (hexadecimal | decimal)
//! ; Note: Limit of `6` imposed, as all bigger numbers are invalid.
//! hexadecimal ::= ('x' | 'X') 1*6(ascii_hexdigit)
//! ; Note: Limit of `7` imposed, as all bigger numbers are invalid.
//! decimal ::= 1*7(ascii_digit)
//! ; Note: Limit of `31` imposed, for `CounterClockwiseContourIntegral`.
//! ; Note: Limited to any known named character reference (see `constants.rs`)
//! named ::= 1*31(ascii_alphanumeric)
//! ```
//!
//! Like much of markdown, there are no “invalid” character references.
//! However, for security reasons, several numeric character references parse
//! fine but are not rendered as their corresponding character.
//! They are instead replaced by a U+FFFD REPLACEMENT CHARACTER (`�`).
//! See [`decode_numeric`][decode_numeric] for more info.
//!
//! To escape ASCII punctuation characters, use the terser
//! [character escape][character_escape] construct instead (as in, `\&`).
//!
//! Character references in markdown are not the same as character references
//! in HTML.
//! Notably, HTML allows several character references without a closing
//! semicolon.
//! See [*§ 13.2.5.72 Character reference state* in the HTML spec][html] for more info.
//!
//! Character references are parsed insensitive to casing.
//! The casing of hexadecimal numeric character references has no effect.
//! The casing of named character references does not matter when parsing, but
//! does affect whether they match.
//! Depending on the name, one or more cases are allowed, such as that `AMP`
//! and `amp` are both allowed but other cases are not.
//! See [`CHARACTER_REFERENCES`][character_references] for which
//! names match.
//!
//! ## Recommendation
//!
//! If possible, use a character escape.
//! Otherwise, use a character reference.
//!
//! ## Tokens
//!
//! * [`CharacterReference`][Name::CharacterReference]
//! * [`CharacterReferenceMarker`][Name::CharacterReferenceMarker]
//! * [`CharacterReferenceMarkerHexadecimal`][Name::CharacterReferenceMarkerHexadecimal]
//! * [`CharacterReferenceMarkerNumeric`][Name::CharacterReferenceMarkerNumeric]
//! * [`CharacterReferenceMarkerSemi`][Name::CharacterReferenceMarkerSemi]
//! * [`CharacterReferenceValue`][Name::CharacterReferenceValue]
//!
//! ## References
//!
//! * [`character-reference.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/character-reference.js)
//! * [*§ 2.5 Entity and numeric character references* in `CommonMark`](https://spec.commonmark.org/0.31/#entity-and-numeric-character-references)
//!
//! [plain]: crate::construct::plain
//! [text]: crate::construct::text
//! [character_escape]: crate::construct::character_reference
//! [decode_numeric]: crate::util::character_reference::decode_numeric
//! [character_references]: crate::util::constant::CHARACTER_REFERENCES
//! [html]: https://html.spec.whatwg.org/multipage/parsing.html#character-reference-state

use crate::choice::Starts;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::{
    character_reference::{decode_named, value_max, value_test},
    slice::Slice,
};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Codes(b"&");

/// Start of character reference.
///
/// ```markdown
/// > | a&amp;b
///      ^
/// > | a&#123;b
///      ^
/// > | a&#x9;b
///      ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b'&') {
        tokenizer.enter(Name::CharacterReference);
        tokenizer.enter(Name::CharacterReferenceMarker);
        tokenizer.consume();
        tokenizer.exit(Name::CharacterReferenceMarker);
        State::Next(StateName::CharacterReference(Step::Open))
    } else {
        State::Nok
    }
}

/// After `&`, at `#` for numeric references or alphanumeric for named
/// references.
///
/// ```markdown
/// > | a&amp;b
///       ^
/// > | a&#123;b
///       ^
/// > | a&#x9;b
///       ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    if let Some(b'#') = tokenizer.current {
        tokenizer.enter(Name::CharacterReferenceMarkerNumeric);
        tokenizer.consume();
        tokenizer.exit(Name::CharacterReferenceMarkerNumeric);
        State::Next(StateName::CharacterReference(Step::Numeric))
    } else {
        tokenizer.enter(Name::CharacterReferenceValue);
        State::Redo(StateName::CharacterReference(Step::Value {
            marker: b'&',
            size: 0,
        }))
    }
}

/// After `#`, at `x` for hexadecimals or digit for decimals.
///
/// ```markdown
/// > | a&#123;b
///        ^
/// > | a&#x9;b
///        ^
/// ```
fn numeric(tokenizer: &mut Tokenizer) -> State {
    if let Some(b'x' | b'X') = tokenizer.current {
        tokenizer.enter(Name::CharacterReferenceMarkerHexadecimal);
        tokenizer.consume();
        tokenizer.exit(Name::CharacterReferenceMarkerHexadecimal);
        tokenizer.enter(Name::CharacterReferenceValue);
        State::Next(StateName::CharacterReference(Step::Value {
            marker: b'x',
            size: 0,
        }))
    } else {
        tokenizer.enter(Name::CharacterReferenceValue);
        State::Redo(StateName::CharacterReference(Step::Value {
            marker: b'#',
            size: 0,
        }))
    }
}

/// After markers (`&#x`, `&#`, or `&`), in value, before `;`.
///
/// The character reference kind defines what and how many characters are
/// allowed.
///
/// ```markdown
/// > | a&amp;b
///       ^^^
/// > | a&#123;b
///        ^^^
/// > | a&#x9;b
///         ^
/// ```
fn value(tokenizer: &mut Tokenizer, marker: u8, size: usize) -> State {
    if matches!(tokenizer.current, Some(b';')) && size > 0 {
        // Named.
        if marker == b'&' {
            // Guaranteed to be valid ASCII bytes.
            let slice = Slice::from_indices(
                tokenizer.parse_state.source,
                tokenizer.point.index - size,
                tokenizer.point.index,
            );

            if decode_named(slice.as_str()).is_none() {
                return State::Nok;
            }
        }

        tokenizer.exit(Name::CharacterReferenceValue);
        tokenizer.enter(Name::CharacterReferenceMarkerSemi);
        tokenizer.consume();
        tokenizer.exit(Name::CharacterReferenceMarkerSemi);
        tokenizer.exit(Name::CharacterReference);
        return State::Ok;
    }

    if let Some(code) = tokenizer.current {
        if size < value_max(marker) && value_test(marker)(&code) {
            tokenizer.consume();
            return State::Next(StateName::CharacterReference(Step::Value {
                marker,
                size: size + 1,
            }));
        }
    }

    State::Nok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Open,
    Numeric,
    Value { marker: u8, size: usize },
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::CharacterReference(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Open => open(tokenizer),
        Step::Numeric => numeric(tokenizer),
        Step::Value { marker, size } => value(tokenizer, marker, size),
    }
}
