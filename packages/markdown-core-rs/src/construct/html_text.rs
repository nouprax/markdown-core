//! HTML (text) occurs in the [text][] content type.
//!
//! ## Grammar
//!
//! HTML (text) forms with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! html_text ::= comment | instruction | declaration | cdata | tag_close | tag_open
//!
//! ; Restriction: the text is not allowed to start with `>`, `->`, or to contain `--`.
//! comment ::= '<!--' *byte '-->'
//! instruction ::= '<?' *byte '?>'
//! declaration ::= '<!' ascii_alphabetic *byte '>'
//! ; Restriction: the text is not allowed to contain `]]`.
//! cdata ::= '<![CDATA[' *byte ']]>'
//! tag_close ::= '</' tag_name [space_or_tab_eol] '>'
//! opening_tag ::= '<' tag_name *(space_or_tab_eol attribute) [[space_or_tab_eol] '/'] [space_or_tab_eol] '>'
//!
//! tag_name ::= ascii_alphabetic *( '-' | ascii_alphanumeric )
//! attribute ::= attribute_name [[space_or_tab_eol] '=' [space_or_tab_eol] attribute_value]
//! attribute_name ::= (':' | '_' | ascii_alphabetic) *('-' | '.' | ':' | '_' | ascii_alphanumeric)
//! attribute_value ::= '"' *(byte - '"') '"' | "'" *(byte - "'")  "'" | 1*(text - '"' - "'" - '/' - '<' - '=' - '>' - '`')
//! ```
//!
//! The grammar for HTML in markdown does not follow the rules of parsing
//! HTML according to the [*§ 13.2 Parsing HTML documents* in the HTML
//! spec][html_parsing].
//! See the related flow construct [HTML (flow)][html_flow] for more info.
//!
//! Because the **tag open** and **tag close** productions in the grammar form
//! with just tags instead of complete elements, it is possible to interleave
//! (a word for switching between languages) markdown and HTML together.
//! For example:
//!
//! ```markdown
//! This is equivalent to <code>*emphasised* code</code>.
//! ```
//!
//! ## Tokens
//!
//! * [`HtmlText`][Name::HtmlText]
//! * [`HtmlTextData`][Name::HtmlTextData]
//!
//! ## References
//!
//! * [`html-text.js` in `micromark`](https://github.com/micromark/micromark/blob/main/packages/micromark-core-commonmark/dev/lib/html-text.js)
//! * [*§ 6.6 Raw HTML* in `CommonMark`](https://spec.commonmark.org/0.31/#raw-html)
//!
//! [text]: crate::construct::text
//! [html_flow]: crate::construct::html_flow
//! [html_parsing]: https://html.spec.whatwg.org/multipage/parsing.html#parsing

use crate::choice::Starts;
use crate::construct::partial_space_or_tab::space_or_tab;
use crate::event::Name;
use crate::state::{Name as StateName, State};
use crate::tokenizer::Tokenizer;
use crate::util::constant::HTML_CDATA_PREFIX;

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Codes(b"<");

/// Start of HTML (text).
///
/// ```markdown
/// > | a <b> c
///       ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if Some(b'<') == tokenizer.current {
        tokenizer.enter(Name::HtmlText);
        tokenizer.enter(Name::HtmlTextData);
        tokenizer.consume();
        State::Next(StateName::HtmlText(Step::Open))
    } else {
        State::Nok
    }
}

/// After `<`, at tag name or other stuff.
///
/// ```markdown
/// > | a <b> c
///        ^
/// > | a <!doctype> c
///        ^
/// > | a <!--b--> c
///        ^
/// ```
fn open(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'!') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::DeclarationOpen))
        }
        Some(b'/') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementCloseStart))
        }
        Some(b'?') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::Instruction))
        }
        // ASCII alphabetical.
        Some(b'A'..=b'Z' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpen))
        }
        _ => State::Nok,
    }
}

/// After `<!`, at declaration, comment, or CDATA.
///
/// ```markdown
/// > | a <!doctype> c
///         ^
/// > | a <!--b--> c
///         ^
/// > | a <![CDATA[>&<]]> c
///         ^
/// ```
fn declaration_open(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'-') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::CommentOpenInside))
        }
        // ASCII alphabetical.
        Some(b'A'..=b'Z' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::Declaration))
        }
        Some(b'[') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::CdataOpenInside { size: 0 }))
        }
        _ => State::Nok,
    }
}

/// In a comment, after `<!-`, at another `-`.
///
/// ```markdown
/// > | a <!--b--> c
///          ^
/// ```
fn comment_open_inside(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'-') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::CommentEnd))
        }
        _ => State::Nok,
    }
}

/// In comment.
///
/// ```markdown
/// > | a <!--b--> c
///           ^
/// ```
fn comment(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.attempt(State::Next(StateName::HtmlText(Step::Comment)), State::Nok);
            State::Redo(StateName::HtmlText(Step::LineEndBefore))
        }
        Some(b'-') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::CommentClose))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::Comment))
        }
    }
}

/// In comment, after `-`.
///
/// ```markdown
/// > | a <!--b--> c
///             ^
/// ```
fn comment_close(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'-') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::CommentEnd))
        }
        _ => State::Redo(StateName::HtmlText(Step::Comment)),
    }
}
/// In comment, after `-`.
///
/// ```markdown
/// > | a <!--b--> c
///             ^
/// ```
fn comment_end(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'>') => State::Redo(StateName::HtmlText(Step::End)),
        Some(b'-') => State::Redo(StateName::HtmlText(Step::CommentClose)),
        _ => State::Redo(StateName::HtmlText(Step::Comment)),
    }
}

/// After `<![`, in CDATA, expecting `CDATA[`.
///
/// ```markdown
/// > | a <![CDATA[>&<]]> b
///          ^^^^^^
/// ```
fn cdata_open_inside(tokenizer: &mut Tokenizer, size: usize) -> State {
    if tokenizer.current == Some(HTML_CDATA_PREFIX[size]) {
        tokenizer.consume();

        if size + 1 == HTML_CDATA_PREFIX.len() {
            State::Next(StateName::HtmlText(Step::Cdata))
        } else {
            State::Next(StateName::HtmlText(Step::CdataOpenInside {
                size: size + 1,
            }))
        }
    } else {
        State::Nok
    }
}

/// In CDATA.
///
/// ```markdown
/// > | a <![CDATA[>&<]]> b
///                ^^^
/// ```
fn cdata(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.attempt(State::Next(StateName::HtmlText(Step::Cdata)), State::Nok);
            State::Redo(StateName::HtmlText(Step::LineEndBefore))
        }
        Some(b']') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::CdataClose))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::Cdata))
        }
    }
}

/// In CDATA, after `]`, at another `]`.
///
/// ```markdown
/// > | a <![CDATA[>&<]]> b
///                    ^
/// ```
fn cdata_close(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b']') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::CdataEnd))
        }
        _ => State::Redo(StateName::HtmlText(Step::Cdata)),
    }
}

/// In CDATA, after `]]`, at `>`.
///
/// ```markdown
/// > | a <![CDATA[>&<]]> b
///                     ^
/// ```
fn cdata_end(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'>') => State::Redo(StateName::HtmlText(Step::End)),
        Some(b']') => State::Redo(StateName::HtmlText(Step::CdataClose)),
        _ => State::Redo(StateName::HtmlText(Step::Cdata)),
    }
}

/// In declaration.
///
/// ```markdown
/// > | a <!b> c
///          ^
/// ```
fn declaration(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'>') => State::Redo(StateName::HtmlText(Step::End)),
        Some(b'\n') => {
            tokenizer.attempt(
                State::Next(StateName::HtmlText(Step::Declaration)),
                State::Nok,
            );
            State::Redo(StateName::HtmlText(Step::LineEndBefore))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::Declaration))
        }
    }
}

/// In instruction.
///
/// ```markdown
/// > | a <?b?> c
///         ^
/// ```
fn instruction(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            tokenizer.attempt(
                State::Next(StateName::HtmlText(Step::Instruction)),
                State::Nok,
            );
            State::Redo(StateName::HtmlText(Step::LineEndBefore))
        }
        Some(b'?') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::InstructionClose))
        }
        _ => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::Instruction))
        }
    }
}

/// In instruction, after `?`, at `>`.
///
/// ```markdown
/// > | a <?b?> c
///           ^
/// ```
fn instruction_close(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'>') => State::Redo(StateName::HtmlText(Step::End)),
        _ => State::Redo(StateName::HtmlText(Step::Instruction)),
    }
}

/// After `</`, in closing tag, at tag name.
///
/// ```markdown
/// > | a </b> c
///         ^
/// ```
fn element_close_start(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // ASCII alphabetical.
        Some(b'A'..=b'Z' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementClose))
        }
        _ => State::Nok,
    }
}

/// After `</x`, in a tag name.
///
/// ```markdown
/// > | a </b> c
///          ^
/// ```
fn element_close(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // ASCII alphanumerical and `-`.
        Some(b'-' | b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementClose))
        }
        _ => State::Redo(StateName::HtmlText(Step::ElementCloseBetween)),
    }
}

/// In closing tag, after tag name.
///
/// ```markdown
/// > | a </b> c
///          ^
/// ```
fn element_close_between(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\n') => {
            tokenizer.attempt(
                State::Next(StateName::HtmlText(Step::ElementCloseBetween)),
                State::Nok,
            );
            State::Redo(StateName::HtmlText(Step::LineEndBefore))
        }
        Some(b'\t' | b' ') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementCloseBetween))
        }
        _ => State::Redo(StateName::HtmlText(Step::End)),
    }
}

/// After `<x`, in opening tag name.
///
/// ```markdown
/// > | a <b> c
///         ^
/// ```
fn element_open(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // ASCII alphanumerical and `-`.
        Some(b'-' | b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpen))
        }
        Some(b'\t' | b'\n' | b' ' | b'/' | b'>') => {
            State::Redo(StateName::HtmlText(Step::ElementOpenBetween))
        }
        _ => State::Nok,
    }
}

/// In opening tag, after tag name.
///
/// ```markdown
/// > | a <b> c
///         ^
/// ```
fn element_open_between(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\n') => {
            tokenizer.attempt(
                State::Next(StateName::HtmlText(Step::ElementOpenBetween)),
                State::Nok,
            );
            State::Redo(StateName::HtmlText(Step::LineEndBefore))
        }
        Some(b'\t' | b' ') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpenBetween))
        }
        Some(b'/') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::End))
        }
        // ASCII alphabetical and `:` and `_`.
        Some(b':' | b'A'..=b'Z' | b'_' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpenAttributeName))
        }
        _ => State::Redo(StateName::HtmlText(Step::End)),
    }
}

/// In attribute name.
///
/// ```markdown
/// > | a <b c> d
///          ^
/// ```
fn element_open_attribute_name(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        // ASCII alphabetical and `-`, `.`, `:`, and `_`.
        Some(b'-' | b'.' | b'0'..=b'9' | b':' | b'A'..=b'Z' | b'_' | b'a'..=b'z') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpenAttributeName))
        }
        _ => State::Redo(StateName::HtmlText(Step::ElementOpenAttributeNameAfter)),
    }
}

/// After attribute name, before initializer, the end of the tag, or
/// whitespace.
///
/// ```markdown
/// > | a <b c> d
///           ^
/// ```
fn element_open_attribute_name_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\n') => {
            tokenizer.attempt(
                State::Next(StateName::HtmlText(Step::ElementOpenAttributeNameAfter)),
                State::Nok,
            );
            State::Redo(StateName::HtmlText(Step::LineEndBefore))
        }
        Some(b'\t' | b' ') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpenAttributeNameAfter))
        }
        Some(b'=') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpenAttributeValueBefore))
        }
        _ => State::Redo(StateName::HtmlText(Step::ElementOpenBetween)),
    }
}

/// Before unquoted, double quoted, or single quoted attribute value, allowing
/// whitespace.
///
/// ```markdown
/// > | a <b c=d> e
///            ^
/// ```
fn element_open_attribute_value_before(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'<' | b'=' | b'>' | b'`') => State::Nok,
        Some(b'\n') => {
            tokenizer.attempt(
                State::Next(StateName::HtmlText(Step::ElementOpenAttributeValueBefore)),
                State::Nok,
            );
            State::Redo(StateName::HtmlText(Step::LineEndBefore))
        }
        Some(b'\t' | b' ') => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpenAttributeValueBefore))
        }
        Some(b'"' | b'\'') => {
            let marker = tokenizer.current.unwrap();
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpenAttributeValueQuoted {
                marker,
            }))
        }
        Some(_) => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpenAttributeValueUnquoted))
        }
    }
}

/// In double or single quoted attribute value.
///
/// ```markdown
/// > | a <b c="d"> e
///             ^
/// ```
fn element_open_attribute_value_quoted(tokenizer: &mut Tokenizer, marker: u8) -> State {
    if tokenizer.current == Some(marker) {
        tokenizer.consume();
        State::Next(StateName::HtmlText(
            Step::ElementOpenAttributeValueQuotedAfter,
        ))
    } else {
        match tokenizer.current {
            None => State::Nok,
            Some(b'\n') => {
                tokenizer.attempt(
                    State::Next(StateName::HtmlText(Step::ElementOpenAttributeValueQuoted {
                        marker,
                    })),
                    State::Nok,
                );
                State::Redo(StateName::HtmlText(Step::LineEndBefore))
            }
            _ => {
                tokenizer.consume();
                State::Next(StateName::HtmlText(Step::ElementOpenAttributeValueQuoted {
                    marker,
                }))
            }
        }
    }
}

/// In unquoted attribute value.
///
/// ```markdown
/// > | a <b c=d> e
///            ^
/// ```
fn element_open_attribute_value_unquoted(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'"' | b'\'' | b'<' | b'=' | b'`') => State::Nok,
        Some(b'\t' | b'\n' | b' ' | b'/' | b'>') => {
            State::Redo(StateName::HtmlText(Step::ElementOpenBetween))
        }
        Some(_) => {
            tokenizer.consume();
            State::Next(StateName::HtmlText(Step::ElementOpenAttributeValueUnquoted))
        }
    }
}

/// After double or single quoted attribute value, before whitespace or the end
/// of the tag.
///
/// ```markdown
/// > | a <b c="d"> e
///               ^
/// ```
fn element_open_attribute_value_quoted_after(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\t' | b'\n' | b' ' | b'/' | b'>') => {
            State::Redo(StateName::HtmlText(Step::ElementOpenBetween))
        }
        _ => State::Nok,
    }
}

/// In certain circumstances of a tag where only an `>` is allowed.
///
/// ```markdown
/// > | a <b c="d"> e
///               ^
/// ```
fn end(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'>') => {
            tokenizer.consume();
            tokenizer.exit(Name::HtmlTextData);
            tokenizer.exit(Name::HtmlText);
            State::Ok
        }
        _ => State::Nok,
    }
}

/// At eol.
///
/// > 👉 **Note**: we can’t have blank lines in text, so no need to worry about
/// > empty tokens.
///
/// ```markdown
/// > | a <!--a
///            ^
///   | b-->
/// ```
fn line_end_before(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        Some(b'\n') => {
            tokenizer.exit(Name::HtmlTextData);
            tokenizer.enter(Name::LineEnd);
            tokenizer.consume();
            tokenizer.exit(Name::LineEnd);
            State::Next(StateName::HtmlText(Step::LineEndAfter))
        }
        _ => unreachable!("expected eol"),
    }
}

/// After eol, at optional whitespace.
///
/// > 👉 **Note**: we can’t have blank lines in text, so no need to worry about
/// > empty tokens.
///
/// ```markdown
///   | a <!--a
/// > | b-->
///     ^
/// ```
fn line_end_after(tokenizer: &mut Tokenizer) -> State {
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::HtmlText(Step::LineEndAfterPrefix)),
            State::Nok,
        );
        State::Redo(space_or_tab())
    } else {
        State::Redo(StateName::HtmlText(Step::LineEndAfterPrefix))
    }
}

/// After eol, after optional whitespace.
///
/// > 👉 **Note**: we can’t have blank lines in text, so no need to worry about
/// > empty tokens.
///
/// ```markdown
///   | a <!--a
/// > | b-->
///     ^
/// ```
fn line_end_after_prefix(tokenizer: &mut Tokenizer) -> State {
    tokenizer.enter(Name::HtmlTextData);
    State::Ok
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    Open,
    DeclarationOpen,
    ElementCloseStart,
    ElementClose,
    ElementCloseBetween,
    ElementOpen,
    ElementOpenBetween,
    ElementOpenAttributeName,
    ElementOpenAttributeNameAfter,
    ElementOpenAttributeValueBefore,
    ElementOpenAttributeValueQuoted { marker: u8 },
    ElementOpenAttributeValueQuotedAfter,
    ElementOpenAttributeValueUnquoted,
    Cdata,
    CdataOpenInside { size: usize },
    CdataClose,
    CdataEnd,
    CommentOpenInside,
    Comment,
    CommentClose,
    CommentEnd,
    Declaration,
    End,
    Instruction,
    InstructionClose,
    LineEndBefore,
    LineEndAfter,
    LineEndAfterPrefix,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::HtmlText(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::Open => open(tokenizer),
        Step::DeclarationOpen => declaration_open(tokenizer),
        Step::ElementCloseStart => element_close_start(tokenizer),
        Step::ElementClose => element_close(tokenizer),
        Step::ElementCloseBetween => element_close_between(tokenizer),
        Step::ElementOpen => element_open(tokenizer),
        Step::ElementOpenBetween => element_open_between(tokenizer),
        Step::ElementOpenAttributeName => element_open_attribute_name(tokenizer),
        Step::ElementOpenAttributeNameAfter => element_open_attribute_name_after(tokenizer),
        Step::ElementOpenAttributeValueBefore => element_open_attribute_value_before(tokenizer),
        Step::ElementOpenAttributeValueQuoted { marker } => {
            element_open_attribute_value_quoted(tokenizer, marker)
        }
        Step::ElementOpenAttributeValueQuotedAfter => {
            element_open_attribute_value_quoted_after(tokenizer)
        }
        Step::ElementOpenAttributeValueUnquoted => element_open_attribute_value_unquoted(tokenizer),
        Step::Cdata => cdata(tokenizer),
        Step::CdataOpenInside { size } => cdata_open_inside(tokenizer, size),
        Step::CdataClose => cdata_close(tokenizer),
        Step::CdataEnd => cdata_end(tokenizer),
        Step::CommentOpenInside => comment_open_inside(tokenizer),
        Step::Comment => comment(tokenizer),
        Step::CommentClose => comment_close(tokenizer),
        Step::CommentEnd => comment_end(tokenizer),
        Step::Declaration => declaration(tokenizer),
        Step::End => end(tokenizer),
        Step::Instruction => instruction(tokenizer),
        Step::InstructionClose => instruction_close(tokenizer),
        Step::LineEndBefore => line_end_before(tokenizer),
        Step::LineEndAfter => line_end_after(tokenizer),
        Step::LineEndAfterPrefix => line_end_after_prefix(tokenizer),
    }
}
