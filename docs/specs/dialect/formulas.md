# Formulas

Status: normative module of the [Markdown Core dialect](../dialect.md).
Option: `formulas` (default `true`). Source: GitHub's mathematical-expression
syntax and `micromark-extension-math` 3.1.0, whose padding rule this module
adopts. Executable oracle: remark with `micromark-extension-math` under
`specs/oracles/remark/`, for the `$` forms only; the backslash forms and the
GitHub heuristics are product fixtures and registered deltas. Landing: present.

The module states the grammar the parser implements in
`packages/markdown-core/extensions/formula.c`; the fixtures
`extensions-formula-github.txt`, `extensions-formula-latex.txt`,
`extensions-formula-conflicts.txt`, and `extensions-formula-option-gates.txt`
are its oracle of record.

## Model

```text
Formula(mode: embedded | standalone, literal: String)
FormulaBlock(literal: String)
```

`Formula` is an inline leaf; `mode` is the only placement field in the AST,
because it records a fact about the source. `FormulaBlock` is a block leaf and
is always standalone. Both literals are opaque strings: the parser validates no
TeX, decodes no escape or character reference inside a body, and runs no
renderer.

## Inline forms

Inline recognition is step A4 of the recognition order. Five source forms
exist; every one is opened at its opening delimiter during the scan, so its
body is opaque to every later step, including emphasis, links, and every other
module.

| Form           | Delimiters                     | Result                         |
| -------------- | ------------------------------ | ------------------------------ |
| dollar         | `$` ... `$`                    | `Formula(mode=embedded)`       |
| backtick       | `` $` `` ... `` `$ ``          | `Formula(mode=embedded)`       |
| display dollar | `$$` ... `$$`                  | `Formula(mode=standalone)`     |
| paren          | `\\(` ... `\\)`                | `Formula(mode=embedded)`       |
| bracket        | `\\[` ... `\\]`                | `Formula(mode=standalone)`     |

The paren and bracket forms are spelled with two authored backslashes. A
single-backslash `\(` or `\[` is an ordinary CommonMark escape and never a
formula delimiter.

Delimiter rules:

- `$$` is tested before `$`. A `$$` run opens and closes a display candidate
  unconditionally. A single `$` can open only when the next byte exists and is
  not ASCII whitespace, and can close only when the previous byte is not ASCII
  whitespace and the next byte, if any, is not an ASCII digit. `$300B and $100B`
  therefore contains no formula. An escaped `\$` is text.
- The backtick form is the dollar form whose body begins with a backtick: a
  matched `$`...`$` pair whose body starts with `` ` `` must end with `` ` ``,
  and the two backticks are removed from the literal; a body that starts with a
  backtick and does not end with one is not a formula, and the pair is released.
- `\\(` and `\\[` can only open; `\\)` and `\\]` can only close. An opener and
  closer match only when they are of the same form. Inside a paren or bracket
  body, `\)` respectively `\]` spelled with one backslash is unescaped to the
  bare bracket in the literal; every other byte is stored as written.
- Delimiters are units on the shared delimiter stack; a closer matches the
  nearest unmatched opener of its own form, and a body may span soft line
  breaks within one inline container but not a block boundary. An unmatched
  delimiter is text.

Padding: if the body begins and ends with a space, LF, or CR and is not made
entirely of those bytes, one such byte is removed from each end; otherwise the
body is stored as written. Tabs are not padding. `$$ mid$$` keeps its leading
space; `$$  x  $$` keeps one space on each side; `$$ $$` keeps both spaces.

## Block forms

Block recognition is step 3 of the block-start order. A line whose content,
after container prefixes and up to three spaces of indentation, is exactly
`$$` or `\\[` followed only by spaces or tabs opens a `FormulaBlock`. The
block may interrupt a paragraph. It is closed by the first later line whose
content, under the same prefixes and indentation bound, is exactly the
matching `$$` or `\\]` followed only by spaces or tabs; the lines between are
the literal with leading and trailing ASCII whitespace removed and interior
line endings kept as written. A block that reaches the end of its container or
of the document without a closer is still a `FormulaBlock` holding every line
after the opener.

A fenced code block whose `info` is exactly `formula` produces a
`FormulaBlock` whose literal is the code block's literal, trimmed of leading
and trailing ASCII whitespace; the code block's other fields are discarded.

A paragraph whose sole inline child is a standalone `Formula` (a `$$` or
`\\[` inline form with nothing else in the paragraph) is replaced by a
`FormulaBlock` with the same literal and the paragraph's scope. This is the
one post-pass of the module; it is bounded to paragraphs with exactly one
child and never changes a paragraph with other content.

## Option behavior and fallback

With `formulas=false`, every delimiter above is ordinary text under the
inherited grammar, a `formula` fence is a `CodeBlock`, and `$$` lines are
paragraph text. With the option on, a candidate that fails any rule above
releases its bytes as text and consumes nothing that a later construct needs.
Formula bodies are opaque under the shared opacity rule, and formula
delimiters inside code spans, HTML tokens, comments, and cross links are those
constructs' own bytes.

## Scopes

An inline `Formula` scope covers both delimiter runs and the body, including
padding bytes that were removed from the literal. A `FormulaBlock` scope covers
the opening line through the closing line, or through the last consumed line
when unclosed; a block produced from a `formula` fence or from a sole
standalone inline keeps the replaced node's scope.

## Required conformance cases

Every fixture row of the four formula fixture files stays byte-identical. Tests
also cover each of the five inline forms, the digit and whitespace rules on
`$`, the backtick form with and without its closing backtick, single-backslash
brackets as escapes, unmatched delimiters, padding on every form, the block
forms with and without closers and inside containers, the `formula` fence,
the sole-standalone paragraph replacement, formula delimiters inside every
opaque construct, exact scopes, option-off output, allocation failure, and
size-doubling runs of `$` and `\`.
