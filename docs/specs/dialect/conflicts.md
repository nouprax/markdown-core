# Compatibility and precedence

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Markdown Core combines familiar Markdown spellings into one language. It does
not emulate an application's complete parser or renderer. The
[syntax pages](../dialect.md) define the accepted forms; this page highlights
differences and explains overlapping syntax.

## Differences to know

| Topic | Markdown Core behavior |
| --- | --- |
| Options | All supported syntax is always enabled. |
| Single tilde | `~x~` is subscript; only `~~x~~` is strikethrough. |
| Empty superscript | `^^` is a valid empty superscript. |
| Quotes and alerts | Every quote is a `Callout`; custom types and their case are retained. |
| Block IDs | Declare with `#id#`; refer through a cross link with `#^id`. |
| Internal links | Target comes before the pipe, raw label after it. Labels are not parsed as Markdown. |
| Heading links | Automatic anchors are generated, while cross-link targets stay raw; applications match them. |
| Properties | One initial `---` envelope with ten known fields. Unsupported members, including `...`, are ignored. |
| Attributes | One grammar at every site; bare names such as `{disabled}` fail. Ordered duplicate classes/records survive. |
| Colon containers | Named directives and nameless fenced divs share one model and one minimum closing-fence length. |
| Formula brackets | Two authored backslashes open `\\(...\\)` or `\\[...\\]`; a single backslash remains a Markdown escape. |
| Task markers | Exactly one Unicode scalar plus a following separator; the authored marker is retained. |
| Comments | HTML and percent comments remain `Comment` nodes. |
| Raw HTML | Block content is opaque, but text between separate inline tags still parses as Markdown. |
| Ordered lists | The first marker always sets the start; nested ordered lists must start at one. |
| Footnote duplicates | All definitions survive; references select the first normalized ID. |
| Grid tables | Malformed geometry rejects the complete candidate instead of dropping source or emitting a partial grid. |
| Presentation | No smart punctuation, emoji shortcode expansion, code execution, math rendering, resource loading, or citation formatting. |

Properties are not general YAML. Cross links do not resolve vault paths.
Code language labels are descriptive and do not add language-specific parsers.
Those boundaries keep the returned AST about authored Markdown.

## Block recognition

Open container prefixes and literal bodies determine which source is available.
A properties envelope is considered only at the document's beginning. At a
block start, the effective precedence is:

1. Code and HTML forms, including HTML comments.
2. Formula blocks, then percent-comment blocks.
3. Quotes/callouts, list and specimen markers with optional task prefixes.
4. Leaf, named-container, and nameless-container directives.
5. ATX headings, then Setext headings.
6. Table candidates, including captions.
7. Thematic breaks, then footnote and link reference definitions.
8. Definition lists, standalone block identifiers, then paragraphs.

Each form's own complete-prefix, indentation, and paragraph-interruption rules
must succeed. This is not a rule that every marker interrupts every paragraph.
Paragraph suffix identifiers are considered during paragraph finalization.
Within tables, the [table rules](tables.md) own row boundaries and cell text.

For example:

```markdown
Title
-----
```

This is a level-two heading, not a paragraph followed by a thematic break.
Likewise a complete initial properties envelope wins over its opening dash
line's other possible meanings.

## Inline recognition

At a given position, recognized literal tokens own their entire extent. Escapes,
code spans, HTML/autolink tokens, formulas, percent comments, cross links,
inline footnotes, citation keys, specimen references, directives, and character
references participate in that order. At a backslash, the doubled-backslash
formula openers are checked before ordinary escaping.

Brackets use the [ordered bracket alternatives](links-and-images.md#bracket-precedence).
Emphasis, strikethrough, subscript, superscript, highlights, and insertions then
match their eligible delimiter runs. Attribute suffixes attach at their owner's
specified site. Remaining text is eligible for email autolinking; an address
recognized at a colon takes precedence over a competing directive name.

```markdown
[topic]{.label}

[topic]: /target
```

This makes a span, because adjacent attributes beat a shortcut reference.
`[topic](/target){.label}` makes a link with attributes instead. A valid direct
link or resolving full/collapsed reference has earlier precedence.

## Literal content and incomplete syntax

Code, formulas, comments, HTML tokens/blocks, complete cross links, and automatic
URL tokens do not recognize nested Markdown in their literal fields. This is
local ownership: two separate tags, comments, or cross links do not shield the
text between them.

```markdown
`==literal==` and <span>==highlighted==</span>
```

The code retains its equals signs. The text between the HTML tags becomes a
highlight. An incomplete outer construct releases its source to the next
alternative; valid inner syntax can still be recognized. An invalid citation
group, for example, may retain ordinary author-in-text citations inside its
brackets.

See [syntax conformance](../../architecture/syntax-conformance.md) for pinned
comparison tools and registered differences. Historical product decisions and
implementation sequencing remain in [plans](../../plans); they are not
additional active grammar rules.
