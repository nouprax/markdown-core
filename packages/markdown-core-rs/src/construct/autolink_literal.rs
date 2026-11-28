//! GFM: autolink literal occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! Autolink literals form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! gfm_autolink_literal ::= gfm_protocol_autolink | gfm_www_autolink | gfm_email_autolink
//!
//! ; Restriction: the code before must be `www_autolink_before`, or the
//! ; literal starts an inline note.
//! ; Restriction: the code after `.` must not be eof.
//! www_autolink ::= 3('w' | 'W') '.' [domain [path]]
//! www_autolink_before ::= eof | eol | space_or_tab | '(' | '*' | '_' | '~'
//!
//! ; Restriction: the code before must be `http_autolink_before`.
//! ; Restriction: the code after the protocol must be `http_autolink_protocol_after`.
//! http_autolink ::= (('h' | 'H') 2('t' | 'T') ('p' | 'P') ['s' | 'S'] | ('f' | 'F') ('t' | 'T') ('p' | 'P')) ':' 2'/' domain [path]
//! http_autolink_before ::= byte - ascii_alpha
//! http_autolink_protocol_after ::= byte - eof - eol - ascii_control - unicode_whitespace - unicode_punctuation
//!
//! ; Restriction: the code before must be `email_autolink_before`.
//! ; Restriction: `ascii_digit` may not occur in the last label part of the label.
//! email_autolink ::= 1*('+' | '-' | '.' | '_' | ascii_alphanumeric) '@' 1*(1*label_segment label_dot_cont) 1*label_segment
//! email_autolink_before ::= byte - ascii_alpha
//!
//! ; Restriction: `_` may not occur in the last two domain parts.
//! domain ::= 1*(url_ampt_cont | domain_punct_cont | '-' | byte - eof - ascii_control - unicode_whitespace - unicode_punctuation)
//! ; Restriction: must not be followed by `punct`.
//! domain_punct_cont ::= '.' | '_'
//! ; Restriction: must not be followed by `char-ref`.
//! url_ampt_cont ::= '&'
//!
//! ; Restriction: a counter `balance = 0` is increased for every `(`, and decreased for every `)`.
//! ; Restriction: `)` must not be `paren_at_end`.
//! ; Restriction: in an inline note, `]` ends the path, unless `\` is before it.
//! path ::= 1*(url_ampt_cont | path_punctuation_cont | '(' | ')' | byte - eof - eol - space_or_tab)
//! ; Restriction: must not be followed by `punct`.
//! path_punctuation_cont ::= trailing_punctuation - '<'
//! ; Restriction: must be followed by `punct` and `balance` must be less than `0`.
//! paren_at_end ::= ')'
//!
//! label_segment ::= label_dash_underscore_cont | ascii_alpha | ascii_digit
//! ; Restriction: if followed by `punct`, the whole email autolink is invalid.
//! label_dash_underscore_cont ::= '-' | '_'
//! ; Restriction: must not be followed by `punct`.
//! label_dot_cont ::= '.'
//!
//! punct ::= *trailing_punctuation ( byte - eof - eol - space_or_tab - '<' )
//! char_ref ::= *ascii_alpha ';' path_end
//! trailing_punctuation ::= '!' | '"' | '\'' | ')' | '*' | ',' | '.' | ':' | ';' | '<' | '?' | '_' | '~'
//! ```
//!
//! The grammar for GFM autolink literal is very relaxed: basically anything
//! except for whitespace is allowed after a prefix.
//! To use whitespace characters and otherwise impossible characters, in URLs,
//! you can use percent encoding:
//!
//! ```markdown
//! https://example.com/alpha%20bravo
//! ```
//!
//! Yields:
//!
//! ```html
//! <p><a href="https://example.com/alpha%20bravo">https://example.com/alpha%20bravo</a></p>
//! ```
//!
//! There are several cases where incorrect encoding of URLs would, in other
//! languages, result in a parse error.
//! In markdown, there are no errors, and URLs are normalized.
//! In addition, many characters are percent encoded
//! (`sanitize_uri` of markdown-rs).
//! For example:
//!
//! ```markdown
//! www.a👍b%
//! ```
//!
//! Yields:
//!
//! ```html
//! <p><a href="http://www.a%F0%9F%91%8Db%25">www.a👍b%</a></p>
//! ```
//!
//! There is a big difference between how www and protocol literals work
//! compared to how email literals work.
//! The first two are done when parsing, and work like anything else in
//! markdown.
//! But email literals are handled afterwards: when everything is parsed, we
//! look back at the events to figure out if there were email addresses.
//! This particularly affects how they interleave with character escapes and
//! character references.
//!
//! ## HTML
//!
//! GFM autolink literals relate to the `<a>` element in HTML.
//! See [*§ 4.5.1 The `a` element*][html_a] in the HTML spec for more info.
//! When an email autolink is used, the string `mailto:` is prepended when
//! generating the `href` attribute of the hyperlink.
//! When a www autolink is used, the string `http:` is prepended.
//!
//! ## Recommendation
//!
//! It is recommended to use labels ([label start link][label_start_link],
//! [label end][label_end]), either with a resource or a definition
//! ([definition][]), instead of autolink literals, as those allow relative
//! URLs and descriptive text to explain the URL in prose.
//!
//! ## Bugs
//!
//! GitHub’s own algorithm to parse autolink literals contains three bugs.
//! A smaller bug is left unfixed in this project for consistency.
//! Two main bugs are not present in this project.
//! The issues relating to autolink literals are:
//!
//! * [GFM autolink extension (`www.`, `https?://` parts): links don’t work when after bracket](https://github.com/github/cmark-gfm/issues/278)\
//!   fixed here ✅
//! * [GFM autolink extension (`www.` part): uppercase does not match on issues/PRs/comments](https://github.com/github/cmark-gfm/issues/280)\
//!   fixed here ✅
//! * [GFM autolink extension (`www.` part): the word `www` matches](https://github.com/github/cmark-gfm/issues/279)\
//!   present here for consistency
//!
//! ## Tokens
//!
//! * [`AutolinkLiteralEmail`][Name::AutolinkLiteralEmail]
//! * [`AutolinkLiteralMailto`][Name::AutolinkLiteralMailto]
//! * [`AutolinkLiteralProtocol`][Name::AutolinkLiteralProtocol]
//! * [`AutolinkLiteralWww`][Name::AutolinkLiteralWww]
//! * [`AutolinkLiteralXmpp`][Name::AutolinkLiteralXmpp]
//!
//! ## References
//!
//! * [`micromark-extension-gfm-autolink-literal`](https://github.com/micromark/micromark-extension-gfm-autolink-literal)
//! * [*§ 6.9 Autolinks (extension)* in `GFM`](https://github.github.com/gfm/#autolinks-extension-)
//!
//! > 👉 **Note**: `mailto:` and `xmpp:` protocols before email autolinks were
//! > added in `cmark-gfm@0.29.0.gfm.5` and are as of yet undocumented.
//!
//! [text]: crate::construct::text
//! [definition]: crate::construct::definition
//! [attention]: crate::construct::attention
//! [label_start_link]: crate::construct::label_start_link
//! [label_end]: crate::construct::label_end
//! [html_a]: https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-a-element

use crate::choice::Starts;
use crate::event::{Event, Kind, Name};
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::{
    char::after_index,
    character_reference::decode_at,
    classes::{classes, PUNCTUATION},
    slice::Slice,
};
use alloc::vec::Vec;

/// Bytes the protocol kind (`https://a.b`) can start at.
pub const PROTOCOL: Starts = Starts::Bytes(b"FHfh");

/// Bytes the `www.` kind (`www.a.b`) can start at.
pub const WWW: Starts = Starts::Bytes(b"Ww");

/// Bytes an address claims (`mailto:a@b.c`, `x:a@b.c`): its colon.
pub const ADDRESS: Starts = Starts::Bytes(b":");

/// At a colon an email address follows, which owns it as text: a directive
/// name there is the address's.
///
/// The colon is data, and the address is linked with the text around it
/// when text resolves, as cmark-gfm links it.
///
/// ```markdown
/// > | mailto:a@b.c
///           ^
/// ```
fn address_start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.current == Some(b':')
        && address(tokenizer.parse_state.bytes, tokenizer.point.index + 1)
    {
        tokenizer.enter(Name::Data);
        tokenizer.consume();
        tokenizer.exit(Name::Data);
        State::Ok
    } else {
        State::Nok
    }
}

/// Whether an email address, as the colon claims it, starts at `start`: a
/// second `@` ends its domain (`:a@b.c@[]`), as it ends the one text links.
fn address(bytes: &[u8], start: usize) -> bool {
    let mut at = start;
    while at < bytes.len()
        && matches!(bytes[at], b'+' | b'-' | b'.' | b'0'..=b'9' | b'A'..=b'Z' | b'_' | b'a'..=b'z')
    {
        at += 1;
    }
    at > start && bytes.get(at) == Some(&b'@') && domain(bytes, at + 1, false).is_some()
}

/// Start of protocol autolink literal.
///
/// ```markdown
/// > | https://example.com/a?b#c
///     ^
/// ```
fn protocol_start(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'F' | b'H' | b'f' | b'h'))
            // Source: <https://github.com/github/cmark-gfm/blob/ef1cfcb/extensions/autolink.c#L214>.
            && !matches!(tokenizer.previous, Some(b'A'..=b'Z' | b'a'..=b'z'))
            // No literal inside an open label.
            && !tokenizer.in_label()
    {
        tokenizer.enter(Name::AutolinkLiteralProtocol);
        tokenizer.attempt(
            State::Next(StateName::AutolinkLiteral(Step::ProtocolAfter)),
            State::Nok,
        );
        tokenizer.attempt(
            State::Next(StateName::AutolinkLiteral(Step::DomainInside {
                domain: Domain::default(),
            })),
            State::Nok,
        );
        State::Redo(StateName::AutolinkLiteral(Step::ProtocolPrefixInside {
            start: tokenizer.point.index,
        }))
    } else {
        State::Nok
    }
}

/// After a protocol autolink literal.
///
/// ```markdown
/// > | https://example.com/a?b#c
///                              ^
/// ```
fn protocol_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::AutolinkLiteralProtocol);
    State::Ok
}

/// In protocol.
///
/// ```markdown
/// > | https://example.com/a?b#c
///     ^^^^^
/// ```
fn protocol_prefix_inside(tokenizer: &mut Tokenizer, start: usize) -> State {
    match tokenizer.current {
        Some(b'A'..=b'Z' | b'a'..=b'z')
            // `5` is size of `https`
            if tokenizer.point.index - start < 5 =>
        {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::ProtocolPrefixInside { start }))
        }
        Some(b':') => {
            let slice = Slice::from_indices(
                tokenizer.parse_state.bytes,
                start,
                tokenizer.point.index,
            );
            let name = slice.as_str().to_ascii_lowercase();

            if name == "http" || name == "https" || name == "ftp" {
                tokenizer.consume();
                State::Next(StateName::AutolinkLiteral(Step::ProtocolSlashesInside {
                    size: 0,
                }))
            } else {
                State::Nok
            }
        }
        _ => State::Nok,
    }
}

/// In protocol slashes.
///
/// ```markdown
/// > | https://example.com/a?b#c
///           ^^
/// ```
fn protocol_slashes_inside(tokenizer: &mut Tokenizer, size: usize) -> State {
    if tokenizer.current == Some(b'/') {
        tokenizer.consume();
        if size == 0 {
            State::Next(StateName::AutolinkLiteral(Step::ProtocolSlashesInside {
                size: 1,
            }))
        } else {
            State::Ok
        }
    } else {
        State::Nok
    }
}

/// Start of www autolink literal.
///
/// ```markdown
/// > | www.example.com/a?b#c
///     ^
/// ```
fn www_start(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'W' | b'w'))
            // Source: <https://github.com/github/cmark-gfm/blob/ef1cfcb/extensions/autolink.c#L156>.
            && (matches!(tokenizer.previous, None | Some(b'\t' | b'\n' | b' ' | b'(' | b'*' | b'_' | b'~'))
                // The start of an inline note is a start of text.
                || tokenizer.at_note_start())
            // No literal inside an open label.
            && !tokenizer.in_label()
    {
        tokenizer.enter(Name::AutolinkLiteralWww);
        tokenizer.attempt(
            State::Next(StateName::AutolinkLiteral(Step::WwwAfter)),
            State::Nok,
        );
        // Note: we *check*, so we can discard the `www.` we parsed.
        // If it worked, we consider it as a part of the domain.
        tokenizer.check(
            State::Next(StateName::AutolinkLiteral(Step::DomainInside {
                domain: Domain::default(),
            })),
            State::Nok,
        );
        State::Redo(StateName::AutolinkLiteral(Step::WwwPrefixInside {
            size: 0,
        }))
    } else {
        State::Nok
    }
}

/// After a www autolink literal.
///
/// ```markdown
/// > | www.example.com/a?b#c
///                          ^
/// ```
fn www_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::AutolinkLiteralWww);
    State::Ok
}

/// In www prefix.
///
/// ```markdown
/// > | www.example.com
///     ^^^^
/// ```
fn www_prefix_inside(tokenizer: &mut Tokenizer, size: usize) -> State {
    match tokenizer.current {
        Some(b'.') if size == 3 => {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::WwwPrefixAfter))
        }
        Some(b'W' | b'w') if size < 3 => {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::WwwPrefixInside {
                size: size + 1,
            }))
        }
        _ => State::Nok,
    }
}

/// After www prefix.
///
/// ```markdown
/// > | www.example.com
///         ^
/// ```
fn www_prefix_after(tokenizer: &mut Tokenizer) -> State {
    // If there is *anything*, we can link.
    if tokenizer.current.is_none() {
        State::Nok
    } else {
        State::Ok
    }
}

/// In domain.
///
/// ```markdown
/// > | https://example.com/a
///             ^^^^^^^^^^^
/// ```
fn domain_inside(tokenizer: &mut Tokenizer, mut domain: Domain) -> State {
    match tokenizer.current {
        // Check whether this marker, which is a trailing punctuation
        // marker, optionally followed by more trailing markers, and then
        // followed by an end.
        Some(b'.' | b'_') => {
            tokenizer.check(
                State::Next(StateName::AutolinkLiteral(Step::DomainAfter { domain })),
                State::Next(StateName::AutolinkLiteral(Step::DomainAtPunctuation {
                    domain,
                })),
            );
            State::Redo(StateName::AutolinkLiteral(Step::Trail))
        }
        // Dashes and continuation bytes are fine.
        Some(b'-' | 0x80..=0xBF) => {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::DomainInside { domain }))
        }
        _ => {
            // Source: <https://github.com/github/cmark-gfm/blob/ef1cfcb/extensions/autolink.c#L12>.
            if host(tokenizer.parse_state.bytes, tokenizer.point.index) {
                domain.seen = true;
                tokenizer.consume();
                State::Next(StateName::AutolinkLiteral(Step::DomainInside { domain }))
            } else {
                State::Redo(StateName::AutolinkLiteral(Step::DomainAfter { domain }))
            }
        }
    }
}

/// In domain, at potential trailing punctuation, that was not trailing.
///
/// ```markdown
/// > | https://example.com
///                    ^
/// ```
fn domain_at_punctuation(tokenizer: &mut Tokenizer, mut domain: Domain) -> State {
    // There is an underscore in the last segment of the domain
    if matches!(tokenizer.current, Some(b'_')) {
        domain.last = true;
    }
    // Otherwise, it’s a `.`: save the last segment underscore in the
    // penultimate segment slot.
    else {
        domain.penult = domain.last;
        domain.last = false;
    }

    tokenizer.consume();
    State::Next(StateName::AutolinkLiteral(Step::DomainInside { domain }))
}

/// After domain
///
/// ```markdown
/// > | https://example.com/a
///                        ^
/// ```
fn domain_after(_tokenizer: &mut Tokenizer, domain: Domain) -> State {
    // No underscores allowed in last two segments.
    if domain.penult
        || domain.last
        // At least one character must be seen.
        || !domain.seen
    // Note: that’s GH says a dot is needed, but it’s not true:
    // <https://github.com/github/cmark-gfm/issues/279>
    {
        State::Nok
    } else {
        State::Redo(StateName::AutolinkLiteral(Step::PathInside {
            path: Path::default(),
        }))
    }
}

/// In path.
///
/// ```markdown
/// > | https://example.com/a
///                        ^^
/// ```
fn path_inside(tokenizer: &mut Tokenizer, mut path: Path) -> State {
    match tokenizer.current {
        // Continuation bytes are fine, we’ve already checked the first one.
        Some(0x80..=0xBF) => {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::PathInside { path }))
        }
        // In an inline note, `]` is the end, and `\]` is not.
        Some(b']') if !tokenizer.labels.starts.is_empty() => State::Ok,
        Some(b'\\') if !tokenizer.labels.starts.is_empty() => {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::PathEscape { path }))
        }
        // Count opening parens.
        Some(b'(') => {
            path.open += 1;
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::PathInside { path }))
        }
        // Check whether this trailing punctuation marker is optionally
        // followed by more trailing markers, and then followed
        // by an end.
        // If this is a paren (followed by trailing, then the end), we
        // *continue* if we saw less closing parens than opening parens.
        Some(
            b'!' | b'"' | b'&' | b'\'' | b')' | b'*' | b',' | b'.' | b':' | b';' | b'<' | b'?'
            | b'_' | b'~',
        ) => {
            let next = if tokenizer.current == Some(b')') && path.close < path.open {
                State::Next(StateName::AutolinkLiteral(Step::PathAtPunctuation { path }))
            } else {
                State::Ok
            };
            tokenizer.check(
                next,
                State::Next(StateName::AutolinkLiteral(Step::PathAtPunctuation { path })),
            );
            State::Redo(StateName::AutolinkLiteral(Step::Trail))
        }
        _ => {
            // Source: <https://github.com/github/cmark-gfm/blob/ef1cfcb/extensions/autolink.c#L12>.
            // Whitespace, a space, a tab or a line ending, ends the path.
            if matches!(tokenizer.current, None | Some(b'\t' | b'\n' | b'\r' | b' ')) {
                State::Ok
            } else {
                tokenizer.consume();
                State::Next(StateName::AutolinkLiteral(Step::PathInside { path }))
            }
        }
    }
}

/// In path in an inline note, after a `\`.
///
/// ```markdown
/// > | ^[https://example.com/a\]b]
///                             ^
/// ```
fn path_escape(tokenizer: &mut Tokenizer, path: Path) -> State {
    // The `]` or `\` it escapes is no end and no escape; other punctuation
    // is still trailing punctuation.
    if matches!(tokenizer.current, Some(b'\\' | b']')) {
        tokenizer.consume();
        State::Next(StateName::AutolinkLiteral(Step::PathInside { path }))
    } else {
        State::Redo(StateName::AutolinkLiteral(Step::PathInside { path }))
    }
}

/// In path, at potential trailing punctuation, that was not trailing.
///
/// ```markdown
/// > | https://example.com/a"b
///                          ^
/// ```
fn path_at_punctuation(tokenizer: &mut Tokenizer, mut path: Path) -> State {
    // Count closing parens.
    if tokenizer.current == Some(b')') {
        path.close += 1;
    }

    tokenizer.consume();
    State::Next(StateName::AutolinkLiteral(Step::PathInside { path }))
}

/// In trail of domain or path.
///
/// ```markdown
/// > | https://example.com").
///                        ^
/// ```
fn trail(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // Regular trailing punctuation.
        Some(
            b'!' | b'"' | b'\'' | b')' | b'*' | b',' | b'.' | b':' | b';' | b'?' | b'_' | b'~',
        ) => {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::Trail))
        }
        // `&` followed by one or more alphabeticals and then a `;`, is
        // as a whole considered as trailing punctuation.
        // In all other cases, it is considered as continuation of the URL.
        Some(b'&') => {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::TrailCharRefStart))
        }
        // `<` is an end, and so is `]` in an inline note.
        Some(b'<') => State::Ok,
        Some(b']') if !tokenizer.labels.starts.is_empty() => State::Ok,
        _ => {
            // Whitespace, a space, a tab or a line ending, is the end of the
            // URL, anything else is continuation.
            if matches!(tokenizer.current, None | Some(b'\t' | b'\n' | b'\r' | b' ')) {
                State::Ok
            } else {
                State::Nok
            }
        }
    }
}

/// In character-reference like trail, after `&`.
///
/// ```markdown
/// > | https://example.com&amp;).
///                         ^
/// ```
fn trail_char_ref_start(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'A'..=b'Z' | b'a'..=b'z')) {
        State::Redo(StateName::AutolinkLiteral(Step::TrailCharRefInside))
    } else {
        State::Nok
    }
}

/// In character-reference like trail.
///
/// ```markdown
/// > | https://example.com&amp;).
///                         ^
/// ```
fn trail_char_ref_inside(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'A'..=b'Z' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::TrailCharRefInside))
        }
        // Switch back to trail if this is well-formed.
        Some(b';') => {
            tokenizer.consume();
            State::Next(StateName::AutolinkLiteral(Step::Trail))
        }
        _ => State::Nok,
    }
}

/// Whether the character at `index` is in a domain: a domain ends at
/// whitespace, which is a space, a tab or a line ending, or at punctuation;
/// any other character, a symbol or a form feed included, is in it.
fn host(bytes: &[u8], index: usize) -> bool {
    match bytes.get(index) {
        None | Some(b'\t' | b'\n' | b'\r' | b' ') => false,
        Some(byte) if byte.is_ascii() => !byte.is_ascii_punctuation(),
        Some(_) => after_index(bytes, index).map_or(false, |char| classes(char) & PUNCTUATION == 0),
    }
}

/// Resolve: postprocess text to find email autolink literals.
///
/// An address is read in the text that a run of data, escapes and character
/// references leaves, as cmark-gfm reads its merged text: `\_@b.c` is the
/// address `_@b.c`.
pub fn resolve(tokenizer: &mut Tokenizer) {
    tokenizer.map.consume(&mut tokenizer.events);

    let mut index = 0;
    let mut links = 0;

    while index < tokenizer.events.len() {
        let event = &tokenizer.events[index];

        if event.kind == Kind::Enter && links == 0 && piece(event.name) {
            index = run(tokenizer, index);
            continue;
        }

        if event.name == Name::Link {
            if event.kind == Kind::Enter {
                links += 1;
            } else {
                links -= 1;
            }
        }

        index += 1;
    }
}

/// Whether a token of `name` is a piece of the text an address is read in.
fn piece(name: Name) -> bool {
    matches!(
        name,
        Name::Data | Name::CharacterEscape | Name::CharacterReference
    )
}

/// A piece of a run of text.
struct Piece {
    /// Its enter event.
    first: usize,
    /// Its exit event.
    last: usize,
    /// Where its bytes start in the source.
    start: usize,
    /// Where its bytes end in the source.
    end: usize,
    /// Where its text starts in the run's text.
    at: usize,
    /// Whether it is data, whose text is its bytes.
    data: bool,
}

/// Link the addresses in the run of text pieces whose first enter event is
/// `first`; the index of the event after the run.
fn run(tokenizer: &mut Tokenizer, first: usize) -> usize {
    let events = &tokenizer.events;
    let bytes = tokenizer.parse_state.bytes;
    let mut pieces: Vec<Piece> = Vec::new();
    let mut text = Vec::new();
    // The piece each byte of `text` is in.
    let mut owner = Vec::new();
    let mut index = first;

    while index < events.len() && events[index].kind == Kind::Enter && piece(events[index].name) {
        let name = events[index].name;
        let mut last = index + 1;
        while !(events[last].kind == Kind::Exit && events[last].name == name) {
            last += 1;
        }
        let start = events[index].point.index;
        let end = events[last].point.index;
        let at = text.len();
        match name {
            Name::Data => text.extend_from_slice(&bytes[start..end]),
            Name::CharacterEscape => text.push(bytes[end - 1]),
            _ => {
                let value = core::str::from_utf8(&bytes[start..end])
                    .ok()
                    .and_then(|source| decode_at(source, 0))
                    .map(|(value, _)| value);
                match value {
                    Some(value) => text.extend_from_slice(value.as_bytes()),
                    None => text.extend_from_slice(&bytes[start..end]),
                }
            }
        }
        owner.resize(text.len(), pieces.len());
        pieces.push(Piece {
            first: index,
            last,
            start,
            end,
            at,
            data: name == Name::Data,
        });
        index = last + 1;
    }

    let after = index;
    let spans = addresses(&text);

    if spans.is_empty() {
        return after;
    }

    // Where in the source each span starts and ends: a piece that is not
    // data is taken whole.
    let mut cuts = Vec::new();
    for (start, end, name) in spans {
        let head = &pieces[owner[start]];
        let tail = &pieces[owner[end - 1]];
        let from = if head.data {
            head.start + start - head.at
        } else {
            head.start
        };
        let to = if tail.data {
            tail.start + end - tail.at
        } else {
            tail.end
        };
        cuts.push((from, Kind::Enter, name));
        cuts.push((to, Kind::Exit, name));
    }

    let mut replace = Vec::new();
    let mut cut = 0;
    let mut point = events[first].point.clone();

    for piece in &pieces {
        let mut at = piece.start;

        while cut < cuts.len() && cuts[cut].0 < piece.end {
            let (place, kind, name) = cuts[cut].clone();
            if piece.data && place > at {
                replace.push(Event {
                    kind: Kind::Enter,
                    name: Name::Data,
                    point: point.clone(),
                    link: None,
                });
                point = point.shift_to(bytes, place);
                replace.push(Event {
                    kind: Kind::Exit,
                    name: Name::Data,
                    point: point.clone(),
                    link: None,
                });
                at = place;
            }
            replace.push(Event {
                kind,
                name,
                point: point.clone(),
                link: None,
            });
            cut += 1;
        }

        if piece.data {
            if at < piece.end {
                replace.push(Event {
                    kind: Kind::Enter,
                    name: Name::Data,
                    point: point.clone(),
                    link: None,
                });
                replace.push(events[piece.last].clone());
            }
        } else {
            replace.extend_from_slice(&events[piece.first..=piece.last]);
        }
        point = events[piece.last].point.clone();
    }

    while cut < cuts.len() {
        let (_, kind, name) = cuts[cut].clone();
        replace.push(Event {
            kind,
            name,
            point: point.clone(),
            link: None,
        });
        cut += 1;
    }

    tokenizer.map.add(first, after - first, replace);
    after
}

/// The email addresses in `bytes`, as `(start, end, kind)`.
fn addresses(bytes: &[u8]) -> Vec<(usize, usize, Name)> {
    let mut spans = Vec::new();
    let mut byte_index = 0;
    let mut min = 0;

    while byte_index < bytes.len() {
        if bytes[byte_index] == b'@' {
            if let Some(start) = peek_bytes_atext(bytes, min, byte_index) {
                let (start, kind) = peek_protocol(bytes, min, start);

                // Note: normally we’d truncate trailing punctuation from
                // the link. However, email autolink literals cannot contain
                // any of those markers, except for `.`, but that can only
                // occur if it isn’t trailing. So we can ignore truncating
                // while postprocessing!
                if let Some(end) = peek_bytes_email_domain(
                    bytes,
                    byte_index + 1,
                    kind == Name::AutolinkLiteralXmpp,
                ) {
                    spans.push((start, end, kind));
                    byte_index = end;
                    min = end;
                }
            }
        }

        byte_index += 1;
    }

    spans
}

/// Move back past atext.
///
/// Moving back is only used when post processing text: so for the email address
/// algorithm.
///
/// ```markdown
/// > | a contact@example.org b
///              ^-- from
///       ^-- to
/// ```
fn peek_bytes_atext(bytes: &[u8], min: usize, end: usize) -> Option<usize> {
    let mut index = end;

    // Take simplified atext.
    // See `email_atext` in `autolink.rs` for a similar algorithm.
    // Source: <https://github.com/github/cmark-gfm/blob/ef1cfcb/extensions/autolink.c#L301>.
    while index > min
        && matches!(bytes[index - 1], b'+' | b'-' | b'.' | b'0'..=b'9' | b'A'..=b'Z' | b'_' | b'a'..=b'z')
    {
        index -= 1;
    }

    // Every preceding character is allowed, a slash included.
    if index == end {
        None
    } else {
        Some(index)
    }
}

/// Move back past a `mailto:` or `xmpp:` protocol.
///
/// Moving back is only used when post processing text: so for the email address
/// algorithm.
///
/// ```markdown
/// > | a mailto:contact@example.org b
///              ^-- from
///       ^-- to
/// ```
fn peek_protocol(bytes: &[u8], min: usize, end: usize) -> (usize, Name) {
    let mut index = end;

    if index > min && bytes[index - 1] == b':' {
        index -= 1;

        // Take alphanumerical.
        while index > min && matches!(bytes[index - 1], b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z') {
            index -= 1;
        }

        let slice = Slice::from_indices(bytes, index, end - 1);
        let name = slice.as_str().to_ascii_lowercase();

        if name == "xmpp" {
            return (index, Name::AutolinkLiteralXmpp);
        } else if name == "mailto" {
            return (index, Name::AutolinkLiteralMailto);
        }
    }

    (end, Name::AutolinkLiteralEmail)
}

/// Move past email domain.
///
/// Peeking like this only used when post processing text: so for the email
/// address algorithm.
///
/// ```markdown
/// > | a contact@example.org b
///               ^-- from
///                         ^-- to
/// ```
fn peek_bytes_email_domain(bytes: &[u8], start: usize, xmpp: bool) -> Option<usize> {
    // A second `@` in the run makes it no address.
    // Source: <https://github.com/github/cmark-gfm/blob/ef1cfcb/extensions/autolink.c#L326>
    domain(bytes, start, xmpp).filter(|end| bytes.get(*end) != Some(&b'@'))
}

/// Move past an email domain, to the byte that ends it.
fn domain(bytes: &[u8], start: usize, xmpp: bool) -> Option<usize> {
    let mut index = start;
    let mut dot = false;

    // Move past “domain”.
    // The reference code is a bit overly complex as it handles the `@`, of which there may be just one.
    // Source: <https://github.com/github/cmark-gfm/blob/ef1cfcb/extensions/autolink.c#L318>
    while index < bytes.len() {
        match bytes[index] {
            // Alphanumerical, `-`, and `_`.
            b'-' | b'0'..=b'9' | b'A'..=b'Z' | b'_' | b'a'..=b'z' => {}
            b'/' if xmpp => {}
            // Dot followed by alphanumerical (not `-` or `_`).
            b'.' if index + 1 < bytes.len()
                && matches!(bytes[index + 1], b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z') =>
            {
                dot = true;
            }
            _ => break,
        }

        index += 1;
    }

    // Domain must not be empty, must include a dot, and must end in alphabetical or `.`.
    // Source: <https://github.com/github/cmark-gfm/blob/ef1cfcb/extensions/autolink.c#L332>.
    if index > start && dot && matches!(bytes[index - 1], b'.' | b'A'..=b'Z' | b'a'..=b'z') {
        Some(index)
    } else {
        None
    }
}

/// What a domain has seen so far.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Domain {
    /// Whether a character other than punctuation was seen.
    seen: bool,
    /// Whether the last segment has an underscore.
    last: bool,
    /// Whether the segment before the last has an underscore.
    penult: bool,
}

/// The parens a path has seen so far.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Path {
    /// Opening parens.
    open: usize,
    /// Closing parens.
    close: usize,
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    AddressStart,
    ProtocolStart,
    ProtocolAfter,
    ProtocolPrefixInside { start: usize },
    ProtocolSlashesInside { size: usize },
    WwwAfter,
    WwwStart,
    WwwPrefixInside { size: usize },
    WwwPrefixAfter,
    DomainInside { domain: Domain },
    DomainAtPunctuation { domain: Domain },
    DomainAfter { domain: Domain },
    PathInside { path: Path },
    PathEscape { path: Path },
    PathAtPunctuation { path: Path },
    Trail,
    TrailCharRefStart,
    TrailCharRefInside,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::AutolinkLiteral(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::AddressStart => address_start(tokenizer),
        Step::ProtocolStart => protocol_start(tokenizer),
        Step::ProtocolAfter => protocol_after(tokenizer),
        Step::ProtocolPrefixInside { start } => protocol_prefix_inside(tokenizer, start),
        Step::ProtocolSlashesInside { size } => protocol_slashes_inside(tokenizer, size),
        Step::WwwAfter => www_after(tokenizer),
        Step::WwwStart => www_start(tokenizer),
        Step::WwwPrefixInside { size } => www_prefix_inside(tokenizer, size),
        Step::WwwPrefixAfter => www_prefix_after(tokenizer),
        Step::DomainInside { domain } => domain_inside(tokenizer, domain),
        Step::DomainAtPunctuation { domain } => domain_at_punctuation(tokenizer, domain),
        Step::DomainAfter { domain } => domain_after(tokenizer, domain),
        Step::PathInside { path } => path_inside(tokenizer, path),
        Step::PathEscape { path } => path_escape(tokenizer, path),
        Step::PathAtPunctuation { path } => path_at_punctuation(tokenizer, path),
        Step::Trail => trail(tokenizer),
        Step::TrailCharRefStart => trail_char_ref_start(tokenizer),
        Step::TrailCharRefInside => trail_char_ref_inside(tokenizer),
    }
}
