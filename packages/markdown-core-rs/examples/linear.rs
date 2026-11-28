//! Sources that nest or repeat one construct, for
//! `scripts/correctness/check-rust-linear.mjs`, which counts the
//! instructions of [`run`][], which parses and drops, on each at two sizes with
//! callgrind: parse cost is linear in the size of the source.
//!
//! `linear` lists the shapes, one per line, with the size each is counted
//! at; `linear <shape> <size>` parses that shape of that size.

use std::env;
use std::hint::black_box;

/// A shape: its name, the size it is counted at, and its source of a size.
type Shape = (&'static str, usize, fn(usize) -> String);

const SHAPES: &[Shape] = &[
    ("emphasis", 2_000, |n| {
        format!("{}a{}\n", "*".repeat(n), "*".repeat(n))
    }),
    ("emphasis-in-brackets", 2_000, |n| {
        format!("{}a{}\n", "*[".repeat(n), "]*".repeat(n))
    }),
    ("mixed-emphasis", 2_000, |n| {
        format!("{}a{}\n", "*_".repeat(n), "_*".repeat(n))
    }),
    ("insertions-in-brackets", 2_000, |n| {
        format!("{}a{}\n", "++[".repeat(n), "]++".repeat(n))
    }),
    ("links", 2_000, |n| {
        format!("{}a{}\n", "[".repeat(n), "](u)".repeat(n))
    }),
    ("brackets", 8_000, |n| {
        format!("{}a{}\n", "[".repeat(n), "]".repeat(n))
    }),
    ("images", 2_000, |n| {
        format!("{}a{}\n", "![".repeat(n), "](u)".repeat(n))
    }),
    ("keys", 8_000, |n| {
        format!("{}@a{}\n", "[".repeat(n), "]".repeat(n))
    }),
    ("tails", 2_000, |n| {
        format!("{}b{}\n", "@a [".repeat(n), "]".repeat(n))
    }),
    ("notes", 2_000, |n| {
        format!("{}a{}\n", "^[".repeat(n), "]".repeat(n))
    }),
    ("spans", 2_000, |n| {
        format!("{}a{}\n", "[".repeat(n), "]{.c}".repeat(n))
    }),
    ("quotes", 2_000, |n| format!("{}a\n", ">".repeat(n))),
    ("lists", 2_000, |n| format!("{}a\n", "- ".repeat(n))),
    // The source is square in the depth: a depth twice as deep is four
    // times the size.
    ("indented-lists", 40_000, |n| {
        let depth = n.isqrt();
        (0..depth)
            .map(|level| format!("{}- a\n", "  ".repeat(level)))
            .collect()
    }),
    ("ordered-lists", 40_000, |n| {
        let depth = n.isqrt();
        (0..depth)
            .map(|level| format!("{}1. a\n", "   ".repeat(level)))
            .collect()
    }),
    ("closers", 2_000, |n| "a* ".repeat(n)),
    ("strong-openers", 2_000, |n| {
        format!("{}{}", "**a ".repeat(n), "b*".repeat(n))
    }),
    ("dollars", 2_000, |n| "$a ".repeat(n)),
    ("round-formulas", 2_000, |n| "\\\\(a ".repeat(n)),
    ("square-formulas", 2_000, |n| "\\\\[a ".repeat(n)),
    ("ticked-dollars", 2_000, |n| "$`a ".repeat(n)),
    ("code", 2_000, |n| "`a ".repeat(n)),
    ("shortcut-references", 2_000, |n| {
        format!("[a]: /u\n\n{}", "[a] ".repeat(n))
    }),
];

/// Parse and drop `source`: what is counted.
#[inline(never)]
fn run(source: &str) {
    drop(black_box(markdown_core::parse(black_box(source))));
}

fn main() {
    let arguments: Vec<String> = env::args().skip(1).collect();
    match arguments.as_slice() {
        [] => {
            for (name, size, _) in SHAPES {
                println!("{name} {size}");
            }
        }
        [name, size] => {
            let (_, _, shape) = SHAPES
                .iter()
                .find(|(each, _, _)| each == name)
                .expect("expected a shape");
            let source = shape(size.parse().expect("expected a size"));
            run(&source);
        }
        _ => panic!("expected no arguments, or a shape and a size"),
    }
}
