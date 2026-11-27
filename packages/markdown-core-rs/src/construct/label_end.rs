//! Label end occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Label end forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! label_end ::= ']' [resource | reference_full | reference_collapsed]
//!
//! resource ::= '(' [space_or_tab_eol] destination [space_or_tab_eol title] [space_or_tab_eol] ')'
//! reference_full ::= '[' label ']'
//! reference_collapsed ::= '[' ']'
//!
//! ; See the `destination`, `title`, and `label` constructs for the BNF of
//! ; those parts.
//! ```
//!
//! See [`destination`][destination], [`label`][label], and [`title`][title]
//! for grammar, notes, and recommendations on each part.
//!
//! In the case of a resource, the destination and title are given directly
//! with the label end.
//! In the case of a reference, this information is provided by a matched
//! [definition][].
//! Full references (`[x][y]`) match to definitions through their explicit,
//! second, label (`y`).
//! Collapsed references (`[x][]`) and shortcut references (`[x]`) match by
//! interpreting the text provided between the first, implicit, label (`x`).
//! To match, the effective label of the reference must be equal to the label
//! of the definition after normalizing with
//! [`normalize_identifier`][].
//!
//! Importantly, while the label of a full reference *can* include [string][string]
//! content, and in case of collapsed and shortcut references even [text][]
//! content, that content is not considered when matching.
//! To illustrate, neither label matches the definition:
//!
//! ```markdown
//! [a&b]: https://example.com
//!
//! [x][a&amp;b], [a\&b][]
//! ```
//!
//! When the resource or reference matches, the destination forms the `href`
//! attribute in case of a [label start (link)][label_start_link], and an
//! `src` attribute in case of a [label start (image)][label_start_image].
//! The title is formed, optionally, on either `<a>` or `<img>`.
//! When matched with a [gfm label start (footnote)][label_start_footnote],
//! no reference or resource can follow the label end.
//!
//! For info on how to encode characters in URLs, see
//! [`destination`][destination].
//! For info on how characters are encoded as `href` on `<a>` or `src` on
//! `<img>` when compiling, see
//! `sanitize_uri` of markdown-rs.
//!
//! In case of a matched [gfm label start (footnote)][label_start_footnote],
//! a counter is injected.
//! In case of a matched [label start (link)][label_start_link], the interpreted
//! content between it and the label end, is placed between the opening and
//! closing tags.
//! In case of a matched [label start (image)][label_start_image], the text is
//! also interpreted, but used *without* the resulting tags:
//!
//! ```markdown
//! [a *b* c](#)
//!
//! ![a *b* c](#)
//! ```
//!
//! Yields:
//!
//! ```html
//! <p><a href="#">a <em>b</em> c</a></p>
//! <p><img src="#" alt="a b c" /></p>
//! ```
//!
//! It is possible to use images in links.
//! It’s somewhat possible to have links in images (the text will be used, not
//! the HTML, see above).
//! But it’s not possible to use links (or footnotes, which result in links)
//! in links.
//! The “deepest” link (or footnote) wins.
//! To illustrate:
//!
//! ```markdown
//! a [b [c](#) d](#) e
//! ```
//!
//! Yields:
//!
//! ```html
//! <p>a [b <a href="#">c</a> d](#) e</p>
//! ```
//!
//! This limitation is imposed because links in links is invalid according to
//! HTML.
//! Technically though, in markdown it is still possible to construct them by
//! using an [autolink][] in a link.
//! You definitely should not do that.
//!
//! ## HTML
//!
//! Label end does not, on its own, relate to anything in HTML.
//! When matched with a [label start (link)][label_start_link], they together
//! relate to the `<a>` element in HTML.
//! See [*§ 4.5.1 The `a` element*][html_a] in the HTML spec for more info.
//! It can also match with [label start (image)][label_start_image], in which
//! case they form an `<img>` element.
//! See [*§ 4.8.3 The `img` element*][html_img] in the HTML spec for more info.
//! It can also match with [gfm label start (footnote)][label_start_footnote],
//! in which case they form `<sup>` and `<a>` elements in HTML.
//! See [*§ 4.5.19 The `sub` and `sup` elements*][html_sup] and
//! [*§ 4.5.1 The `a` element*][html_a] in the HTML spec for more info.
//!
//! ## Recommendation
//!
//! It is recommended to use labels for links instead of [autolinks][autolink].
//! Labels allow more characters in URLs, and allow relative URLs and `www.`
//! URLs.
//! They also allow for descriptive text to explain the URL in prose.
//!
//! In footnotes, it’s recommended to use words instead of numbers (or letters
//! or anything with an order) as calls.
//! That makes it easier to reuse and reorder footnotes.
//!
//! ## Tokens
//!
//! * [`Data`][Name::Data]
//! * [`FootnoteCall`][Name::FootnoteCall]
//! * [`Image`][Name::Image]
//! * [`Label`][Name::Label]
//! * [`LabelEnd`][Name::LabelEnd]
//! * [`LabelMarker`][Name::LabelMarker]
//! * [`LabelText`][Name::LabelText]
//! * [`LineEnding`][Name::LineEnding]
//! * [`Link`][Name::Link]
//! * [`Reference`][Name::Reference]
//! * [`ReferenceMarker`][Name::ReferenceMarker]
//! * [`ReferenceText`][Name::ReferenceText]
//! * [`Resource`][Name::Resource]
//! * [`ResourceDestination`][Name::ResourceDestination]
//! * [`ResourceDestinationLiteral`][Name::ResourceDestinationLiteral]
//! * [`ResourceDestinationLiteralMarker`][Name::ResourceDestinationLiteralMarker]
//! * [`ResourceDestinationRaw`][Name::ResourceDestinationRaw]
//! * [`ResourceDestinationText`][Name::ResourceDestinationText]
//! * [`ResourceMarker`][Name::ResourceMarker]
//! * [`ResourceTitle`][Name::ResourceTitle]
//! * [`ResourceTitleMarker`][Name::ResourceTitleMarker]
//! * [`ResourceTitleText`][Name::ResourceTitleText]
//! * [`SpaceOrTab`][Name::SpaceOrTab]
//!
//! ## References
//!
//! * [`label-end.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/label-end.js)
//! * [`micromark-extension-gfm-task-list-item`](https://github.com/micromark/micromark-extension-gfm-footnote)
//! * [*§ 4.7 Link reference definitions* in `CommonMark`](https://spec.commonmark.org/0.31/#link-reference-definitions)
//! * [*§ 6.3 Links* in `CommonMark`](https://spec.commonmark.org/0.31/#links)
//! * [*§ 6.4 Images* in `CommonMark`](https://spec.commonmark.org/0.31/#images)
//!
//! > 👉 **Note**: Footnotes are not specified in GFM yet.
//! > See [`github/cmark-gfm#270`](https://github.com/github/cmark-gfm/issues/270)
//! > for the related issue.
//!
//! [plain]: crate::construct::plain
//! [text]: crate::construct::text
//! [destination]: crate::construct::partial_destination
//! [title]: crate::construct::partial_title
//! [label]: crate::construct::partial_label
//! [label_start_picture]: crate::construct::label_start_picture
//! [label_start_link]: crate::construct::label_start_link
//! [label_start_footnote]: crate::construct::label_start_footnote
//! [definition]: crate::construct::definition
//! [autolink]: crate::construct::autolink
//! [normalize_identifier]: crate::util::normalize_identifier::normalize_identifier
//! [html_a]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-a-element
//! [html_img]: https://html.spec.whatwg.org/multipage/embedded-content.html#the-img-element
//! [html_sup]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-sub-and-sup-elements

use crate::choice::Starts;
use crate::construct::partial_space_or_tab_eol::space_or_tab_eol;
use crate::construct::{
    citation, partial_attributes, partial_destination, partial_label, partial_title,
};
use crate::event::{Event, Kind, Name};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::{Fallback, Label, LabelKind, LabelStart, Tokenizer};
use crate::util::{
    constant::RESOURCE_DESTINATION_BALANCE_MAX,
    normalize_identifier::normalize_identifier,
    skip,
    slice::{Position, Slice},
};
use alloc::{string::String, vec, vec::Vec};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Bytes(b"]");

/// Start of label end.
///
/// ```markdown
/// > | [a](b) c
///       ^
/// > | [a][b] c
///       ^
/// > | [a][] b
///       ^
/// > | [a] b
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if Some(b']') == tokenizer.current {
        // If there is an okay opening:
        if !tokenizer.labels.starts.is_empty() {
            let label_start = tokenizer.labels.starts.last().unwrap();
            let end = tokenizer.events.len();

            // If the corresponding label (link) start is marked as inactive,
            // it means we’d be wrapping a link, like this:
            //
            // ```markdown
            // > | a [b [c](d) e](f) g.
            //                  ^
            // ```
            //
            // We can’t have that, so it’s just balanced brackets.
            // Only a bracketed span or citations may wrap a link.
            if label_start.inactive
                && label_start.kind != LabelKind::Link
                && tokenizer.parse_state.bytes.get(tokenizer.point.index + 1) != Some(&b'{')
            {
                return State::Redo(StateName::LabelEnd(Step::Nok));
            }

            tokenizer.enter(Name::LabelEnd);
            tokenizer.enter(Name::LabelMarker);
            tokenizer.consume();
            tokenizer.exit(Name::LabelMarker);
            tokenizer.exit(Name::LabelEnd);
            return State::Next(StateName::LabelEnd(Step::After { end }));
        }
    }

    State::Nok
}

/// After `]`.
///
/// ```markdown
/// > | [a](b) c
///       ^
/// > | [a][b] c
///       ^
/// > | [a][] b
///       ^
/// > | [a] b
///       ^
/// ```
fn after(tokenizer: &mut Tokenizer, end: usize) -> State {
    let start_index = tokenizer.labels.starts.len() - 1;
    let kind = tokenizer.labels.starts[start_index].kind;
    // Attributes make a bracketed span of a link, image, or footnote start,
    // which wins over a shortcut reference or a footnote call, and is all a
    // start inside a link may be.
    if tokenizer.current == Some(b'{') && kind != LabelKind::Note {
        tokenizer.labels.starts[start_index].kind = LabelKind::Span;
        tokenizer.attempt(
            State::Next(StateName::LabelEnd(Step::Done { end })),
            State::Next(StateName::LabelEnd(Step::Unspan { end, kind })),
        );
        return State::Redo(StateName::Attributes(partial_attributes::Step::Start {
            line: false,
        }));
    }
    State::Redo(StateName::LabelEnd(
        if tokenizer.labels.starts[start_index].inactive {
            Step::Cite {
                end,
                tail: true,
                defined: false,
            }
        } else {
            Step::Reference { end }
        },
    ))
}

/// After a label start that attributes did not follow: it is what it was.
///
/// ```markdown
/// > | [a]{.} b
///        ^
/// ```
fn unspan(tokenizer: &mut Tokenizer, end: usize, kind: LabelKind) -> State {
    let index = tokenizer.labels.starts.len() - 1;
    tokenizer.labels.starts[index].kind = kind;
    State::Redo(StateName::LabelEnd(
        if tokenizer.labels.starts[index].inactive {
            Step::Cite {
                end,
                tail: true,
                defined: false,
            }
        } else {
            Step::Reference { end }
        },
    ))
}

/// After `]`, at what makes the label a link, an image, or a footnote call.
///
/// ```markdown
/// > | [a](b) c
///        ^
/// > | [a] b
///        ^
/// ```
fn reference(tokenizer: &mut Tokenizer, end: usize) -> State {
    let start_index = tokenizer.labels.starts.len() - 1;
    let start = &tokenizer.labels.starts[start_index];

    let indices = (
        tokenizer.events[start.start.1].point.index,
        tokenizer.events[end].point.index,
    );

    // We don’t care about virtual spaces, so `indices` and `as_str` are fine.
    let mut id = normalize_identifier(
        Slice::from_indices(tokenizer.parse_state.bytes, indices.0, indices.1).as_str(),
    );

    // An inline note holds more than spaces and tabs, and takes nothing
    // after it.
    if start.kind == LabelKind::Note {
        let body = &tokenizer.parse_state.bytes[indices.0..indices.1];
        return State::Redo(if body.iter().all(|byte| matches!(byte, b'\t' | b' ')) {
            StateName::LabelEnd(Step::Nok)
        } else {
            StateName::LabelEnd(Step::Ok { end })
        });
    }

    // See if this matches a footnote definition.
    if start.kind == LabelKind::Footnote {
        if tokenizer.parse_state.definitions.notes.contains(&id) {
            // A resource or a full reference makes it a link all the same.
            if matches!(tokenizer.current, Some(b'(' | b'[')) {
                tokenizer.labels.starts[start_index].kind = LabelKind::UndefinedFootnote;
                tokenizer.attempt(
                    State::Next(StateName::LabelEnd(Step::Ok { end })),
                    State::Next(StateName::LabelEnd(Step::Call { end })),
                );
                return State::Redo(StateName::LabelEnd(if tokenizer.current == Some(b'(') {
                    Step::ResourceStart
                } else {
                    Step::ReferenceFull
                }));
            }
            return State::Redo(StateName::LabelEnd(Step::Ok { end }));
        }

        // Nope, this might be a normal link?
        tokenizer.labels.starts[start_index].kind = LabelKind::UndefinedFootnote;
        let mut new_id = String::new();
        new_id.push('^');
        new_id.push_str(&id);
        id = new_id;
    }

    let defined = tokenizer.parse_state.definitions.links.contains(&id);

    match tokenizer.current {
        // Resource (`[asd](fgh)`)?
        Some(b'(') => {
            tokenizer.attempt(
                State::Next(StateName::LabelEnd(Step::Ok { end })),
                State::Next(if defined {
                    StateName::LabelEnd(Step::Ok { end })
                } else {
                    StateName::LabelEnd(Step::Nok)
                }),
            );
            State::Redo(StateName::LabelEnd(Step::ResourceStart))
        }
        // Full (`[asd][fgh]`) or collapsed (`[asd][]`) reference?
        Some(b'[') => {
            tokenizer.attempt(
                State::Next(StateName::LabelEnd(Step::Ok { end })),
                State::Next(if defined {
                    StateName::LabelEnd(Step::ReferenceNotFull { end })
                } else {
                    StateName::LabelEnd(Step::Cite {
                        end,
                        tail: false,
                        defined: false,
                    })
                }),
            );
            State::Redo(StateName::LabelEnd(Step::ReferenceFull))
        }
        // Shortcut (`[asd]`) reference, a citation group, or a tail?
        _ => State::Redo(StateName::LabelEnd(Step::Cite {
            end,
            tail: true,
            defined,
        })),
    }
}

/// After `]`, where no resource or full reference matched: a citation
/// tail, when a key is right before the start and nothing but a shortcut
/// reference could follow (`tail`), then a citation group, then a defined
/// shortcut reference.
///
/// ```markdown
/// > | a @b [c] d
///             ^
/// > | a [@b; @c] d
///               ^
/// ```
fn cite(tokenizer: &mut Tokenizer, end: usize, tail: bool, defined: bool) -> State {
    let index = tokenizer.labels.starts.len() - 1;
    let start = &tokenizer.labels.starts[index];
    let link = if defined {
        StateName::LabelEnd(Step::Ok { end })
    } else {
        StateName::LabelEnd(Step::Nok)
    };
    if start.kind != LabelKind::Link {
        return State::Redo(link);
    }
    let bytes = tokenizer.parse_state.bytes;
    let sections = citation::sections(
        &tokenizer.events,
        bytes,
        &tokenizer.labels.pairs,
        start.start.1,
        end,
    );
    let group = citation::group(&sections);
    let author = if tail && citation::tail(&sections) {
        citation::author(&tokenizer.events, bytes, start.start.0)
    } else {
        None
    };
    let inactive = start.inactive;
    if let Some(author) = author {
        tokenizer.labels.starts[index].kind = LabelKind::Tail {
            author,
            fallback: if defined && !inactive {
                Fallback::Link
            } else if group {
                Fallback::Group
            } else {
                Fallback::Data
            },
        };
    } else if group {
        tokenizer.labels.starts[index].kind = LabelKind::Group;
    } else if inactive {
        return State::Redo(StateName::LabelEnd(Step::Nok));
    } else {
        return State::Redo(link);
    }
    State::Redo(StateName::LabelEnd(Step::Done { end }))
}

/// After a defined footnote call that no resource follows.
///
/// ```markdown
/// > | [^a] b
///         ^
/// ```
fn call(tokenizer: &mut Tokenizer, end: usize) -> State {
    let index = tokenizer.labels.starts.len() - 1;
    tokenizer.labels.starts[index].kind = LabelKind::Footnote;
    State::Redo(StateName::LabelEnd(Step::Ok { end }))
}

/// After `]`, at `[`, but not at a full reference.
///
/// > 👉 **Note**: we only get here if the label is defined.
///
/// ```markdown
/// > | [a][] b
///        ^
/// > | [a] b
///        ^
/// ```
fn reference_not_full(tokenizer: &mut Tokenizer, end: usize) -> State {
    tokenizer.attempt(
        State::Next(StateName::LabelEnd(Step::Ok { end })),
        State::Next(StateName::LabelEnd(Step::Nok)),
    );
    State::Redo(StateName::LabelEnd(Step::ReferenceCollapsed))
}

/// Done, we found something.
///
/// ```markdown
/// > | [a](b) c
///           ^
/// > | [a][b] c
///           ^
/// > | [a][] b
///          ^
/// > | [a] b
///        ^
/// ```
fn ok(tokenizer: &mut Tokenizer, end: usize) -> State {
    let kind = &tokenizer.labels.starts[tokenizer.labels.starts.len() - 1].kind;
    let media = matches!(
        kind,
        LabelKind::Link | LabelKind::Image | LabelKind::UndefinedFootnote
    );
    if tokenizer.current == Some(b'{') && media {
        tokenizer.attempt(
            State::Next(StateName::LabelEnd(Step::Done { end })),
            State::Next(StateName::LabelEnd(Step::Done { end })),
        );
        State::Redo(StateName::Attributes(partial_attributes::Step::Start {
            line: false,
        }))
    } else {
        State::Redo(StateName::LabelEnd(Step::Done { end }))
    }
}

/// Done, with the attributes of a link or an image.
///
/// ```markdown
/// > | [a](b){.c} d
///               ^
/// ```
fn done(tokenizer: &mut Tokenizer, end: usize) -> State {
    // Remove the start.
    let label_start = tokenizer.labels.starts.pop().unwrap();

    // If this is a link or footnote, we need to mark earlier link starts as no
    // longer viable for use (as they would otherwise contain a link).
    // These link starts are still looking for balanced closing brackets, so
    // we can’t remove them, but we can mark them.
    // An inline note holds its own links: they do not reach past it, and the
    // note is no link either.
    // A bracketed span is no link either.
    // Nor are citations.
    if !matches!(
        label_start.kind,
        LabelKind::Image
            | LabelKind::Note
            | LabelKind::Span
            | LabelKind::Group
            | LabelKind::Tail { .. }
    ) {
        let mut index = tokenizer.labels.starts.len();
        while index > 0 {
            index -= 1;
            let label_start = &mut tokenizer.labels.starts[index];
            match label_start.kind {
                LabelKind::Note => break,
                LabelKind::Image => {}
                _ => label_start.inactive = true,
            }
        }
    }

    tokenizer.labels.pairs.push(Label {
        kind: label_start.kind,
        start: label_start.start,
        end: (end, tokenizer.events.len() - 1),
    });
    tokenizer.register_resolver_before(ResolveName::Label);
    State::Ok
}

/// Done, it’s nothing.
///
/// There was an okay opening, but we didn’t match anything.
///
/// ```markdown
/// > | [a](b c
///        ^
/// > | [a][b c
///        ^
/// > | [a] b
///        ^
/// ```
fn nok(tokenizer: &mut Tokenizer) -> State {
    let start = tokenizer.labels.starts.pop().unwrap();
    tokenizer.labels.loose.push(start);
    State::Nok
}

/// At a resource.
///
/// ```markdown
/// > | [a](b) c
///        ^
/// ```
fn resource_start(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'(') => {
            tokenizer.enter(Name::Resource);
            tokenizer.enter(Name::ResourceMarker);
            tokenizer.consume();
            tokenizer.exit(Name::ResourceMarker);
            State::Next(StateName::LabelEnd(Step::ResourceBefore))
        }
        _ => unreachable!("expected `(`"),
    }
}

/// In resource, after `(`, at optional whitespace.
///
/// ```markdown
/// > | [a](b) c
///         ^
/// ```
fn resource_before(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b'\n' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::LabelEnd(Step::ResourceOpen)),
            State::Next(StateName::LabelEnd(Step::ResourceOpen)),
        );
        State::Redo(space_or_tab_eol())
    } else {
        State::Redo(StateName::LabelEnd(Step::ResourceOpen))
    }
}

/// In resource, after optional whitespace, at `)` or a destination.
///
/// ```markdown
/// > | [a](b) c
///         ^
/// ```
fn resource_open(tokenizer: &mut Tokenizer) -> State {
    if let Some(b')') = tokenizer.current {
        State::Redo(StateName::LabelEnd(Step::ResourceEnd))
    } else {
        tokenizer.attempt(
            State::Next(StateName::LabelEnd(Step::ResourceDestinationAfter)),
            State::Nok,
        );
        State::Redo(StateName::Destination(partial_destination::Step::Start(
            partial_destination::Options {
                names: partial_destination::Names {
                    whole: Name::ResourceDestination,
                    literal: Name::ResourceDestinationLiteral,
                    marker: Name::ResourceDestinationLiteralMarker,
                    raw: Name::ResourceDestinationRaw,
                    text: Name::ResourceDestinationText,
                },
                limit: RESOURCE_DESTINATION_BALANCE_MAX,
            },
        )))
    }
}

/// In resource, after destination, at optional whitespace.
///
/// ```markdown
/// > | [a](b) c
///          ^
/// ```
fn resource_destination_after(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b'\n' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::LabelEnd(Step::ResourceBetween)),
            State::Next(StateName::LabelEnd(Step::ResourceEnd)),
        );
        State::Redo(space_or_tab_eol())
    } else {
        State::Redo(StateName::LabelEnd(Step::ResourceEnd))
    }
}

/// In resource, after destination and whitespace, at `(` or title.
///
/// ```markdown
/// > | [a](b ) c
///           ^
/// ```
fn resource_between(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'"' | b'\'' | b'(') => {
            tokenizer.attempt(
                State::Next(StateName::LabelEnd(Step::ResourceTitleAfter)),
                State::Nok,
            );
            State::Redo(StateName::Title(partial_title::Step::Start(
                partial_title::Names {
                    whole: Name::ResourceTitle,
                    marker: Name::ResourceTitleMarker,
                    text: Name::ResourceTitleText,
                },
            )))
        }
        _ => State::Redo(StateName::LabelEnd(Step::ResourceEnd)),
    }
}

/// In resource, after title, at optional whitespace.
///
/// ```markdown
/// > | [a](b "c") d
///              ^
/// ```
fn resource_title_after(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b'\n' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::LabelEnd(Step::ResourceEnd)),
            State::Next(StateName::LabelEnd(Step::ResourceEnd)),
        );
        State::Redo(space_or_tab_eol())
    } else {
        State::Redo(StateName::LabelEnd(Step::ResourceEnd))
    }
}

/// In resource, at `)`.
///
/// ```markdown
/// > | [a](b) d
///          ^
/// ```
fn resource_end(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b')') => {
            tokenizer.enter(Name::ResourceMarker);
            tokenizer.consume();
            tokenizer.exit(Name::ResourceMarker);
            tokenizer.exit(Name::Resource);
            State::Ok
        }
        _ => State::Nok,
    }
}

/// In reference (full), at `[`.
///
/// ```markdown
/// > | [a][b] d
///        ^
/// ```
fn reference_full(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'[') => {
            tokenizer.attempt(
                State::Next(StateName::LabelEnd(Step::ReferenceFullAfter)),
                State::Nok,
            );
            State::Redo(StateName::Label(partial_label::Step::Start(
                partial_label::Names {
                    whole: Name::Reference,
                    marker: Name::ReferenceMarker,
                    text: Name::ReferenceText,
                },
            )))
        }
        _ => unreachable!("expected `[`"),
    }
}

/// In reference (full), after `]`.
///
/// ```markdown
/// > | [a][b] d
///          ^
/// ```
fn reference_full_after(tokenizer: &mut Tokenizer) -> State {
    if tokenizer
        .parse_state
        .definitions
        .links
        // We don’t care about virtual spaces, so `as_str` is fine.
        .contains(&normalize_identifier(
            Slice::from_position(
                tokenizer.parse_state.bytes,
                &Position::from_exit_event(
                    &tokenizer.events,
                    skip::to_back(
                        &tokenizer.events,
                        tokenizer.events.len() - 1,
                        &[Name::ReferenceText],
                    ),
                ),
            )
            .as_str(),
        ))
    {
        State::Ok
    } else {
        State::Nok
    }
}

/// In reference (collapsed), at `[`.
///
/// > 👉 **Note**: we only get here if the label is defined.
///
/// ```markdown
/// > | [a][] d
///        ^
/// ```
fn reference_collapsed(tokenizer: &mut Tokenizer) -> State {
    // We only attempt a collapsed label if there’s a `[`.
    debug_assert_eq!(tokenizer.current, Some(b'['), "expected opening bracket");
    tokenizer.enter(Name::Reference);
    tokenizer.enter(Name::ReferenceMarker);
    tokenizer.consume();
    tokenizer.exit(Name::ReferenceMarker);
    State::Next(StateName::LabelEnd(Step::ReferenceCollapsedOpen))
}

/// In reference (collapsed), at `]`.
///
/// > 👉 **Note**: we only get here if the label is defined.
///
/// ```markdown
/// > | [a][] d
///         ^
/// ```
fn reference_collapsed_open(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b']') => {
            tokenizer.enter(Name::ReferenceMarker);
            tokenizer.consume();
            tokenizer.exit(Name::ReferenceMarker);
            tokenizer.exit(Name::Reference);
            State::Ok
        }
        _ => State::Nok,
    }
}

/// Resolve images, links, and footnotes.
///
/// This turns matching label starts and label ends into links, images, and
/// footnotes, and turns unmatched label starts back into data.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    // Inject labels.
    let mut labels = tokenizer.labels.pairs.split_off(0);
    let dropped = settle(tokenizer, &mut labels);
    insert_labels(tokenizer, &labels);
    mark_as_data(tokenizer, &dropped);
    // Handle loose starts.
    let starts = tokenizer.labels.starts.split_off(0);
    mark_as_data(tokenizer, &starts);
    let starts = tokenizer.labels.loose.split_off(0);
    mark_as_data(tokenizer, &starts);

    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// Settle citation tails, from the outside in: a tail whose key is the
/// referent of a group around it is what it is without the key.
/// Returns the starts and ends of tails that are then brackets as text,
/// which are taken out of `labels`.
fn settle(tokenizer: &Tokenizer, labels: &mut Vec<Label>) -> Vec<LabelStart> {
    let mut referents = vec![];
    let mut dropped = vec![];
    let mut index = labels.len();
    while index > 0 {
        index -= 1;
        if let LabelKind::Tail { author, fallback } = labels[index].kind {
            if referents.contains(&author) {
                labels[index].kind = match fallback {
                    Fallback::Link => LabelKind::Link,
                    Fallback::Group => LabelKind::Group,
                    Fallback::Data => {
                        let label = labels.remove(index);
                        for range in [label.start, label.end] {
                            dropped.push(LabelStart {
                                kind: LabelKind::Link,
                                start: range,
                                inactive: false,
                            });
                        }
                        continue;
                    }
                };
            }
        }
        if matches!(
            labels[index].kind,
            LabelKind::Group | LabelKind::Tail { .. }
        ) {
            let sections = citation::sections(
                &tokenizer.events,
                tokenizer.parse_state.bytes,
                &labels[..index],
                labels[index].start.1,
                labels[index].end.0,
            );
            referents.extend(sections.iter().filter_map(|section| section.key));
        }
    }
    dropped
}

/// Inject links/images/footnotes.
fn insert_labels(tokenizer: &mut Tokenizer, labels: &[Label]) {
    // Add grouping events.
    let mut index = 0;
    while index < labels.len() {
        let label = &labels[index];
        match label.kind {
            LabelKind::Group => {
                citation::insert(tokenizer, label, None, &labels[..index]);
                index += 1;
                continue;
            }
            LabelKind::Tail { author, .. } => {
                citation::insert(tokenizer, label, Some(author), &labels[..index]);
                index += 1;
                continue;
            }
            _ => {}
        }
        let group_name = if label.kind == LabelKind::Footnote {
            Name::FootnoteCall
        } else if label.kind == LabelKind::Note {
            Name::Note
        } else if label.kind == LabelKind::Image {
            Name::Image
        } else if label.kind == LabelKind::Span {
            Name::Span
        } else {
            Name::Link
        };

        // If this is a fine link or span, which starts with a footnote start
        // that did not match, we need to inject the caret as text, where it
        // may be attention.
        let mut caret = vec![];
        let start_name = tokenizer.events[label.start.0].name;
        let span = label.kind == LabelKind::Span;

        // A bracketed span from an image start leaves its `!` as text before
        // it.
        if span && start_name == Name::LabelImage {
            let mark = vec![
                Event {
                    kind: Kind::Enter,
                    name: Name::Data,
                    point: tokenizer.events[label.start.0].point.clone(),
                    link: None,
                },
                Event {
                    kind: Kind::Exit,
                    name: Name::Data,
                    point: tokenizer.events[label.start.0 + 2].point.clone(),
                    link: None,
                },
            ];
            tokenizer.events[label.start.0].name = Name::LabelLink;
            tokenizer.events[label.start.1].name = Name::LabelLink;
            tokenizer.events[label.start.0].point =
                tokenizer.events[label.start.0 + 3].point.clone();
            // Remove the `!`: Enter:LabelImageMarker, Exit:LabelImageMarker.
            tokenizer.map.add(label.start.0 + 1, 2, vec![]);
            tokenizer.map.add(label.start.0, 0, mark);
        }

        if label.kind == LabelKind::UndefinedFootnote
            || (span && start_name == Name::FootnoteCallLabel)
        {
            // Add caret.
            caret.push(Event {
                kind: Kind::Enter,
                name: Name::AttentionSequence,
                // Enter:GfmFootnoteCallMarker.
                point: tokenizer.events[label.start.1 - 2].point.clone().clone(),
                link: None,
            });
            caret.push(Event {
                kind: Kind::Exit,
                name: Name::AttentionSequence,
                // Exit:GfmFootnoteCallMarker.
                point: tokenizer.events[label.start.1 - 1].point.clone(),
                link: None,
            });
            // Change and move label end.
            tokenizer.events[label.start.0].name = Name::LabelLink;
            tokenizer.events[label.start.1].name = Name::LabelLink;
            tokenizer.events[label.start.1].point = caret[0].point.clone();
            // Remove the caret.
            // Enter:GfmFootnoteCallMarker, Exit:GfmFootnoteCallMarker.
            tokenizer.map.add(label.start.1 - 2, 2, vec![]);
        }

        // Insert a group enter and label enter.
        tokenizer.map.add(
            label.start.0,
            0,
            vec![
                Event {
                    kind: Kind::Enter,
                    name: group_name,
                    point: tokenizer.events[label.start.0].point.clone(),
                    link: None,
                },
                Event {
                    kind: Kind::Enter,
                    name: Name::Label,
                    point: tokenizer.events[label.start.0].point.clone(),
                    link: None,
                },
            ],
        );

        // Empty events not allowed.
        // Though: if this was what looked like a footnote, but didn’t match,
        // it’s a link instead, and we need to inject the `^`.
        if label.start.1 != label.end.0 || !caret.is_empty() {
            tokenizer.map.add_before(
                label.start.1 + 1,
                0,
                vec![Event {
                    kind: Kind::Enter,
                    name: Name::LabelText,
                    point: tokenizer.events[label.start.1].point.clone(),
                    link: None,
                }],
            );
            tokenizer.map.add(
                label.end.0,
                0,
                vec![Event {
                    kind: Kind::Exit,
                    name: Name::LabelText,
                    point: tokenizer.events[label.end.0].point.clone(),
                    link: None,
                }],
            );
        }

        if !caret.is_empty() {
            tokenizer.map.add(label.start.1 + 1, 0, caret);
        }

        // Insert a label exit.
        tokenizer.map.add(
            label.end.0 + 4,
            0,
            vec![Event {
                kind: Kind::Exit,
                name: Name::Label,
                point: tokenizer.events[label.end.0 + 3].point.clone(),
                link: None,
            }],
        );

        // Insert a group exit.
        tokenizer.map.add(
            label.end.1 + 1,
            0,
            vec![Event {
                kind: Kind::Exit,
                name: group_name,
                point: tokenizer.events[label.end.1].point.clone(),
                link: None,
            }],
        );

        index += 1;
    }
}

/// Remove loose label starts.
fn mark_as_data(tokenizer: &mut Tokenizer, events: &[LabelStart]) {
    let mut index = 0;

    while index < events.len() {
        let data_enter_index = events[index].start.0;
        let data_exit_index = events[index].start.1;
        // The caret of a footnote start is text, where it may be attention.
        let caret = tokenizer.events[data_enter_index].name == Name::FootnoteCallLabel;
        let data_end = if caret {
            data_exit_index - 2
        } else {
            data_exit_index
        };
        let mut list = vec![
            Event {
                kind: Kind::Enter,
                name: Name::Data,
                point: tokenizer.events[data_enter_index].point.clone(),
                link: None,
            },
            Event {
                kind: Kind::Exit,
                name: Name::Data,
                point: tokenizer.events[data_end].point.clone(),
                link: None,
            },
        ];
        if caret {
            list.push(Event {
                kind: Kind::Enter,
                name: Name::AttentionSequence,
                point: tokenizer.events[data_exit_index - 2].point.clone(),
                link: None,
            });
            list.push(Event {
                kind: Kind::Exit,
                name: Name::AttentionSequence,
                point: tokenizer.events[data_exit_index].point.clone(),
                link: None,
            });
        }

        tokenizer.map.add(
            data_enter_index,
            data_exit_index - data_enter_index + 1,
            list,
        );

        index += 1;
    }
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    After {
        end: usize,
    },
    ResourceStart,
    ResourceBefore,
    ResourceOpen,
    ResourceDestinationAfter,
    ResourceBetween,
    ResourceTitleAfter,
    ResourceEnd,
    Ok {
        end: usize,
    },
    Done {
        end: usize,
    },
    Nok,
    ReferenceFull,
    ReferenceFullAfter,
    ReferenceNotFull {
        end: usize,
    },
    Call {
        end: usize,
    },
    Unspan {
        end: usize,
        kind: LabelKind,
    },
    Cite {
        end: usize,
        tail: bool,
        defined: bool,
    },
    Reference {
        end: usize,
    },
    ReferenceCollapsed,
    ReferenceCollapsedOpen,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::LabelEnd(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::After { end } => after(tokenizer, end),
        Step::ResourceStart => resource_start(tokenizer),
        Step::ResourceBefore => resource_before(tokenizer),
        Step::ResourceOpen => resource_open(tokenizer),
        Step::ResourceDestinationAfter => resource_destination_after(tokenizer),
        Step::ResourceBetween => resource_between(tokenizer),
        Step::ResourceTitleAfter => resource_title_after(tokenizer),
        Step::ResourceEnd => resource_end(tokenizer),
        Step::Ok { end } => ok(tokenizer, end),
        Step::Done { end } => done(tokenizer, end),
        Step::Nok => nok(tokenizer),
        Step::ReferenceFull => reference_full(tokenizer),
        Step::ReferenceFullAfter => reference_full_after(tokenizer),
        Step::ReferenceNotFull { end } => reference_not_full(tokenizer, end),
        Step::Call { end } => call(tokenizer, end),
        Step::Unspan { end, kind } => unspan(tokenizer, end, kind),
        Step::Cite { end, tail, defined } => cite(tokenizer, end, tail, defined),
        Step::Reference { end } => reference(tokenizer, end),
        Step::ReferenceCollapsed => reference_collapsed(tokenizer),
        Step::ReferenceCollapsedOpen => reference_collapsed_open(tokenizer),
    }
}
