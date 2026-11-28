//! GFM: table occurs in the [flow][] content type.
//!
//! ## Grammar
//!
//! Tables form with the following BNF
//! (<small>see [construct][crate::construct] for character groups</small>):
//!
//! ```bnf
//! gfm_table ::= gfm_table_head 0*(eol gfm_table_body_row)
//!
//! ; Restriction: both rows must have the same number of cells.
//! gfm_table_head ::= gfm_table_row eol gfm_table_delimiter_row
//!
//! gfm_table_row ::= ['|'] gfm_table_cell 0*('|' gfm_table_cell) ['|'] *space_or_tab
//! gfm_table_cell ::= *space_or_tab gfm_table_text *space_or_tab
//! gfm_table_text ::= 0*(line - '\\' - '|' | '\\' ['\\' | '|'])
//
//! gfm_table_delimiter_row ::= ['|'] gfm_table_delimiter_cell 0*('|' gfm_table_delimiter_cell) ['|'] *space_or_tab
//! gfm_table_delimiter_cell ::= *space_or_tab gfm_table_delimiter_value *space_or_tab
//! gfm_table_delimiter_value ::= [':'] 1*'-' [':']
//! ```
//!
//! As this construct occurs in flow, like all flow constructs, it must be
//! followed by an eol (line ending) or eof (end of file).
//!
//! The above grammar shows that basically anything can be a cell or a row.
//! The main thing that makes something a row, is that it occurs directly before
//! or after a delimiter row, or after another row.
//!
//! It is not required for a table to have a body: it can end right after the
//! delimiter row.
//!
//! Each column can be marked with an alignment.
//! The alignment marker is a colon (`:`) used before and/or after delimiter row
//! filler.
//! To illustrate:
//!
//! ```markdown
//! | none | left | right | center |
//! | ---- | :--- | ----: | :----: |
//! ```
//!
//! The number of cells in the delimiter row, is the number of columns of the
//! table.
//! Only the head row is required to have the same number of cells.
//! Body rows are not required to have a certain number of cells.
//! For body rows that have less cells than the number of columns of the table,
//! empty cells are injected.
//! When a row has more cells than the number of columns of the table, the
//! superfluous cells are dropped.
//! To illustrate:
//!
//! ```markdown
//! | a | b |
//! | - | - |
//! | c |
//! | d | e | f |
//! ```
//!
//! Yields:
//!
//! ```html
//! <table>
//! <thead>
//! <tr>
//! <th>a</th>
//! <th>b</th>
//! </tr>
//! </thead>
//! <tbody>
//! <tr>
//! <td>c</td>
//! <td></td>
//! </tr>
//! <tr>
//! <td>d</td>
//! <td>e</td>
//! </tr>
//! </tbody>
//! </table>
//! ```
//!
//! Each cell’s text is interpreted as the [text][] content type.
//! That means that it can include constructs such as [attention][attention].
//!
//! The grammar for cells prohibits the use of `|` in them.
//! To use pipes in cells, encode them as a character reference or character
//! escape: `&vert;` (or `&VerticalLine;`, `&verbar;`, `&#124;`, `&#x7c;`) or
//! `\|`.
//!
//! Escapes will typically work, but they are not supported in
//! [code (text)][raw_text] (and formulas).
//! To work around this, GitHub came up with a rather weird “trick”.
//! When inside a table cell *and* inside code, escaped pipes *are* decoded.
//! To illustrate:
//!
//! ```markdown
//! | Name | Character |
//! | - | - |
//! | Left curly brace | `{` |
//! | Pipe | `\|` |
//! | Right curly brace | `}` |
//! ```
//!
//! Yields:
//!
//! ```html
//! <table>
//! <thead>
//! <tr>
//! <th>Name</th>
//! <th>Character</th>
//! </tr>
//! </thead>
//! <tbody>
//! <tr>
//! <td>Left curly brace</td>
//! <td><code>{</code></td>
//! </tr>
//! <tr>
//! <td>Pipe</td>
//! <td><code>|</code></td>
//! </tr>
//! <tr>
//! <td>Right curly brace</td>
//! <td><code>}</code></td>
//! </tr>
//! </tbody>
//! </table>
//! ```
//!
//! > 👉 **Note**: no other character can be escaped like this.
//! > Escaping pipes in code does not work when not inside a table, either.
//!
//! ## HTML
//!
//! GFM tables relate to several HTML elements: `<table>`, `<tbody>`, `<td>`,
//! `<th>`, `<thead>`, and `<tr>`.
//! See
//! [*§ 4.9.1 The `table` element*][html_table],
//! [*§ 4.9.5 The `tbody` element*][html_tbody],
//! [*§ 4.9.9 The `td` element*][html_td],
//! [*§ 4.9.10 The `th` element*][html_th],
//! [*§ 4.9.6 The `thead` element*][html_thead], and
//! [*§ 4.9.8 The `tr` element*][html_tr]
//! in the HTML spec for more info.
//!
//! If the alignment of a column is left, right, or center, a deprecated
//! `align` attribute is added to each `<th>` and `<td>` element belonging to
//! that column.
//! That attribute is interpreted by browsers as if a CSS `text-align` property
//! was included, with its value set to that same keyword.
//!
//! ## Recommendation
//!
//! When authoring markdown with GFM tables, it’s recommended to *always* put
//! pipes around cells.
//! Without them, it can be hard to infer whether the table will work, how many
//! columns there are, and which column you are currently editing.
//!
//! It is recommended to not use many columns, as it results in very long lines,
//! making it hard to infer which column you are currently editing.
//!
//! For larger tables, particularly when cells vary in size, it is recommended
//! *not* to manually “pad” cell text.
//! While it can look better, it results in a lot of time spent realigning
//! everything when a new, longer cell is added or the longest cell removed, as
//! every row then must be changed.
//! Other than costing time, it also causes large diffs in Git.
//!
//! To illustrate, when authoring large tables, it is discouraged to pad cells
//! like this:
//!
//! ```markdown
//! | Alpha bravo charlie |              delta |
//! | ------------------- | -----------------: |
//! | Echo                | Foxtrot golf hotel |
//! ```
//!
//! Instead, use single spaces (and single filler dashes):
//!
//! ```markdown
//! | Alpha bravo charlie | delta |
//! | - | -: |
//! | Echo | Foxtrot golf hotel |
//! ```
//!
//! ## Bugs
//!
//! GitHub’s own algorithm to parse tables contains a bug.
//! This bug is not present in this project.
//! The issue relating to tables is:
//!
//! * [GFM tables: escaped escapes are incorrectly treated as escapes](https://github.com/github/cmark-gfm/issues/277)
//!
//! ## Tokens
//!
//! * [`Table`][Name::Table]
//! * [`TableBody`][Name::TableBody]
//! * [`TableCell`][Name::TableCell]
//! * [`TableCellDivider`][Name::TableCellDivider]
//! * [`TableCellText`][Name::TableCellText]
//! * [`TableDelimiterCell`][Name::TableDelimiterCell]
//! * [`TableDelimiterCellValue`][Name::TableDelimiterCellValue]
//! * [`TableDelimiterFiller`][Name::TableDelimiterFiller]
//! * [`TableDelimiterMarker`][Name::TableDelimiterMarker]
//! * [`TableDelimiterRow`][Name::TableDelimiterRow]
//! * [`TableHead`][Name::TableHead]
//! * [`TableRow`][Name::TableRow]
//! * [`LineEnding`][Name::LineEnding]
//!
//! ## References
//!
//! * [`micromark-extension-gfm-table`](https://github.com/micromark/micromark-extension-gfm-table)
//! * [*§ 4.10 Tables (extension)* in `GFM`](https://github.github.com/gfm/#tables-extension-)
//!
//! [flow]: crate::construct::flow
//! [text]: crate::construct::text
//! [attention]: crate::construct::attention
//! [raw_text]: crate::construct::raw_text
//! [html_table]: https://html.spec.whatwg.org/multipage/tables.html#the-table-element
//! [html_tbody]: https://html.spec.whatwg.org/multipage/tables.html#the-tbody-element
//! [html_td]: https://html.spec.whatwg.org/multipage/tables.html#the-td-element
//! [html_th]: https://html.spec.whatwg.org/multipage/tables.html#the-th-element
//! [html_thead]: https://html.spec.whatwg.org/multipage/tables.html#the-thead-element
//! [html_tr]: https://html.spec.whatwg.org/multipage/tables.html#the-tr-element

use crate::choice::Starts;
use crate::construct::parts::space_or_tab::{space_or_tab, space_or_tab_min_max};
use crate::event::{Content, Event, Kind, Link, Name};
use crate::resolve::Name as ResolveName;
use crate::state::{Name as StateName, State};
use crate::subtokenize::Subresult;
use crate::tokenizer::Tokenizer;
use crate::util::{constant::TAB_SIZE, skip::opt_back as skip_opt_back};
use alloc::vec;

/// Bytes this construct can start at.
pub const STARTS: Starts = Starts::Any;

/// Whether a table goes on at this line: a row follows its head or rows,
/// no new container pierces it, and it is no lazy line.
pub fn open(tokenizer: &Tokenizer) -> bool {
    !tokenizer.pierce
        && !tokenizer.lazy
        && !tokenizer.events.is_empty()
        && matches!(
            tokenizer.events[skip_opt_back(
                &tokenizer.events,
                tokenizer.events.len() - 1,
                &[Name::LineEnding, Name::SpaceOrTab],
            )]
            .name,
            Name::TableHead | Name::TableRow
        )
}

/// Start of a GFM table.
///
/// If there is a valid table row or table head before, then we try to parse
/// another row.
/// Otherwise, we try to parse a head.
///
/// ```markdown
/// > | | a |
///     ^
///   | | - |
/// > | | b |
///     ^
/// ```
fn start(tokenizer: &mut Tokenizer) -> State {
    if open(tokenizer) {
        State::Redo(StateName::Table(Step::BodyRowStart))
    } else {
        State::Redo(StateName::Table(Step::HeadRowBefore {
            head: Head::default(),
        }))
    }
}

/// Before table head row.
///
/// ```markdown
/// > | | a |
///     ^
///   | | - |
///   | | b |
/// ```
fn head_row_before(tokenizer: &mut Tokenizer, head: Head) -> State {
    tokenizer.enter(Name::TableHead);
    tokenizer.enter(Name::TableRow);
    if matches!(tokenizer.current, Some(b'\t' | b' ')) {
        tokenizer.attempt(
            State::Next(StateName::Table(Step::HeadRowStart { head })),
            State::Nok,
        );
        // A head that goes on a paragraph is a line of it, which any
        // indent leaves a paragraph line.
        let max = if tokenizer.interrupt {
            usize::MAX
        } else {
            TAB_SIZE - 1
        };
        State::Redo(space_or_tab_min_max(0, max))
    } else {
        State::Redo(StateName::Table(Step::HeadRowStart { head }))
    }
}

/// Before table head row, after whitespace.
///
/// ```markdown
/// > | | a |
///     ^
///   | | - |
///   | | b |
/// ```
fn head_row_start(tokenizer: &mut Tokenizer, mut head: Head) -> State {
    match tokenizer.current {
        // 4+ spaces.
        Some(b'\t' | b' ') if !tokenizer.interrupt => State::Nok,
        Some(b'|') => State::Redo(StateName::Table(Step::HeadRowBreak { head })),
        _ => {
            head.seen = true;
            // Count the first character, that isn’t a pipe, double.
            head.count += 1;
            State::Redo(StateName::Table(Step::HeadRowBreak { head }))
        }
    }
}

/// At break in table head row.
///
/// ```markdown
/// > | | a |
///     ^
///       ^
///         ^
///   | | - |
///   | | b |
/// ```
fn head_row_break(tokenizer: &mut Tokenizer, mut head: Head) -> State {
    match tokenizer.current {
        None => State::Nok,
        Some(b'\n') => {
            // If anything other than one pipe (ignoring whitespace) was used, it’s fine.
            if head.count > 1 {
                head.count = 0;
                // Feel free to interrupt:
                tokenizer.interrupt = true;
                tokenizer.exit(Name::TableRow);
                tokenizer.enter(Name::LineEnding);
                tokenizer.consume();
                tokenizer.exit(Name::LineEnding);
                State::Next(StateName::Table(Step::HeadDelimiterStart { head }))
            } else {
                State::Nok
            }
        }
        Some(b'\t' | b' ') => {
            tokenizer.attempt(
                State::Next(StateName::Table(Step::HeadRowBreak { head })),
                State::Nok,
            );
            State::Redo(space_or_tab())
        }
        _ => {
            head.count += 1;

            // Whether a delimiter was seen.
            if head.seen {
                head.seen = false;
                // Header cell count.
                head.cells += 1;
            }

            if tokenizer.current == Some(b'|') {
                tokenizer.enter(Name::TableCellDivider);
                tokenizer.consume();
                tokenizer.exit(Name::TableCellDivider);
                // Whether a delimiter was seen.
                head.seen = true;
                State::Next(StateName::Table(Step::HeadRowBreak { head }))
            } else {
                // Anything else is cell data.
                tokenizer.enter(Name::Data);
                State::Redo(StateName::Table(Step::HeadRowData { head }))
            }
        }
    }
}

/// In table head row data.
///
/// ```markdown
/// > | | a |
///       ^
///   | | - |
///   | | b |
/// ```
fn head_row_data(tokenizer: &mut Tokenizer, head: Head) -> State {
    match tokenizer.current {
        None | Some(b'\t' | b'\n' | b' ' | b'|') => {
            tokenizer.exit(Name::Data);
            State::Redo(StateName::Table(Step::HeadRowBreak { head }))
        }
        _ => {
            let name = if tokenizer.current == Some(b'\\') {
                StateName::Table(Step::HeadRowEscape { head })
            } else {
                StateName::Table(Step::HeadRowData { head })
            };
            tokenizer.consume();
            State::Next(name)
        }
    }
}

/// In table head row escape.
///
/// ```markdown
/// > | | a\-b |
///         ^
///   | | ---- |
///   | | c    |
/// ```
fn head_row_escape(tokenizer: &mut Tokenizer, head: Head) -> State {
    // A backslash before a pipe keeps it in the cell, even after another
    // backslash: the pipe is split before any escape is read.
    match tokenizer.current {
        Some(b'|') => {
            tokenizer.consume();
            State::Next(StateName::Table(Step::HeadRowData { head }))
        }
        _ => State::Redo(StateName::Table(Step::HeadRowData { head })),
    }
}

/// Before delimiter row.
///
/// ```markdown
///   | | a |
/// > | | - |
///     ^
///   | | b |
/// ```
fn head_delimiter_start(tokenizer: &mut Tokenizer, mut head: Head) -> State {
    // Reset `interrupt`.
    tokenizer.interrupt = false;

    if tokenizer.lazy || tokenizer.pierce {
        State::Nok
    } else {
        tokenizer.enter(Name::TableDelimiterRow);
        // Track if we’ve seen a `:` or `|`.
        head.seen = false;

        match tokenizer.current {
            Some(b'\t' | b' ') => {
                tokenizer.attempt(
                    State::Next(StateName::Table(Step::HeadDelimiterBefore { head })),
                    State::Next(StateName::Table(Step::HeadDelimiterNok)),
                );

                State::Redo(space_or_tab_min_max(0, TAB_SIZE - 1))
            }
            _ => State::Redo(StateName::Table(Step::HeadDelimiterBefore { head })),
        }
    }
}

/// Before delimiter row, after optional whitespace.
///
/// Reused when a `|` is found later, to parse another cell.
///
/// ```markdown
///   | | a |
/// > | | - |
///     ^
///   | | b |
/// ```
fn head_delimiter_before(tokenizer: &mut Tokenizer, mut head: Head) -> State {
    match tokenizer.current {
        Some(b'-' | b':') => State::Redo(StateName::Table(Step::HeadDelimiterValueBefore { head })),
        Some(b'|') => {
            head.seen = true;
            // If we start with a pipe, we open a cell marker.
            tokenizer.enter(Name::TableCellDivider);
            tokenizer.consume();
            tokenizer.exit(Name::TableCellDivider);
            State::Next(StateName::Table(Step::HeadDelimiterCellBefore { head }))
        }
        // More whitespace / empty row not allowed at start.
        _ => State::Redo(StateName::Table(Step::HeadDelimiterNok)),
    }
}

/// After `|`, before delimiter cell.
///
/// ```markdown
///   | | a |
/// > | | - |
///      ^
/// ```
fn head_delimiter_cell_before(tokenizer: &mut Tokenizer, head: Head) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.attempt(
                State::Next(StateName::Table(Step::HeadDelimiterValueBefore { head })),
                State::Nok,
            );
            State::Redo(space_or_tab())
        }
        _ => State::Redo(StateName::Table(Step::HeadDelimiterValueBefore { head })),
    }
}

/// Before delimiter cell value.
///
/// ```markdown
///   | | a |
/// > | | - |
///       ^
/// ```
fn head_delimiter_value_before(tokenizer: &mut Tokenizer, mut head: Head) -> State {
    match tokenizer.current {
        None | Some(b'\n') => State::Redo(StateName::Table(Step::HeadDelimiterCellAfter { head })),
        Some(b':') => {
            // Align: left.
            head.count += 1;
            head.seen = true;
            tokenizer.enter(Name::TableDelimiterMarker);
            tokenizer.consume();
            tokenizer.exit(Name::TableDelimiterMarker);
            State::Next(StateName::Table(Step::HeadDelimiterLeftAlignmentAfter {
                head,
            }))
        }
        Some(b'-') => {
            // Align: none.
            head.count += 1;
            State::Redo(StateName::Table(Step::HeadDelimiterLeftAlignmentAfter {
                head,
            }))
        }
        _ => State::Redo(StateName::Table(Step::HeadDelimiterNok)),
    }
}

/// After delimiter cell left alignment marker.
///
/// ```markdown
///   | | a  |
/// > | | :- |
///        ^
/// ```
fn head_delimiter_left_alignment_after(tokenizer: &mut Tokenizer, head: Head) -> State {
    match tokenizer.current {
        Some(b'-') => {
            tokenizer.enter(Name::TableDelimiterFiller);
            State::Redo(StateName::Table(Step::HeadDelimiterFiller { head }))
        }
        // Anything else is not ok after the left-align colon.
        _ => State::Redo(StateName::Table(Step::HeadDelimiterNok)),
    }
}

/// In delimiter cell filler.
///
/// ```markdown
///   | | a |
/// > | | - |
///       ^
/// ```
fn head_delimiter_filler(tokenizer: &mut Tokenizer, mut head: Head) -> State {
    match tokenizer.current {
        Some(b'-') => {
            tokenizer.consume();
            State::Next(StateName::Table(Step::HeadDelimiterFiller { head }))
        }
        Some(b':') => {
            // Align is `center` if it was `left`, `right` otherwise.
            head.seen = true;
            tokenizer.exit(Name::TableDelimiterFiller);
            tokenizer.enter(Name::TableDelimiterMarker);
            tokenizer.consume();
            tokenizer.exit(Name::TableDelimiterMarker);
            State::Next(StateName::Table(Step::HeadDelimiterRightAlignmentAfter {
                head,
            }))
        }
        _ => {
            tokenizer.exit(Name::TableDelimiterFiller);
            State::Redo(StateName::Table(Step::HeadDelimiterRightAlignmentAfter {
                head,
            }))
        }
    }
}

/// After delimiter cell right alignment marker.
///
/// ```markdown
///   | |  a |
/// > | | -: |
///         ^
/// ```
fn head_delimiter_right_alignment_after(tokenizer: &mut Tokenizer, head: Head) -> State {
    match tokenizer.current {
        Some(b'\t' | b' ') => {
            tokenizer.attempt(
                State::Next(StateName::Table(Step::HeadDelimiterCellAfter { head })),
                State::Nok,
            );
            State::Redo(space_or_tab())
        }
        _ => State::Redo(StateName::Table(Step::HeadDelimiterCellAfter { head })),
    }
}

/// After delimiter cell.
///
/// ```markdown
///   | |  a |
/// > | | -: |
///          ^
/// ```
fn head_delimiter_cell_after(tokenizer: &mut Tokenizer, head: Head) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            // Exit when:
            // * there was no `:` or `|` at all (it’s a thematic break or setext
            //   underline instead)
            // * the header cell count is not the delimiter cell count
            if !head.seen || head.cells != head.count {
                State::Redo(StateName::Table(Step::HeadDelimiterNok))
            } else {
                tokenizer.exit(Name::TableDelimiterRow);
                tokenizer.exit(Name::TableHead);
                tokenizer.register_resolver(ResolveName::Table);
                State::Ok
            }
        }
        Some(b'|') => State::Redo(StateName::Table(Step::HeadDelimiterBefore { head })),
        _ => State::Redo(StateName::Table(Step::HeadDelimiterNok)),
    }
}

/// In delimiter row, at a disallowed byte.
///
/// ```markdown
///   | | a |
/// > | | x |
///       ^
/// ```
fn head_delimiter_nok(_tokenizer: &mut Tokenizer) -> State {
    State::Nok
}

/// Before table body row.
///
/// ```markdown
///   | | a |
///   | | - |
/// > | | b |
///     ^
/// ```
fn body_row_start(tokenizer: &mut Tokenizer) -> State {
    if tokenizer.lazy {
        State::Nok
    } else {
        tokenizer.enter(Name::TableRow);

        match tokenizer.current {
            Some(b'\t' | b' ') => {
                tokenizer.attempt(
                    State::Next(StateName::Table(Step::BodyRowBreak)),
                    State::Nok,
                );
                // We’re parsing a body row.
                // If we’re here, we already attempted blank lines and indented
                // code.
                // So parse as much whitespace as needed:
                State::Redo(space_or_tab_min_max(0, usize::MAX))
            }
            _ => State::Redo(StateName::Table(Step::BodyRowBreak)),
        }
    }
}

/// At break in table body row.
///
/// ```markdown
///   | | a |
///   | | - |
/// > | | b |
///     ^
///       ^
///         ^
/// ```
fn body_row_break(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\n') => {
            tokenizer.exit(Name::TableRow);
            State::Ok
        }
        Some(b'\t' | b' ') => {
            tokenizer.attempt(
                State::Next(StateName::Table(Step::BodyRowBreak)),
                State::Nok,
            );
            State::Redo(space_or_tab())
        }
        Some(b'|') => {
            tokenizer.enter(Name::TableCellDivider);
            tokenizer.consume();
            tokenizer.exit(Name::TableCellDivider);
            State::Next(StateName::Table(Step::BodyRowBreak))
        }
        // Anything else is cell content.
        _ => {
            tokenizer.enter(Name::Data);
            State::Redo(StateName::Table(Step::BodyRowData))
        }
    }
}

/// In table body row data.
///
/// ```markdown
///   | | a |
///   | | - |
/// > | | b |
///       ^
/// ```
fn body_row_data(tokenizer: &mut Tokenizer) -> State {
    match tokenizer.current {
        None | Some(b'\t' | b'\n' | b' ' | b'|') => {
            tokenizer.exit(Name::Data);
            State::Redo(StateName::Table(Step::BodyRowBreak))
        }
        _ => {
            let name = if tokenizer.current == Some(b'\\') {
                StateName::Table(Step::BodyRowEscape)
            } else {
                StateName::Table(Step::BodyRowData)
            };
            tokenizer.consume();
            State::Next(name)
        }
    }
}

/// In table body row escape.
///
/// ```markdown
///   | | a    |
///   | | ---- |
/// > | | b\-c |
///         ^
/// ```
fn body_row_escape(tokenizer: &mut Tokenizer) -> State {
    // A backslash before a pipe keeps it in the cell, even after another
    // backslash: the pipe is split before any escape is read.
    match tokenizer.current {
        Some(b'|') => {
            tokenizer.consume();
            State::Next(StateName::Table(Step::BodyRowData))
        }
        _ => State::Redo(StateName::Table(Step::BodyRowData)),
    }
}

/// Resolve GFM table.
pub fn resolve(tokenizer: &mut Tokenizer) -> Option<Subresult> {
    let mut index = 0;
    let mut in_first_cell_before_pipe = true;
    let mut in_row = false;
    let mut in_delimiter_row = false;
    let mut last_cell = (0, 0, 0, 0);
    let mut cell = (0, 0, 0, 0);
    let mut after_head_before_first_body_row = false;
    let mut last_table_end = 0;
    let mut last_table_has_body = false;

    while index < tokenizer.events.len() {
        let event = &tokenizer.events[index];

        if event.kind == Kind::Enter {
            // Start of head.
            if event.name == Name::TableHead {
                after_head_before_first_body_row = false;

                // Inject previous (body end and) table end.
                if last_table_end != 0 {
                    flush_table_end(tokenizer, last_table_end, last_table_has_body);
                    last_table_has_body = false;
                    last_table_end = 0;
                }

                // Inject table start.
                let enter = Event {
                    kind: Kind::Enter,
                    name: Name::Table,
                    point: tokenizer.events[index].point.clone(),
                    link: None,
                };
                tokenizer.map.add(index, 0, vec![enter]);
            } else if matches!(event.name, Name::TableRow | Name::TableDelimiterRow) {
                in_delimiter_row = event.name == Name::TableDelimiterRow;
                in_row = true;
                in_first_cell_before_pipe = true;
                last_cell = (0, 0, 0, 0);
                cell = (0, index + 1, 0, 0);

                // Inject table body start.
                if after_head_before_first_body_row {
                    after_head_before_first_body_row = false;
                    last_table_has_body = true;
                    let enter = Event {
                        kind: Kind::Enter,
                        name: Name::TableBody,
                        point: tokenizer.events[index].point.clone(),
                        link: None,
                    };
                    tokenizer.map.add(index, 0, vec![enter]);
                }
            }
            // Cell data.
            else if in_row
                && matches!(
                    event.name,
                    Name::Data | Name::TableDelimiterMarker | Name::TableDelimiterFiller
                )
            {
                in_first_cell_before_pipe = false;

                // First value in cell.
                if cell.2 == 0 {
                    if last_cell.1 != 0 {
                        cell.0 = cell.1;
                        flush_cell(tokenizer, last_cell, in_delimiter_row, None);
                        last_cell = (0, 0, 0, 0);
                    }

                    cell.2 = index;
                }
            } else if event.name == Name::TableCellDivider {
                if in_first_cell_before_pipe {
                    in_first_cell_before_pipe = false;
                } else {
                    if last_cell.1 != 0 {
                        cell.0 = cell.1;
                        flush_cell(tokenizer, last_cell, in_delimiter_row, None);
                    }

                    last_cell = cell;
                    cell = (last_cell.1, index, 0, 0);
                }
            }
        // Exit events.
        } else if event.name == Name::TableHead {
            after_head_before_first_body_row = true;
            last_table_end = index;
        } else if matches!(event.name, Name::TableRow | Name::TableDelimiterRow) {
            in_row = false;
            last_table_end = index;
            if last_cell.1 != 0 {
                cell.0 = cell.1;
                flush_cell(tokenizer, last_cell, in_delimiter_row, Some(index));
            } else if cell.1 != 0 {
                flush_cell(tokenizer, cell, in_delimiter_row, Some(index));
            }
        } else if in_row
            && (matches!(
                event.name,
                Name::Data | Name::TableDelimiterMarker | Name::TableDelimiterFiller
            ))
        {
            cell.3 = index;
        }

        index += 1;
    }

    if last_table_end != 0 {
        flush_table_end(tokenizer, last_table_end, last_table_has_body);
    }

    tokenizer.map.consume(&mut tokenizer.events);
    None
}

/// Generate a cell.
fn flush_cell(
    tokenizer: &mut Tokenizer,
    span: (usize, usize, usize, usize),
    in_delimiter_row: bool,
    row_end: Option<usize>,
) {
    let group_name = if in_delimiter_row {
        Name::TableDelimiterCell
    } else {
        Name::TableCell
    };
    let value_name = if in_delimiter_row {
        Name::TableDelimiterCellValue
    } else {
        Name::TableCellText
    };

    // Insert an exit for the previous cell, if there is one.
    //
    // ```markdown
    // > | | aa | bb | cc |
    //          ^-- exit
    //           ^^^^-- this cell
    // ```
    if span.0 != 0 {
        tokenizer.map.add(
            span.0,
            0,
            vec![Event {
                kind: Kind::Exit,
                name: group_name,
                point: tokenizer.events[span.0].point.clone(),
                link: None,
            }],
        );
    }

    // Insert enter of this cell.
    //
    // ```markdown
    // > | | aa | bb | cc |
    //           ^-- enter
    //           ^^^^-- this cell
    // ```
    tokenizer.map.add(
        span.1,
        0,
        vec![Event {
            kind: Kind::Enter,
            name: group_name,
            point: tokenizer.events[span.1].point.clone(),
            link: None,
        }],
    );

    // Insert text start at first data start and end at last data end, and
    // remove events between.
    //
    // ```markdown
    // > | | aa | bb | cc |
    //            ^-- enter
    //             ^-- exit
    //           ^^^^-- this cell
    // ```
    if span.2 != 0 {
        tokenizer.map.add(
            span.2,
            0,
            vec![Event {
                kind: Kind::Enter,
                name: value_name,
                point: tokenizer.events[span.2].point.clone(),
                link: None,
            }],
        );
        debug_assert_ne!(span.3, 0);

        if !in_delimiter_row {
            tokenizer.events[span.2].link = Some(Link {
                previous: None,
                next: None,
                content: Content::Cell,
            });

            // To do: positional info of the remaining `data` nodes likely have
            // to be fixed.
            if span.3 > span.2 + 1 {
                let a = span.2 + 1;
                let b = span.3 - span.2 - 1;
                tokenizer.map.add(a, b, vec![]);
            }
        }

        tokenizer.map.add(
            span.3 + 1,
            0,
            vec![Event {
                kind: Kind::Exit,
                name: value_name,
                point: tokenizer.events[span.3].point.clone(),
                link: None,
            }],
        );
    }

    // Insert an exit for the last cell, if at the row end.
    //
    // ```markdown
    // > | | aa | bb | cc |
    //                    ^-- exit
    //               ^^^^^^-- this cell (the last one contains two “between” parts)
    // ```
    if let Some(row_end) = row_end {
        tokenizer.map.add(
            row_end,
            0,
            vec![Event {
                kind: Kind::Exit,
                name: group_name,
                point: tokenizer.events[row_end].point.clone(),
                link: None,
            }],
        );
    }
}

/// Generate table end (and table body end).
fn flush_table_end(tokenizer: &mut Tokenizer, index: usize, body: bool) {
    let mut exits = vec![];

    if body {
        exits.push(Event {
            kind: Kind::Exit,
            name: Name::TableBody,
            point: tokenizer.events[index].point.clone(),
            link: None,
        });
    }

    exits.push(Event {
        kind: Kind::Exit,
        name: Name::Table,
        point: tokenizer.events[index].point.clone(),
        link: None,
    });

    tokenizer.map.add(index + 1, 0, exits);
}

/// What the head of a table has counted so far.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct Head {
    /// Whether a delimiter (`|`, or `:` in the delimiter row) was seen.
    seen: bool,
    /// Number of cells in the head row.
    cells: usize,
    /// In the head row, the number of non-whitespace parts; in the
    /// delimiter row, the number of delimiter cells.
    count: usize,
}

/// Steps of this construct.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Step {
    Start,
    HeadRowBefore { head: Head },
    HeadRowStart { head: Head },
    HeadRowBreak { head: Head },
    HeadRowData { head: Head },
    HeadRowEscape { head: Head },
    HeadDelimiterStart { head: Head },
    HeadDelimiterBefore { head: Head },
    HeadDelimiterCellBefore { head: Head },
    HeadDelimiterValueBefore { head: Head },
    HeadDelimiterLeftAlignmentAfter { head: Head },
    HeadDelimiterFiller { head: Head },
    HeadDelimiterRightAlignmentAfter { head: Head },
    HeadDelimiterCellAfter { head: Head },
    HeadDelimiterNok,
    BodyRowStart,
    BodyRowBreak,
    BodyRowData,
    BodyRowEscape,
}

impl From<Step> for StateName {
    fn from(step: Step) -> StateName {
        StateName::Table(step)
    }
}

/// Run a step.
pub fn step(tokenizer: &mut Tokenizer, step: Step) -> State {
    match step {
        Step::Start => start(tokenizer),
        Step::HeadRowBefore { head } => head_row_before(tokenizer, head),
        Step::HeadRowStart { head } => head_row_start(tokenizer, head),
        Step::HeadRowBreak { head } => head_row_break(tokenizer, head),
        Step::HeadRowData { head } => head_row_data(tokenizer, head),
        Step::HeadRowEscape { head } => head_row_escape(tokenizer, head),
        Step::HeadDelimiterStart { head } => head_delimiter_start(tokenizer, head),
        Step::HeadDelimiterBefore { head } => head_delimiter_before(tokenizer, head),
        Step::HeadDelimiterCellBefore { head } => head_delimiter_cell_before(tokenizer, head),
        Step::HeadDelimiterValueBefore { head } => head_delimiter_value_before(tokenizer, head),
        Step::HeadDelimiterLeftAlignmentAfter { head } => {
            head_delimiter_left_alignment_after(tokenizer, head)
        }
        Step::HeadDelimiterFiller { head } => head_delimiter_filler(tokenizer, head),
        Step::HeadDelimiterRightAlignmentAfter { head } => {
            head_delimiter_right_alignment_after(tokenizer, head)
        }
        Step::HeadDelimiterCellAfter { head } => head_delimiter_cell_after(tokenizer, head),
        Step::HeadDelimiterNok => head_delimiter_nok(tokenizer),
        Step::BodyRowStart => body_row_start(tokenizer),
        Step::BodyRowBreak => body_row_break(tokenizer),
        Step::BodyRowData => body_row_data(tokenizer),
        Step::BodyRowEscape => body_row_escape(tokenizer),
    }
}
