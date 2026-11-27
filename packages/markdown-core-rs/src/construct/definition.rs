//! Definition occurs in the [content] content type.
//!
//! ## Grammar
//!
//! Definition forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! definition ::= label ':' [ space_or_tab_eol ] destination [ space_or_tab_eol title ] [ space_or_tab ]
//!
//! ; See the `destination`, `title`, and `label` constructs for the BNF of
//! ; those parts.
//! ```
//!
//! This construct must be followed by an eol (line ending) or eof (end of
//! file), like flow constructs.
//!
//! See [`destination`][destination], [`label`][label], and [`title`][title]
//! for grammar, notes, and recommendations on each part.
//!
//! The `destination`, `label`, and `title` parts are interpreted as the
//! [string][string] content type.
//! That means that [character escapes][character_escape] and
//! [character references][character_reference] are allowed.
//!
//! Definitions match to references through identifiers.
//! To match, both labels must be equal after normalizing with
//! [`normalize_identifier`][normalize_identifier].
//! One definition can match to multiple references.
//! Multiple definitions with the same, normalized, identifier are ignored: the
//! first definition is preferred.
//! To illustrate, the definition with a destination of `x` wins:
//!
//! ```markdown
//! [a]: x
//! [a]: y
//!
//! [a]
//! ```
//!
//! Importantly, while labels *can* include [string][string] content (character
//! escapes and character references), these are not considered when matching.
//! To illustrate, neither definition matches the reference:
//!
//! ```markdown
//! [a&amp;b]: x
//! [a\&b]: y
//!
//! [a&b]
//! ```
//!
//! For info on how to encode characters in URLs, see
//! [`destination`][destination].
//! For info on how characters are encoded as `href` on `<a>` or `src` on
//! `<img>` when compiling, see
//! `sanitize_uri` of markdown-rs.
//!
//! ## HTML
//!
//! Definitions in markdown do not, on their own, relate to anything in HTML.
//! When matched with a [label end (reference)][label_end], they together
//! relate to the `<a>` or `<img>` elements in HTML.
//! The definition forms its `href` or `src`, and optionally `title`,
//! attributes.
//! See [*§ 4.5.1 The `a` element*][html_a] and
//! [*§ 4.8.3 The `img` element*][html_img] in the HTML spec for more info.
//!
//! ## Tokens
//!
//! * [`Definition`][Name::Definition]
//! * [`DefinitionDestination`][Name::DefinitionDestination]
//! * [`DefinitionDestinationLiteral`][Name::DefinitionDestinationLiteral]
//! * [`DefinitionDestinationLiteralMarker`][Name::DefinitionDestinationLiteralMarker]
//! * [`DefinitionDestinationRaw`][Name::DefinitionDestinationRaw]
//! * [`DefinitionDestinationText`][Name::DefinitionDestinationText]
//! * [`DefinitionLabel`][Name::DefinitionLabel]
//! * [`DefinitionLabelMarker`][Name::DefinitionLabelMarker]
//! * [`DefinitionLabelText`][Name::DefinitionLabelText]
//! * [`DefinitionMarker`][Name::DefinitionMarker]
//! * [`DefinitionTitle`][Name::DefinitionTitle]
//! * [`DefinitionTitleMarker`][Name::DefinitionTitleMarker]
//! * [`DefinitionTitleText`][Name::DefinitionTitleText]
//! * [`LineEnd`][Name::LineEnd]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! ## References
//!
//! * [`definition.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/definition.js)
//! * [*§ 4.7 Link reference definitions* in `CommonMark`](https://spec.commonmark.org/0.31/#link-reference-definitions)
//!
//! [content]: crate::construct::content
//! [plain]: crate::construct::plain
//! [character_escape]: crate::construct::character_escape
//! [character_reference]: crate::construct::character_reference
//! [destination]: crate::construct::partial_destination
//! [label]: crate::construct::partial_label
//! [label_end]: crate::construct::label_end
//! [title]: crate::construct::partial_title
//! [normalize_identifier]: crate::util::normalize_identifier
//! [html_a]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-a-element
//! [html_img]: https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element

use crate::construct::partial_space_or_tab::space_or_tab;
use crate::construct::partial_space_or_tab_eol::space_or_tab_eol;
use crate::construct::{partial_destination, partial_label, partial_title};
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::{
    normalize_identifier::normalize_identifier,
    skip,
    slice::{Position, Slice},
};

/// At start of a definition.
///
/// ```markdown
/// > | [a]: b "c"
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    // Do not interrupt paragraphs (but do follow definitions).
    if !tokenizer.interrupt
        || (!tokenizer.events.is_empty()
            && tokenizer.events[skip::opt_back(
                &tokenizer.events,
                tokenizer.events.len() - 1,
                &[Name::LineEnd, Name::SpaceOrTab],
            )]
            .name
                == Name::Definition)
    {
        tokenizer.enter(Name::Definition);

        if matches!(tokenizer.current, Some(b'\t' | b' ')) {
            // Note: arbitrary whitespace allowed even if code (indented) is on.
            tokenizer.attempt(State::Next(StateName::Definition(Step::Before)), State::Nok);
            State::Redo(space_or_tab())
        } else {
            State::Redo(StateName::Definition(Step::Before))
        }
    } else {
        State::Nok
    }
}

/// After optional whitespace, at `[`.
///
/// ```markdown
/// > | [a]: b "c"
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'[') => {
            tokenizer.attempt(
                State::Next(StateName::Definition(Step::LabelAfter)),
                State::Nok,
            );
            State::Redo(StateName::Label(partial_label::Step::Start(
                partial_label::Names {
                    whole: Name::DefinitionLabel,
                    marker: Name::DefinitionLabelMarker,
                    text: Name::DefinitionLabelText,
                },
            )))
        }
        _ => State::Nok,
    }
}

/// After label.
///
/// ```markdown
/// > | [a]: b "c"
///        ^
/// ```
fn label_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b':') => {
            let end = skip::to_back(
                &tokenizer.events,
                tokenizer.events.len() - 1,
                &[Name::DefinitionLabelText],
            );

            tokenizer.enter(Name::DefinitionMarker);
            tokenizer.consume();
            tokenizer.exit(Name::DefinitionMarker);
            State::Next(StateName::Definition(Step::MarkerAfter { end }))
        }
        _ => State::Nok,
    }
}

/// After marker.
///
/// ```markdown
/// > | [a]: b "c"
///         ^
/// ```
fn marker_after(tokenizer: &mut Tokenizer, end: usize) -> State {
    tokenizer.attempt(
        State::Next(StateName::Definition(Step::DestinationBefore { end })),
        State::Next(StateName::Definition(Step::DestinationBefore { end })),
    );
    State::Redo(space_or_tab_eol())
}

/// Before destination.
///
/// ```markdown
/// > | [a]: b "c"
///          ^
/// ```
fn destination_before(tokenizer: &mut Tokenizer, end: usize) -> State {
    tokenizer.attempt(
        State::Next(StateName::Definition(Step::DestinationAfter { end })),
        State::Nok,
    );
    State::Redo(StateName::Destination(partial_destination::Step::Start(
        partial_destination::Options {
            names: partial_destination::Names {
                whole: Name::DefinitionDestination,
                literal: Name::DefinitionDestinationLiteral,
                marker: Name::DefinitionDestinationLiteralMarker,
                raw: Name::DefinitionDestinationRaw,
                text: Name::DefinitionDestinationText,
            },
            limit: usize::MAX,
        },
    )))
}

/// After destination.
///
/// ```markdown
/// > | [a]: b "c"
///           ^
/// ```
fn destination_after(tokenizer: &mut Tokenizer, end: usize) -> State {
    tokenizer.attempt(
        State::Next(StateName::Definition(Step::After { end })),
        State::Next(StateName::Definition(Step::After { end })),
    );
    State::Redo(StateName::Definition(Step::TitleBefore))
}

/// After definition.
///
/// ```markdown
/// > | [a]: b
///           ^
/// > | [a]: b "c"
///               ^
/// ```
fn after(tokenizer: &mut Tokenizer, end: usize) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::Definition(Step::AfterWhitespace { end })),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::Definition(Step::AfterWhitespace { end }))
    }
}

/// After definition, after optional whitespace.
///
/// ```markdown
/// > | [a]: b
///           ^
/// > | [a]: b "c"
///               ^
/// ```
fn after_whitespace(tokenizer: &mut Tokenizer, end: usize) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::Definition);

            // Note: we don’t care about uniqueness.
            // It’s likely that that doesn’t happen very frequently.
            // It is more likely that it wastes precious time.
            tokenizer.definitions.links.push(
                // Note: we don’t care about virtual spaces, so `as_str` is fine.
                normalize_identifier(
                    Slice::from_position(
                        tokenizer.parse_state.source,
                        &Position::from_exit_event(&tokenizer.events, end),
                    )
                    .as_str(),
                ),
            );

            // You’d be interrupting.
            tokenizer.interrupt = true;
            State::Ok
        }
        _ => State::Nok,
    }
}

/// After destination, at whitespace.
///
/// ```markdown
/// > | [a]: b
///           ^
/// > | [a]: b "c"
///           ^
/// ```
fn title_before(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b'\n' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::Definition(Step::TitleBeforeMarker)),
            State::Nok,
        );
        State::Redo(space_or_tab_eol())
    } else {
        State::Nok
    }
}

/// At title.
///
/// ```markdown
///   | [a]: b
/// > | "c"
///     ^
/// ```
fn title_before_marker(tokenizer: &mut Tokenizer) -> State {
    tokenizer.attempt(
        State::Next(StateName::Definition(Step::TitleAfter)),
        State::Nok,
    );
    State::Redo(StateName::Title(partial_title::Step::Start(
        partial_title::Names {
            whole: Name::DefinitionTitle,
            marker: Name::DefinitionTitleMarker,
            text: Name::DefinitionTitleText,
        },
    )))
}

/// After title.
///
/// ```markdown
/// > | [a]: b "c"
///               ^
/// ```
fn title_after(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::Definition(Step::TitleAfterOptionalWhitespace)),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::Definition(Step::TitleAfterOptionalWhitespace))
    }
}

/// After title, after optional whitespace.
///
/// ```markdown
/// > | [a]: b "c"
///               ^
/// ```
fn title_after_optional_whitespace(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Ok,
        _ => State::Nok,
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    LabelAfter,
    MarkerAfter { end: usize },
    DestinationBefore { end: usize },
    DestinationAfter { end: usize },
    TitleBefore,
    After { end: usize },
    AfterWhitespace { end: usize },
    TitleBeforeMarker,
    TitleAfter,
    TitleAfterOptionalWhitespace,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Definition(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::LabelAfter => label_after(tokenizer),
        Step::MarkerAfter { end } => marker_after(tokenizer, end),
        Step::DestinationBefore { end } => destination_before(tokenizer, end),
        Step::DestinationAfter { end } => destination_after(tokenizer, end),
        Step::TitleBefore => title_before(tokenizer),
        Step::After { end } => after(tokenizer, end),
        Step::AfterWhitespace { end } => after_whitespace(tokenizer, end),
        Step::TitleBeforeMarker => title_before_marker(tokenizer),
        Step::TitleAfter => title_after(tokenizer),
        Step::TitleAfterOptionalWhitespace => title_after_optional_whitespace(tokenizer),
    }
}
