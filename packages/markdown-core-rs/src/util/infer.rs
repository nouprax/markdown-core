//! Infer things from events.
//!
//! Used by the compiler to the canonical AST.

use crate::ast::Flow;
use crate::event::{Event, Kind, Name};
use alloc::{vec, vec::Vec};

/// Figure out if a list is spread or not.
///
/// When `include_items: true` is passed, infers whether the list as a whole
/// is “loose”.
pub fn list_loose(events: &[Event], mut index: usize, include_items: bool) -> bool {
    let mut balance = 0;
    // A blank line between items, which is one when an item follows it.
    let mut blank = false;
    let name = &events[index].name;
    debug_assert!(
        matches!(name, Name::ListOrdered | Name::ListUnordered),
        "expected list"
    );

    while index < events.len() {
        let event = &events[index];

        if event.kind == Kind::Enter {
            balance += 1;

            if balance == 2
                && event.name == Name::ListItem
                && (blank || (include_items && list_item_loose(events, index)))
            {
                return true;
            }
        } else {
            balance -= 1;

            if balance == 1 && event.name == Name::BlankLineEnding {
                // Blank line directly after item, which is just a prefix.
                //
                // ```markdown
                // > | -␊
                //      ^
                //   | - a
                // ```
                let mut at_bare_list_item = false;
                // Blank line at block quote prefix:
                //
                // ```markdown
                // > | * >␊
                //        ^
                //   | * a
                // ```
                let mut at_bare_block_quote = false;

                // List.
                let mut before = index - 2;

                if events[before].name == Name::ListItem {
                    before -= 1;

                    if matches!(
                        events[before].name,
                        Name::SpaceOrTab | Name::ContainerIndent
                    ) {
                        before -= 2;
                    }

                    // A quote that holds only its prefix, or a container
                    // directive only its opening fence, through the bare
                    // containers in it.
                    let mut at = before;
                    while matches!(events[at].name, Name::BlockQuote | Name::DirectiveContainer)
                        && events[at].kind == Kind::Exit
                    {
                        at -= 1;
                        if matches!(
                            events[at].name,
                            Name::BlockQuotePrefix | Name::DirectiveContainerFence
                        ) {
                            at_bare_block_quote = true;
                            break;
                        }
                    }
                    if !at_bare_block_quote && events[before].name == Name::ListItemPrefix {
                        at_bare_list_item = true;
                    }
                }

                if !at_bare_list_item && !at_bare_block_quote {
                    blank = true;
                }
            }

            // Done.
            if balance == 0 && event.name == *name {
                break;
            }
        }

        index += 1;
    }

    false
}

/// Figure out if an item is spread or not.
pub fn list_item_loose(events: &[Event], mut index: usize) -> bool {
    debug_assert!(
        matches!(events[index].name, Name::ListItem),
        "expected list item"
    );
    let mut balance = 0;
    // A blank line between blocks of the item, which is one when a block
    // follows it: blank lines that end the item are not.
    let mut blank = false;

    while index < events.len() {
        let event = &events[index];

        if event.kind == Kind::Enter {
            balance += 1;
            if blank && balance == 2 && !between(event.name) {
                return true;
            }
        } else {
            balance -= 1;

            if balance == 1 && event.name == Name::BlankLineEnding {
                // Blank line directly after a prefix:
                //
                // ```markdown
                // > | -␊
                //      ^
                //   |   a
                // ```
                let mut at_prefix = false;

                // List item.
                let mut before = index - 2;

                if matches!(
                    events[before].name,
                    Name::SpaceOrTab | Name::ContainerIndent
                ) {
                    before -= 2;
                }

                if events[before].name == Name::ListItemPrefix {
                    at_prefix = true;
                }

                // The line ending of a line that a quote or a container
                // directive in the item ended on, which is not blank:
                //
                // ```markdown
                // > | - >␊
                //        ^
                //   |   a
                // ```
                let enter = &events[index - 1];
                let ended = &events[index - 2];
                let inner = ended.kind == Kind::Exit
                    && matches!(ended.name, Name::BlockQuote | Name::DirectiveContainer)
                    && ended.point.line == enter.point.line;

                if !at_prefix && !inner {
                    blank = true;
                }
            }

            // Done.
            if balance == 0 && event.name == Name::ListItem {
                break;
            }
        }

        index += 1;
    }

    false
}

/// Whether a token directly in a list item is none of its blocks.
fn between(name: Name) -> bool {
    matches!(
        name,
        Name::LineEnding
            | Name::BlankLineEnding
            | Name::SpaceOrTab
            | Name::ContainerIndent
            | Name::ListItemPrefix
            // The prefix of a block quote around the list, on a line of
            // the item.
            | Name::BlockQuotePrefix
    )
}

/// Figure out the alignment of a GFM table.
pub fn table_align(events: &[Event], mut index: usize) -> Vec<Flow> {
    debug_assert!(matches!(events[index].name, Name::Table), "expected table");
    let mut in_delimiter_row = false;
    let mut align = vec![];

    while index < events.len() {
        let event = &events[index];

        if in_delimiter_row {
            if event.kind == Kind::Enter {
                // Start of alignment value: set a new column.
                if event.name == Name::TableDelimiterCellValue {
                    align.push(if events[index + 1].name == Name::TableDelimiterMarker {
                        Flow::Left
                    } else {
                        Flow::None
                    });
                }
            } else {
                // End of alignment value: change the column.
                if event.name == Name::TableDelimiterCellValue {
                    if events[index - 1].name == Name::TableDelimiterMarker {
                        let align_index = align.len() - 1;
                        align[align_index] = if align[align_index] == Flow::Left {
                            Flow::Center
                        } else {
                            Flow::Right
                        }
                    }
                }
                // Done!
                else if event.name == Name::TableDelimiterRow {
                    break;
                }
            }
        } else if event.kind == Kind::Enter && event.name == Name::TableDelimiterRow {
            in_delimiter_row = true;
        }

        index += 1;
    }

    align
}
