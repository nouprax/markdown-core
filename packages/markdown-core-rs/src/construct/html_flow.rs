//! HTML (flow) occurs in the [flow][] content type.
//!
//! ## Grammar
//!
//! HTML (flow) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! html_flow ::= raw | comment | instruction | declaration | cdata | basic | complete
//!
//! ; Note: closing tag name does not need to match opening tag name.
//! raw ::= '<' raw_tag_name [[space_or_tab *line | '>' *line] eol] *(*line eol) ['</' raw_tag_name *line]
//! comment ::= '<!--' [*'-' '>' *line | *line *(eol *line) ['-->' *line]]
//! instruction ::= '<?' ['>' *line | *line *(eol *line) ['?>' *line]]
//! declaration ::= '<!' ascii_alphabetic *line *(eol *line) ['>' *line]
//! cdata ::= '<![CDATA[' *line *(eol *line) [']]>' *line]
//! basic ::= '< ['/'] basic_tag_name [['/'] '>' *line *(eol 1*line)]
//! complete ::= (opening_tag | closing_tag) [*space_or_tab *(eol 1*line)]
//!
//! raw_tag_name ::= 'pre' | 'script' | 'style' | 'textarea' ; Note: case-insensitive.
//! basic_tag_name ::= 'address' | 'article' | 'aside' | ... ; See `constants.rs`, and note: case-insensitive.
//! opening_tag ::= '<' tag_name *(1*space_or_tab attribute) [*space_or_tab '/'] *space_or_tab '>'
//! closing_tag ::= '</' tag_name *space_or_tab '>'
//! tag_name ::= ascii_alphabetic *('-' | ascii_alphanumeric)
//! attribute ::= attribute_name [*space_or_tab '=' *space_or_tab attribute_value]
//! attribute_name ::= (':' | '_' | ascii_alphabetic) *('-' | '.' | ':' | '_' | ascii_alphanumeric)
//! attribute_value ::= '"' *(line - '"') '"' | "'" *(line - "'")  "'" | 1*(text - '"' - "'" - '/' - '<' - '=' - '>' - '`')
//! ```
//!
//! As this construct occurs in flow, like all flow constructs, it must be
//! followed by an eol (line ending) or eof (end of file).
//!
//! The grammar for HTML in markdown does not follow the rules of parsing
//! HTML according to the [*§ 13.2 Parsing HTML documents* in the HTML
//! spec][html_parsing].
//! As such, HTML in markdown *resembles* HTML, but is instead a (naïve?)
//! attempt to parse an XML-like language.
//! By extension, another notable property of the grammar is that it can
//! result in invalid HTML, in that it allows things that wouldn’t work or
//! wouldn’t work well in HTML, such as mismatched tags.
//!
//! Interestingly, most of the productions above have a clear opening and
//! closing condition (raw, comment, insutrction, declaration, cdata), but the
//! closing condition does not need to be satisfied.
//! In this case, the parser never has to backtrack.
//!
//! Because the **basic** and **complete** productions in the grammar form with
//! a tag, followed by more stuff, and stop at a blank line, it is possible to
//! interleave (a word for switching between languages) markdown and HTML
//! together, by placing the opening and closing tags on their own lines,
//! with blank lines between them and markdown.
//! For example:
//!
//! ```markdown
//! <div>This is <code>code</code> but this is not *emphasis*.</div>
//!
//! <div>
//!
//! This is a paragraph in a `div` and with `code` and *emphasis*.
//!
//! </div>
//! ```
//!
//! The **complete** production of HTML (flow) is not allowed to interrupt
//! content.
//! That means that a blank line is needed between a [paragraph][paragraph] and it.
//! However, [HTML (text)][html_text] has a similar production, which will
//! typically kick-in instead.
//!
//! The list of tag names allowed in the **raw** production are defined in
//! [`HTML_RAW_NAMES`][].
//! This production exists because there are a few cases where markdown
//! *inside* some elements, and hence interleaving, does not make sense.
//!
//! The list of tag names allowed in the **basic** production are defined in
//! [`HTML_BLOCK_NAMES`][].
//! This production exists because there are a few cases where we can decide
//! early that something is going to be a flow (block) element instead of a
//! phrasing (inline) element.
//! We *can* interrupt and don’t have to care too much about it being
//! well-formed.
//!
//! ## Tokens
//!
//! * [`HtmlFlow`][Name::HtmlFlow]
//! * [`HtmlFlowData`][Name::HtmlFlowData]
//! * [`LineEnd`][Name::LineEnd]
//!
//! ## References
//!
//! * [`html-flow.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/html-flow.js)
//! * [*§ 4.6 HTML blocks* in `CommonMark`](https://spec.commonmark.org/0.31/#html-blocks)
//!
//! [flow]: crate::construct::flow
//! [html_text]: crate::construct::html_text
//! [prose]: crate::construct::prose
//! [html_raw_names]: crate::util::constant::HTML_RAW_NAMES
//! [html_block_names]: crate::util::constant::HTML_BLOCK_NAMES
//! [html_parsing]: https://html.spec.whatwg.org/multipage/parsing.html#parsing

use crate::choice::Starts;
use crate::construct::partial_space_or_tab::{
    space_or_tab_with_options, Options as SpaceOrTabOptions,
};
use crate::construct::{blank_line, partial_non_lax_continuation};
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::{
    constant::{HTML_BLOCK_NAMES, HTML_CDATA_PREFIX, HTML_RAW_NAMES, HTML_RAW_SIZE_MAX, TAB_SIZE},
    slice::Slice,
};

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Codes(b"\t <");

/// Symbol for `<script>` (condition 1).
const RAW: u8 = 1;
/// Symbol for `<!---->` (condition 2).
const COMMENT: u8 = 2;
/// Symbol for `<?php?>` (condition 3).
const INSTRUCTION: u8 = 3;
/// Symbol for `<!doctype>` (condition 4).
const DECLARATION: u8 = 4;
/// Symbol for `<![CDATA[]]>` (condition 5).
const CDATA: u8 = 5;
/// Symbol for `<div` (condition 6).
const BASIC: u8 = 6;
/// Symbol for `<x>` (condition 7).
const COMPLETE: u8 = 7;

/// Start of HTML (flow).
///
/// ```markdown
/// > | <x />
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::HtmlFlow);

    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(State::Next(StateName::HtmlFlow(Step::Before)), State::Nok);
        State::Redo(space_or_tab_with_options(SpaceOrTabOptions {
            kind: Name::HtmlFlowData,
            min: 0,
            max: TAB_SIZE - 1,
            connect: false,
            content: None,
        }))
    } else {
        State::Redo(StateName::HtmlFlow(Step::Before))
    }
}

/// At `<`, after optional whitespace.
///
/// ```markdown
/// > | <x />
///     ^
/// ```
fn before(tokenizer: &mut Tokenizer) -> State {
    if Some(b'<') == tokenizer.current {
        tokenizer.enter(Name::HtmlFlowData);
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::Open))
    } else {
        State::Nok
    }
}

/// After `<`, at tag name or other stuff.
///
/// ```markdown
/// > | <x />
///      ^
/// > | <!doctype>
///      ^
/// > | <!--xxx-->
///      ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'!') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::DeclarationOpen))
        }
        Some(b'/') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::ElementCloseStart {
                start: tokenizer.point.index,
            }))
        }
        Some(b'?') => {
            tokenizer.consume();
            let kind = INSTRUCTION;
            // Do not form containers.
            tokenizer.concrete = true;
            // While we’re in an instruction instead of a declaration, we’re on a `?`
            // right now, so we do need to search for `>`, similar to declarations.
            State::Next(StateName::HtmlFlow(Step::ContinuationDeclarationInside {
                kind,
            }))
        }
        // ASCII alphabetical.
        Some(b'A'..=b'Z' | b'a'..=b'z') => State::Redo(StateName::HtmlFlow(Step::ElementName {
            start: tokenizer.point.index,
            shut: false,
        })),
        _ => State::Nok,
    }
}

/// After `<!`, at declaration, comment, or CDATA.
///
/// ```markdown
/// > | <!doctype>
///       ^
/// > | <!--xxx-->
///       ^
/// > | <![CDATA[>&<]]>
///       ^
/// ```
fn declaration_open(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'-') => {
            tokenizer.consume();
            let kind = COMMENT;
            State::Next(StateName::HtmlFlow(Step::CommentOpenInside { kind }))
        }
        Some(b'A'..=b'Z' | b'a'..=b'z') => {
            tokenizer.consume();
            let kind = DECLARATION;
            // Do not form containers.
            tokenizer.concrete = true;
            State::Next(StateName::HtmlFlow(Step::ContinuationDeclarationInside {
                kind,
            }))
        }
        Some(b'[') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CdataOpenInside { size: 0 }))
        }
        _ => State::Nok,
    }
}

/// After `<!-`, inside a comment, at another `-`.
///
/// ```markdown
/// > | <!--xxx-->
///        ^
/// ```
fn comment_open_inside(tokenizer: &mut Tokenizer, kind: u8) -> State {
    if let Some(b'-') = tokenizer.current {
        tokenizer.consume();
        // Do not form containers.
        tokenizer.concrete = true;
        State::Next(StateName::HtmlFlow(Step::ContinuationDeclarationInside {
            kind,
        }))
    } else {
        State::Nok
    }
}

/// After `<![`, inside CDATA, expecting `CDATA[`.
///
/// ```markdown
/// > | <![CDATA[>&<]]>
///        ^^^^^^
/// ```
fn cdata_open_inside(tokenizer: &mut Tokenizer, size: usize) -> State {
    if tokenizer.current == Some(HTML_CDATA_PREFIX[size]) {
        tokenizer.consume();

        if size + 1 == HTML_CDATA_PREFIX.len() {
            // Do not form containers.
            tokenizer.concrete = true;
            State::Next(StateName::HtmlFlow(Step::Continuation { kind: CDATA }))
        } else {
            State::Next(StateName::HtmlFlow(Step::CdataOpenInside {
                size: size + 1,
            }))
        }
    } else {
        State::Nok
    }
}

/// After `</`, in closing tag, at tag name.
///
/// ```markdown
/// > | </x>
///       ^
/// ```
fn element_close_start(tokenizer: &mut Tokenizer, start: usize) -> State {
    if let Some(b'A'..=b'Z' | b'a'..=b'z') = tokenizer.current {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::ElementName { start, shut: true }))
    } else {
        State::Nok
    }
}

/// In tag name.
///
/// ```markdown
/// > | <ab>
///      ^^
/// > | </ab>
///       ^^
/// ```
fn element_name(tokenizer: &mut Tokenizer, start: usize, shut: bool) -> State {
    match tokenizer.current {
        None | Some(b'\t' | b'\n' | b' ' | b'/' | b'>') => {
            let shut_element = shut;
            let slash = matches!(tokenizer.current, Some(b'/'));
            // Guaranteed to be valid ASCII bytes.
            let slice =
                Slice::from_indices(tokenizer.parse_state.source, start, tokenizer.point.index);
            let name = slice
                .as_str()
                // The line ending case might result in a `\r` that is already accounted for.
                .trim()
                .to_ascii_lowercase();

            if !slash && !shut_element && HTML_RAW_NAMES.contains(&name.as_str()) {
                let kind = RAW;
                // Do not form containers.
                tokenizer.concrete = true;
                State::Redo(StateName::HtmlFlow(Step::Continuation { kind }))
            } else if HTML_BLOCK_NAMES.contains(&name.as_str()) {
                let kind = BASIC;

                if slash {
                    tokenizer.consume();
                    State::Next(StateName::HtmlFlow(Step::BasicSelfShut { kind }))
                } else {
                    // Do not form containers.
                    tokenizer.concrete = true;
                    State::Redo(StateName::HtmlFlow(Step::Continuation { kind }))
                }
            } else {
                // Do not support complete HTML when interrupting.
                if tokenizer.interrupt && !tokenizer.lax {
                    State::Nok
                } else if shut_element {
                    State::Redo(StateName::HtmlFlow(Step::CompleteShutElementAfter))
                } else {
                    State::Redo(StateName::HtmlFlow(Step::CompleteAttributeNameBefore))
                }
            }
        }
        // ASCII alphanumerical and `-`.
        Some(b'-' | b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::ElementName { start, shut }))
        }
        Some(_) => State::Nok,
    }
}

/// After closing slash of a basic tag name.
///
/// ```markdown
/// > | <div/>
///          ^
/// ```
fn basic_self_shut(tokenizer: &mut Tokenizer, kind: u8) -> State {
    if let Some(b'>') = tokenizer.current {
        tokenizer.consume();
        // Do not form containers.
        tokenizer.concrete = true;
        State::Next(StateName::HtmlFlow(Step::Continuation { kind }))
    } else {
        State::Nok
    }
}

/// After closing slash of a complete tag name.
///
/// ```markdown
/// > | <x/>
///        ^
/// ```
fn complete_shut_element_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteShutElementAfter))
        }
        _ => State::Redo(StateName::HtmlFlow(Step::CompleteEnd)),
    }
}

/// At an attribute name.
///
/// At first, this state is used after a complete tag name, after whitespace,
/// where it expects optional attributes or the end of the tag.
/// It is also reused after attributes, when expecting more optional
/// attributes.
///
/// ```markdown
/// > | <a />
///        ^
/// > | <a :b>
///        ^
/// > | <a _b>
///        ^
/// > | <a b>
///        ^
/// > | <a >
///        ^
/// ```
fn complete_attribute_name_before(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteAttributeNameBefore))
        }
        Some(b'/') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteEnd))
        }
        // ASCII alphanumerical and `:` and `_`.
        Some(b'0'..=b'9' | b':' | b'A'..=b'Z' | b'_' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteAttributeName))
        }
        _ => State::Redo(StateName::HtmlFlow(Step::CompleteEnd)),
    }
}

/// In attribute name.
///
/// ```markdown
/// > | <a :b>
///         ^
/// > | <a _b>
///         ^
/// > | <a b>
///         ^
/// ```
fn complete_attribute_name(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // ASCII alphanumerical and `-`, `.`, `:`, and `_`.
        Some(b'-' | b'.' | b'0'..=b'9' | b':' | b'A'..=b'Z' | b'_' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteAttributeName))
        }
        _ => State::Redo(StateName::HtmlFlow(Step::CompleteAttributeNameAfter)),
    }
}

/// After attribute name, at an optional initializer, the end of the tag, or
/// whitespace.
///
/// ```markdown
/// > | <a b>
///         ^
/// > | <a b=c>
///         ^
/// ```
fn complete_attribute_name_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteAttributeNameAfter))
        }
        Some(b'=') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteAttributeValueBefore))
        }
        _ => State::Redo(StateName::HtmlFlow(Step::CompleteAttributeNameBefore)),
    }
}

/// Before unquoted, double quoted, or single quoted attribute value, allowing
/// whitespace.
///
/// ```markdown
/// > | <a b=c>
///          ^
/// > | <a b="c">
///          ^
/// ```
fn complete_attribute_value_before(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'<' | b'=' | b'>' | b'`') => State::Nok,
        Some(b'\t' | b' ') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteAttributeValueBefore))
        }
        Some(b'"' | b'\'') => {
            let quote = tokenizer.current.unwrap();
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteAttributeValueQuoted {
                quote,
            }))
        }
        _ => State::Redo(StateName::HtmlFlow(Step::CompleteAttributeValueUnquoted)),
    }
}

/// In double or single quoted attribute value.
///
/// ```markdown
/// > | <a b="c">
///           ^
/// > | <a b='c'>
///           ^
/// ```
fn complete_attribute_value_quoted(tokenizer: &mut Tokenizer, quote: u8) -> State {
    if tokenizer.current == Some(quote) {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::CompleteAttributeValueQuotedAfter))
    } else if matches!(tokenizer.current, None | Some(b'\n')) {
        State::Nok
    } else {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::CompleteAttributeValueQuoted {
            quote,
        }))
    }
}

/// In unquoted attribute value.
///
/// ```markdown
/// > | <a b=c>
///          ^
/// ```
fn complete_attribute_value_unquoted(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\t' | b'\n' | b' ' | b'"' | b'\'' | b'/' | b'<' | b'=' | b'>' | b'`') => {
            State::Redo(StateName::HtmlFlow(Step::CompleteAttributeNameAfter))
        }
        Some(_) => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteAttributeValueUnquoted))
        }
    }
}

/// After double or single quoted attribute value, before whitespace or the
/// end of the tag.
///
/// ```markdown
/// > | <a b="c">
///            ^
/// ```
fn complete_attribute_value_quoted_after(tokenizer: &mut Tokenizer) -> State {
    if let Some(b'\t' | b' ' | b'/' | b'>') = tokenizer.current {
        State::Redo(StateName::HtmlFlow(Step::CompleteAttributeNameBefore))
    } else {
        State::Nok
    }
}

/// In certain circumstances of a complete tag where only an `>` is allowed.
///
/// ```markdown
/// > | <a b="c">
///             ^
/// ```
fn complete_end(tokenizer: &mut Tokenizer) -> State {
    if let Some(b'>') = tokenizer.current {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::CompleteAfter))
    } else {
        State::Nok
    }
}

/// After `>` in a complete tag.
///
/// ```markdown
/// > | <x>
///        ^
/// ```
fn complete_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            // Do not form containers.
            tokenizer.concrete = true;
            State::Redo(StateName::HtmlFlow(Step::Continuation { kind: COMPLETE }))
        }
        Some(b'\t' | b' ') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::CompleteAfter))
        }
        Some(_) => State::Nok,
    }
}

/// In continuation of any HTML kind.
///
/// ```markdown
/// > | <!--xxx-->
///          ^
/// ```
fn continuation(tokenizer: &mut Tokenizer, kind: u8) -> State {
    if kind == COMMENT && tokenizer.current == Some(b'-') {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::ContinuationCommentInside {
            kind,
        }))
    } else if kind == RAW && tokenizer.current == Some(b'<') {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::ContinuationRawElementOpen {
            kind,
        }))
    } else if kind == DECLARATION && tokenizer.current == Some(b'>') {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::ContinuationClose))
    } else if kind == INSTRUCTION && tokenizer.current == Some(b'?') {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::ContinuationDeclarationInside {
            kind,
        }))
    } else if kind == CDATA && tokenizer.current == Some(b']') {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::ContinuationCdataInside { kind }))
    } else if matches!(kind, BASIC | COMPLETE) && tokenizer.current == Some(b'\n') {
        tokenizer.exit(Name::HtmlFlowData);
        tokenizer.check(
            State::Next(StateName::HtmlFlow(Step::ContinuationAfter)),
            State::Next(StateName::HtmlFlow(Step::ContinuationStart { kind })),
        );
        State::Redo(StateName::HtmlFlow(Step::BlankLineBefore))
    } else if matches!(tokenizer.current, None | Some(b'\n')) {
        tokenizer.exit(Name::HtmlFlowData);
        State::Redo(StateName::HtmlFlow(Step::ContinuationStart { kind }))
    } else {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::Continuation { kind }))
    }
}

/// In continuation, at eol.
///
/// ```markdown
/// > | <x>
///        ^
///   | asd
/// ```
fn continuation_start(tokenizer: &mut Tokenizer, kind: u8) -> State {
    tokenizer.check(
        State::Next(StateName::HtmlFlow(Step::ContinuationStartNonLax { kind })),
        State::Next(StateName::HtmlFlow(Step::ContinuationAfter)),
    );
    State::Redo(StateName::NonLaxContinuation(
        partial_non_lax_continuation::Step::Start,
    ))
}

/// In continuation, at eol, before non-lazy content.
///
/// ```markdown
/// > | <x>
///        ^
///   | asd
/// ```
fn continuation_start_non_lax(tokenizer: &mut Tokenizer, kind: u8) -> State {
    match tokenizer.current {
        Some(b'\n') => {
            tokenizer.enter(Name::LineEnd);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnd);
            State::Next(StateName::HtmlFlow(Step::ContinuationBefore { kind }))
        }
        _ => unreachable!("expected eol"),
    }
}

/// In continuation, before non-lazy content.
///
/// ```markdown
///   | <x>
/// > | asd
///     ^
/// ```
fn continuation_before(tokenizer: &mut Tokenizer, kind: u8) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Redo(StateName::HtmlFlow(Step::ContinuationStart { kind })),
        _ => {
            tokenizer.enter(Name::HtmlFlowData);
            State::Redo(StateName::HtmlFlow(Step::Continuation { kind }))
        }
    }
}

/// In comment continuation, after one `-`, expecting another.
///
/// ```markdown
/// > | <!--xxx-->
///             ^
/// ```
fn continuation_comment_inside(tokenizer: &mut Tokenizer, kind: u8) -> State {
    match tokenizer.current {
        Some(b'-') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::ContinuationDeclarationInside {
                kind,
            }))
        }
        _ => State::Redo(StateName::HtmlFlow(Step::Continuation { kind })),
    }
}

/// In raw continuation, after `<`, at `/`.
///
/// ```markdown
/// > | <script>console.log(1)</script>
///                            ^
/// ```
fn continuation_raw_element_open(tokenizer: &mut Tokenizer, kind: u8) -> State {
    match tokenizer.current {
        Some(b'/') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::ContinuationRawEndElement {
                kind,
                start: tokenizer.point.index,
            }))
        }
        _ => State::Redo(StateName::HtmlFlow(Step::Continuation { kind })),
    }
}

/// In raw continuation, after `</`, in a raw tag name.
///
/// ```markdown
/// > | <script>console.log(1)</script>
///                             ^^^^^^
/// ```
fn continuation_raw_end_element(tokenizer: &mut Tokenizer, kind: u8, start: usize) -> State {
    match tokenizer.current {
        Some(b'>') => {
            // Guaranteed to be valid ASCII bytes.
            let slice =
                Slice::from_indices(tokenizer.parse_state.source, start, tokenizer.point.index);
            let name = slice.as_str().to_ascii_lowercase();

            if HTML_RAW_NAMES.contains(&name.as_str()) {
                tokenizer.consume();
                State::Next(StateName::HtmlFlow(Step::ContinuationClose))
            } else {
                State::Redo(StateName::HtmlFlow(Step::Continuation { kind }))
            }
        }
        Some(b'A'..=b'Z' | b'a'..=b'z') if tokenizer.point.index - start < HTML_RAW_SIZE_MAX => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::ContinuationRawEndElement {
                kind,
                start,
            }))
        }
        _ => State::Redo(StateName::HtmlFlow(Step::Continuation { kind })),
    }
}

/// In cdata continuation, after `]`, expecting `]>`.
///
/// ```markdown
/// > | <![CDATA[>&<]]>
///                  ^
/// ```
fn continuation_cdata_inside(tokenizer: &mut Tokenizer, kind: u8) -> State {
    match tokenizer.current {
        Some(b']') => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::ContinuationDeclarationInside {
                kind,
            }))
        }
        _ => State::Redo(StateName::HtmlFlow(Step::Continuation { kind })),
    }
}

/// In declaration or instruction continuation, at `>`.
///
/// ```markdown
/// > | <!-->
///         ^
/// > | <?>
///       ^
/// > | <!q>
///        ^
/// > | <!--ab-->
///             ^
/// > | <![CDATA[>&<]]>
///                   ^
/// ```
fn continuation_declaration_inside(tokenizer: &mut Tokenizer, kind: u8) -> State {
    if kind == COMMENT && tokenizer.current == Some(b'-') {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::ContinuationDeclarationInside {
            kind,
        }))
    } else if tokenizer.current == Some(b'>') {
        tokenizer.consume();
        State::Next(StateName::HtmlFlow(Step::ContinuationClose))
    } else {
        State::Redo(StateName::HtmlFlow(Step::Continuation { kind }))
    }
}

/// In closed continuation: everything we get until the eol/eof is part of it.
///
/// ```markdown
/// > | <!doctype>
///               ^
/// ```
fn continuation_close(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::HtmlFlowData);
            State::Redo(StateName::HtmlFlow(Step::ContinuationAfter))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::HtmlFlow(Step::ContinuationClose))
        }
    }
}

/// Done.
///
/// ```markdown
/// > | <!doctype>
///               ^
/// ```
fn continuation_after(tokenizer: &mut Tokenizer) -> State {
    tokenizer.exit(Name::HtmlFlow);
    // Feel free to interrupt.
    tokenizer.interrupt = false;
    // No longer concrete.
    tokenizer.concrete = false;
    State::Ok
}

/// Before eol, expecting blank line.
///
/// ```markdown
/// > | <div>
///          ^
///   |
/// ```
fn blank_line_before(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::LineEnd);
    tokenizer.consume();
    tokenizer.exit(Name::LineEnd);
    State::Next(StateName::BlankLine(blank_line::Step::Start))
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Before,
    Open,
    DeclarationOpen,
    CommentOpenInside { kind: u8 },
    CdataOpenInside { size: usize },
    ElementCloseStart { start: usize },
    ElementName { start: usize, shut: bool },
    BasicSelfShut { kind: u8 },
    CompleteShutElementAfter,
    CompleteEnd,
    CompleteAttributeNameBefore,
    CompleteAttributeName,
    CompleteAttributeNameAfter,
    CompleteAttributeValueBefore,
    CompleteAttributeValueQuoted { quote: u8 },
    CompleteAttributeValueQuotedAfter,
    CompleteAttributeValueUnquoted,
    CompleteAfter,
    BlankLineBefore,
    Continuation { kind: u8 },
    ContinuationDeclarationInside { kind: u8 },
    ContinuationAfter,
    ContinuationStart { kind: u8 },
    ContinuationBefore { kind: u8 },
    ContinuationCommentInside { kind: u8 },
    ContinuationRawElementOpen { kind: u8 },
    ContinuationRawEndElement { kind: u8, start: usize },
    ContinuationClose,
    ContinuationCdataInside { kind: u8 },
    ContinuationStartNonLax { kind: u8 },
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::HtmlFlow(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Before => before(tokenizer),
        Step::Open => open(tokenizer),
        Step::DeclarationOpen => declaration_open(tokenizer),
        Step::CommentOpenInside { kind } => comment_open_inside(tokenizer, kind),
        Step::CdataOpenInside { size } => cdata_open_inside(tokenizer, size),
        Step::ElementCloseStart { start } => element_close_start(tokenizer, start),
        Step::ElementName { start, shut } => element_name(tokenizer, start, shut),
        Step::BasicSelfShut { kind } => basic_self_shut(tokenizer, kind),
        Step::CompleteShutElementAfter => complete_shut_element_after(tokenizer),
        Step::CompleteEnd => complete_end(tokenizer),
        Step::CompleteAttributeNameBefore => complete_attribute_name_before(tokenizer),
        Step::CompleteAttributeName => complete_attribute_name(tokenizer),
        Step::CompleteAttributeNameAfter => complete_attribute_name_after(tokenizer),
        Step::CompleteAttributeValueBefore => complete_attribute_value_before(tokenizer),
        Step::CompleteAttributeValueQuoted { quote } => {
            complete_attribute_value_quoted(tokenizer, quote)
        }
        Step::CompleteAttributeValueQuotedAfter => complete_attribute_value_quoted_after(tokenizer),
        Step::CompleteAttributeValueUnquoted => complete_attribute_value_unquoted(tokenizer),
        Step::CompleteAfter => complete_after(tokenizer),
        Step::BlankLineBefore => blank_line_before(tokenizer),
        Step::Continuation { kind } => continuation(tokenizer, kind),
        Step::ContinuationDeclarationInside { kind } => {
            continuation_declaration_inside(tokenizer, kind)
        }
        Step::ContinuationAfter => continuation_after(tokenizer),
        Step::ContinuationStart { kind } => continuation_start(tokenizer, kind),
        Step::ContinuationBefore { kind } => continuation_before(tokenizer, kind),
        Step::ContinuationCommentInside { kind } => continuation_comment_inside(tokenizer, kind),
        Step::ContinuationRawElementOpen { kind } => continuation_raw_element_open(tokenizer, kind),
        Step::ContinuationRawEndElement { kind, start } => {
            continuation_raw_end_element(tokenizer, kind, start)
        }
        Step::ContinuationClose => continuation_close(tokenizer),
        Step::ContinuationCdataInside { kind } => continuation_cdata_inside(tokenizer, kind),
        Step::ContinuationStartNonLax { kind } => continuation_start_non_lax(tokenizer, kind),
    }
}
