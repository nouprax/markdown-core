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
    let mut ends_blank = false;
    let name = &events[index].name;
    debug_assert!(
        matches!(name, Name::ListOrdered | Name::ListUnordered),
        "expected list"
    );

    while index < events.len() {
        let event = &events[index];

        if event.kind == Kind::Enter {
            balance += 1;

            if include_items && balance == 2 && event.name == Name::ListItem {
                // An item that ends with a blank line makes the list loose
                // when another item follows.
                if ends_blank {
                    return true;
                }
                let (loose, blank) = list_item_loose(events, index);
                if loose {
                    return true;
                }
                ends_blank = blank;
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

/// Figure out if an item is spread or not: a blank line after one of its
/// blocks, with another block after it; and whether it ends with a blank
/// line after a block.
///
/// A blank line belongs to the block before it. Definitions compile to no
/// block, so they neither end with a blank line nor follow one:
///
/// ```markdown
/// > | - a
///   |
///   |   [b]: c
/// ```
fn list_item_loose(events: &[Event], mut index: usize) -> (bool, bool) {
    debug_assert!(
        matches!(events[index].name, Name::ListItem),
        "expected list item"
    );
    let mut balance = 0;
    // Whether the last block before is one that compiles.
    let mut after_block = false;
    // Whether a blank line came after a block that compiles.
    let mut blank = false;

    while index < events.len() {
        let event = &events[index];

        if event.kind == Kind::Enter {
            balance += 1;

            if balance == 2 {
                match event.name {
                    Name::BlankLineEnding => blank = blank || after_block,
                    Name::Definition => after_block = false,
                    Name::ListItemPrefix
                    | Name::BlockQuotePrefix
                    | Name::SpaceOrTab
                    | Name::LineEnding
                    | Name::ListItemIndent
                    | Name::FootnoteDefinitionIndent => {}
                    _ => {
                        if blank {
                            return (true, false);
                        }
                        after_block = true;
                    }
                }
            }
        } else {
            balance -= 1;

            // Done.
            if balance == 0 && event.name == Name::ListItem {
                break;
            }
        }

        index += 1;
    }

    (false, blank)
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
