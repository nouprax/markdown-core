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
    let name = &events[index].name;
    debug_assert!(
        matches!(name, Name::ListOrdered | Name::ListUnordered),
        "expected list"
    );

    while index < events.len() {
        let event = &events[index];

        if event.kind == Kind::Enter {
            balance += 1;

            if include_items
                && balance == 2
                && event.name == Name::ListItem
                && list_item_loose(events, index)
            {
                return true;
            }
        } else {
            balance -= 1;

            if balance == 1 && event.name == Name::BlankLineEnd {
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
                        Name::SpaceOrTab | Name::ListItemIndent | Name::FootnoteDefinitionIndent
                    ) {
                        before -= 2;
                    }

                    if events[before].name == Name::BlockQuote
                        && events[before - 1].name == Name::BlockQuotePrefix
                    {
                        at_bare_block_quote = true;
                    } else if events[before].name == Name::ListItemPrefix {
                        at_bare_list_item = true;
                    }
                }

                if !at_bare_list_item && !at_bare_block_quote {
                    return true;
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

    while index < events.len() {
        let event = &events[index];

        if event.kind == Kind::Enter {
            balance += 1;
        } else {
            balance -= 1;

            if balance == 1 && event.name == Name::BlankLineEnd {
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
                    Name::SpaceOrTab | Name::ListItemIndent | Name::FootnoteDefinitionIndent
                ) {
                    before -= 2;
                }

                if events[before].name == Name::ListItemPrefix {
                    at_prefix = true;
                }

                if !at_prefix {
                    return true;
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

/// Figure out the alignment of a GFM table.
pub fn table_sides(events: &[Event], mut index: usize) -> Vec<Flow> {
    debug_assert!(matches!(events[index].name, Name::Table), "expected table");
    let mut in_delimiter_row = false;
    let mut side = vec![];

    while index < events.len() {
        let event = &events[index];

        if in_delimiter_row {
            if event.kind == Kind::Enter {
                // Start of alignment value: set a new column.
                if event.name == Name::TableDelimiterCellValue {
                    side.push(if events[index + 1].name == Name::TableDelimiterMarker {
                        Flow::Start
                    } else {
                        Flow::None
                    });
                }
            } else {
                // End of alignment value: change the column.
                if event.name == Name::TableDelimiterCellValue {
                    if events[index - 1].name == Name::TableDelimiterMarker {
                        let side_index = side.len() - 1;
                        side[side_index] = if side[side_index] == Flow::Start {
                            Flow::Center
                        } else {
                            Flow::End
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

    side
}
