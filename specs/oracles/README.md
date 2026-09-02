# Whitelisted oracles

These six files are the reviewed syntax-extension requirement corpus used by
the parity and reconstruction tests.

**96 examples**, covering the two named deliverables — the directive syntax fix
and the formula fix:

| File | Examples | States |
|---|---|---|
| `extensions-directive.txt` | 51 | the directive grammar: names, attribute forms, the `#`/`.` shorthand, class accumulation, malformed-attribute degradation |
| `extensions-directive-option-gates.txt` | 2 | what the directive option admits |
| `extensions-formula-github.txt` | 19 | the dollar forms and the inline-math padding rule |
| `extensions-formula-latex.txt` | 12 | the `\(` and `\[` forms |
| `extensions-formula-option-gates.txt` | 5 | which delimiter sets each option admits |
| `extensions-formula-conflicts.txt` | 7 | formula against the other extensions |

## What these are, and are not

They are **requirements**. Each example is an input and the behavior that the
current parser must produce.

They are **not a second AST contract**. The canonical node vocabulary and field
semantics come from `docs/specs/canonical-ast.json`; these fixtures constrain
the grammar and extension option behavior.

## Why oracles and not code

A fixture says *what the engine must do*. An implementation says only one way
to do it. Parser changes should preserve the requirement without importing a
second algorithm or lifecycle.
