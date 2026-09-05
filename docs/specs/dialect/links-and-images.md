# Links and images

Status: normative module of the [Markdown Core dialect](../dialect.md). It
owns the shared `Destination` value, the consumer model of every link and
image form, the resolved reference model, the ordered bracket procedure that
every bracket-closing module participates in, GFM bare autolinks, and the
Obsidian image-dimension suffix. Options: `autolinks` (default `true`) and
`imageDimensions` (default `false`). Sources: CommonMark links, images, and
reference definitions; cmark-gfm's autolink extension; Obsidian's external
image dimensions. Executable oracles: cmark and cmark-gfm; image dimensions
are product fixtures. Landing: `Destination` with `M1`, resolved references
with `M2`, dimensions with `O9`.

## Model

```text
Destination =
  url(String)
  | cross(path: String, anchor: String?)

Link(dest: Destination, title: String?, content: [Markup])
Image(dest: Destination, title: String?, width: Int?, height: Int?,
      content: [Markup])
```

`Destination` is a tagged value, not a node: it has no scope, children,
anchor, or attributes, and branch fields exist only in their branch. Every
`Link` and `Image` owns the `url` branch; every `CrossLink` of the
[cross links](cross-links.md) module owns the `cross` branch. `Link.content`
is the parsed label content and `Image.content` the parsed alt content.
`Int` is a 32-bit signed integer on every surface.

`url` holds the complete semantic destination produced by the inherited
grammar: the bytes between angle brackets or the bare destination, with
CommonMark backslash escapes and character references decoded and no
percent-encoding, normalization, or resolution. It may be empty: `[a]()` and
`[a](<>)` produce `url("")`. `title` is the decoded title, or `null` when none
was written; absent and empty titles remain distinct. An email autolink's
destination carries the inherited `mailto:` prefix.

The parser does not fetch a URL, open a file, test existence, or infer a media
type; no such result is a field or a branch.

## Resolved references

Every successful link form produces the same node:

```text
[text](url "title")   \
[text][label]          |
[text][]               |--> Link(dest=url(url), title, content, scope)
[label]                |
<autolink>            /
```

and every successful direct or reference image produces `Image`. A reference
resolves through the parser-owned reference map: one lookup of the normalized
label per candidate, the first definition in source order winning among
duplicates, and the inherited definition grammar deciding what is a
definition. The resolved occurrence takes the definition's destination and
title, keeps the content authored at the occurrence, and keeps the scope of
its own occurrence; the definition's range is never copied, unioned, or
substituted. Two occurrences resolved through one definition share the
definition's resource internally so that a long destination or title is
stored once, and they have no shared consumer identity.

A definition is parser state and produces no node; an unreferenced definition
produces nothing. A reference whose label resolves to no definition, and
bracket text that satisfies no form, is the inherited literal text with its
brackets. The public AST therefore has no `LinkReference`, `ImageReference`,
`ReferenceDefinition`, or `ReferenceForm` once `M2` lands, and no reference
is modeled as a `Citation` or through a document link registry.

## The bracket procedure

At every unescaped `]` that matches an active bracket opener, the inline
parser tests these alternatives in order and takes the first success. A failed
alternative leaves the cursor at the `]`; the container after a failed
alternative is text.

1. Under `footnotes`, a `[^label]` whose label is defined is a footnote call
   and produces a `Cite`, whatever follows the `]`; the
   [footnotes](footnotes.md) module states it.
2. A valid direct tail `(...)` produces `Link` or `Image`; a following
   container attaches under `linkAttributes`.
3. A full `[label]` or collapsed `[]` tail whose label resolves, explicitly or
   through a virtual heading definition, produces `Link` or `Image`; a
   following container attaches under `linkAttributes`. A tail whose label
   does not resolve does not block the later alternatives.
4. Under `bracketedSpans`, a valid attribute container beginning at the byte
   after `]` produces a `Span`.
5. Under `citations`, a valid cite group produces a `Cite`.
6. A shortcut reference whose label resolves, not followed by `[]` or by a
   link label, produces `Link` or `Image`; a following container attaches
   under `linkAttributes` only while `bracketedSpans` is off.
7. Otherwise the pair is the inherited literal text.

For an image opener `![`, alternatives 4 through 6 yield a literal `!`
followed by the node. A `[[` under `crossLinks` is claimed by the cross-link
scanner before this procedure runs, and a text directive's label is claimed by
the directive scanner; neither reaches this procedure.

## Autolinks

Angle-bracket autolinks `<https://example.com>` and `<user@example.com>` are
inherited and always recognized, at inline step A3. With `autolinks=true`,
GFM bare autolinks (`https://`, `http://`, `www.`, and email forms) are
recognized last, at step E, as a post-pass over `Text` nodes only, with
cmark-gfm's start and termination rules: a candidate never extends into a
`Comment`, `CrossLink`, `Code`, `HTML`, `Formula`, or any other node, so a
URL ends at a `Mark` or emphasis boundary, and `www.x.com/[[y]]` with
`crossLinks` on is an autolink ending before the cross link. A bare autolink
never accepts an attribute container, and its inherited termination rule
applies to the braces. Both forms produce `Link(dest=url(...), title=null)`
with the link text as content. With the option off, bare URLs are text.

## Image dimensions

With `imageDimensions=true`, an image whose alt label ends with one of these
complete suffixes receives typed dimensions:

```text
W
WxH
alt|W
alt|WxH
```

`W` and `H` are ASCII digit strings with no leading zero and values from 1 to
2147483647, `x` is lowercase, and no whitespace surrounds `x` or `|`. The
suffix is matched against the raw source bytes between the last top-level
unescaped `|` that is not inside a code span or nested brackets and the
closing `]`; for a label with no such pipe, against the whole label. For a
numeric-only label the alt content is empty; for a pipe form the bytes before
the pipe are the alt content, parsed by the inline parser, and may be empty.
`width` is `W`; `height` is `H` or `null` for a width-only form. The rule
applies to direct and resolved reference images alike.

Zero, a leading zero, a value above the limit, signs, whitespace, missing
components, or non-decimal components produce no dimensions, and the whole
label is alt content. With the option off, every alt label is inherited alt
content byte for byte. A `width` or `height` attribute record under
`linkAttributes` is independent: it never populates the typed fields, and the
typed fields never produce a record. Internal image embeds are `CrossLink`
values whose `label` stays raw; this rule does not apply to them.

## Scopes

`Link.scope` and `Image.scope` cover the opener, the content, the tail, and
an occurrence-local attribute container. An autolink's scope covers the angle
brackets or the bare URL. Content and alt child scopes end before a dimension
suffix, and the suffix is inside the image's scope.

## Required conformance cases

Tests cover empty, absolute, relative, and fragment-only destinations; angle
brackets, escapes, and character references in destinations and titles;
absent versus empty titles; every link and image form with and without a
definition, unused and duplicate definitions, and the identical dump of a
direct and a resolved occurrence apart from scope; the shared-resource bound
for a long destination or title referenced many times, on every surface;
`mailto:` on email autolinks; every step of the bracket procedure with each
participating option on and off; bare autolinks ending at every node boundary
and rejecting a container; every valid and invalid dimension form, formatted
alt content, pipes inside code spans and brackets, reference images, the
limit, and coexistence with dimension records; exact scopes; option-off
output; allocation failure; and size-doubling brackets, parentheses, URLs,
and digit runs.
