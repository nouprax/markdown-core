# Whitelisted oracles

These six files are the **entire** permitted inheritance from this repository's
history after `580d10c`. They are extracted here so that the history never needs
to be opened again: the rule in [`docs/RECONSTRUCTION.md`](../../docs/RECONSTRUCTION.md)
§4.9 forbids reading any commit, and this directory is what that rule carves out.

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

They are **requirements**. Each example is an input and the behaviour that input
must produce, which is the thing the reconstruction lacks and cannot derive.

They are **not goldens to copy in**. Their expected output was produced by an
engine this branch is not rebuilding from, and it shows: the dump vocabulary
differs, some rows carry fields the 1.0 baseline has no concept of, and the
positions reflect fixes that are separately scheduled. Copying an expected block
verbatim would import an answer to a question nobody asked here.

**Read the input and the intent; derive the expected output from this engine.**
Where an example's expected output disagrees with what this engine should
produce, this engine is right and the example is stale — say so in the commit,
and record it, rather than bending the engine to match.

## Why oracles and not code

A fixture says *what the engine must do*. An implementation says *one way
somebody did it*. Taking the first is taking a requirement. Taking the second is
inheriting a design, including its defects — which is how eleven of them
survived into 1.0.3 in the first place.
