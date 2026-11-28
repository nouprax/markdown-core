//! No nesting depth overflows the stack: inputs nested 10 000 levels deep
//! parse and drop on a thread with a 256 KiB stack, the default of a
//! secondary thread on iOS being 512 KiB. The dump, whose lines indent with
//! their depth, so that its size is square in it, prints a tree of 2 000.

use std::thread;

/// Parse and drop `source` on a thread with a small stack, and print the
/// tree when it is no deeper than [`PRINTED`][].
fn deep(source: String) {
    thread::Builder::new()
        .stack_size(256 * 1024)
        .spawn(move || {
            let tree = markdown_core::parse(&source);
            if source.len() <= 4 * PRINTED {
                assert!(!markdown_core::dump::dump(&tree, &source).is_empty());
            }
        })
        .expect("expected a thread")
        .join()
        .expect("expected the parse not to fail");
}

const LEVELS: usize = 10_000;
const PRINTED: usize = 2_000;

#[test]
fn printed() {
    deep(format!("{}a\n", "> *".repeat(PRINTED / 2)));
}

#[test]
fn quotes() {
    deep(format!("{}a\n", ">".repeat(LEVELS)));
}

#[test]
fn emphasis() {
    deep(format!("{}a{}\n", "*".repeat(LEVELS), "*".repeat(LEVELS)));
}

#[test]
fn brackets() {
    deep(format!(
        "{}a{}\n",
        "[".repeat(LEVELS),
        "](u)".repeat(LEVELS)
    ));
}

#[test]
fn notes() {
    deep(format!("{}a{}\n", "^[".repeat(LEVELS), "]".repeat(LEVELS)));
}

#[test]
fn spans() {
    deep(format!(
        "{}a{}\n",
        "[".repeat(LEVELS),
        "]{.c}".repeat(LEVELS)
    ));
}

#[test]
fn quotes_and_emphasis() {
    deep(format!(
        "{}{}a{}\n",
        "> *".repeat(LEVELS / 2),
        "",
        "*".repeat(LEVELS / 2)
    ));
}

#[test]
fn properties() {
    deep(format!(
        "---\nk: {}1{}\n---\n",
        "[".repeat(LEVELS),
        "]".repeat(LEVELS)
    ));
}

#[test]
fn lists() {
    deep(format!("{}a\n", "- ".repeat(LEVELS)));
}

// Each line of an indented list holds all the indent before it, so the
// source is square in the depth.
#[test]
fn indented_lists() {
    let source: String = (0..1_000)
        .map(|level| format!("{}- a\n", "  ".repeat(level)))
        .collect();
    deep(source);
}

/// A tree deeper than any stack drops: each kind that owns nodes, in a
/// chain 200 000 deep, on the small stack, whether or not the parser's
/// stack growth is built in.
#[test]
fn drops() {
    use markdown_core::ast::{Kind, Node, Span};
    let shapes: [fn(Vec<Node>) -> Kind; 6] = [
        |content| Kind::Emphasis { content },
        |content| Kind::Paragraph { content },
        |items| Kind::List {
            flavor: markdown_core::ast::Flavor::Bullet,
            start: None,
            delimiter: None,
            tight: true,
            items,
        },
        |cells| Kind::TableRow { cells },
        |citations| Kind::Cite { citations },
        |definitions| Kind::DefinitionList { definitions },
    ];
    for shape in shapes {
        thread::Builder::new()
            .stack_size(256 * 1024)
            .spawn(move || {
                let mut node = Node::new(Kind::SoftBreak, vec![Span::new(0, 1)]);
                for _ in 0..200_000 {
                    node = Node::new(shape(vec![node]), vec![Span::new(0, 1)]);
                }
                drop(node);
            })
            .expect("expected a thread")
            .join()
            .expect("expected the drop not to overflow");
    }
}
