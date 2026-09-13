# Emphasis

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Wrap text in one asterisk or underscore for emphasis, or two for strong emphasis.

```markdown
*italic* and _also italic_

**bold** and __also bold__

***both***
```

The first paragraph contains `Emphasis` nodes, the second contains `Strong`
nodes, and the third nests the two kinds. The AST records formatting intent;
a renderer typically displays these as italic and bold text.

## Nest formatting

```markdown
**Read the *important* part** and *visit [the guide](/guide)*.
```

Strong content contains nested emphasis. The later emphasis contains a link.
Matching delimiters belong to the containing node's scope, while its children
cover their own content.

## Spaces and word boundaries

Opening delimiters cannot be followed by whitespace; closing delimiters cannot
be preceded by whitespace. CommonMark's punctuation and word-boundary rules
also apply:

```markdown
The * spaced * pair stays literal.

word*inside*word and word_inside_word
```

The spaced pair remains text. Asterisks can emphasize `inside` within a word;
underscores in the second spelling stay literal. Ambiguous runs follow
CommonMark's delimiter matching, including its rule of three for runs that
could both open and close.

Escaped signs and signs inside [code](code.md) do not participate. An unmatched
run stays text. Formatting can span a soft line break inside one paragraph,
but cannot continue across a block boundary.
