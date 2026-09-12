# Links and images

[Syntax guide](../dialect.md) · [Documentation](../README.md)

Put link text in brackets and its destination in parentheses. An optional
quoted title follows the destination.

```markdown
Read [the guide](/guide "Getting started").
```

The result contains `Link` with destination `/guide`, title `Getting started`,
and parsed label text `the guide`. The parser stores the destination without
opening it, resolving it against a base URL, or percent-encoding it. CommonMark
escapes and character references in the destination and title are decoded.

Empty destinations and titles are allowed. A missing title is null; an authored
empty title is the empty string. Angle brackets around the destination allow
forms such as `[guide](<folder/my guide.md>)`.

## Reference links

Give a destination a label and reuse it:

```markdown
[Read more][guide], [guide][], or simply [guide].

[guide]: /guide "Getting started"
```

All three occurrences become ordinary `Link` nodes with the same destination
and title. The definition itself produces no node. Definitions can appear before
or after uses. Labels ignore case and normalize whitespace; the first definition
wins when labels repeat. Each link keeps the scope of its own occurrence.

An undefined reference remains bracket text. A definition is recognized only
where the block grammar permits it. A footnote definition beginning `[^label]:`
is not a link definition. Headings can also supply
[implicit references](anchors.md#implicit-heading-references).

## Automatic links

```markdown
<https://example.com> <reader@example.com>

https://example.com/guide www.example.com reader@example.com
```

These become links. Email destinations gain `mailto:` and bare `www.`
destinations gain `http://`. Bare URL schemes accept `http`, `https`, and `ftp`
case-insensitively. The `www.` form needs an eligible boundary and a domain
containing a dot.

Bare URLs end at whitespace or `<`, with trailing punctuation, entity-shaped
suffixes, and unmatched closing parentheses excluded. Their bodies are opaque:
Markdown-looking bytes inside the URL remain part of it. Bare URL and `www.`
links do not start inside an open link or image label. Inline footnotes have
their own inline body, where these forms can start and end at the footnote's
closing bracket. Email recognition applies to remaining text.

## Images

Add `!` before a link or reference image:

```markdown
![A **bright** sunrise](/sunrise.png "Morning")

![Another view][photo]

[photo]: /sunrise.png
```

Each image becomes `Media`. Its content is parsed alt text, its destination is
a URL value, and its title is optional. The parser does not infer a media type
from the filename or load the resource. Internal `![[...]]` embeds use the
separate [cross-embed syntax](cross-links.md).

## Image dimensions

End the alt label with `|width` or `|widthxheight`:

```markdown
![Sunrise|320](/sunrise.png)

![Sunrise|320x180](/sunrise.png)

![320x180](/sunrise.png)
```

The first image has width 320 and no height. The next two have width 320 and
height 180. Their alt content is `Sunrise`, `Sunrise`, and empty, respectively.
The same rule applies to resolved reference images.

Dimensions are positive decimal integers, at most 2,147,483,647, with no leading
zeros, signs, spaces, or uppercase `X`. Invalid suffixes remain part of the alt
text and leave `dimensions=null`. The last top-level unescaped pipe is the
candidate separator; pipes in code spans or nested brackets do not count.
An empty prefix, as in `![|320](image.png)`, is valid.

## Attributes

Attach a container immediately after a direct link, resolved full/collapsed
reference, or angle autolink:

```markdown
[guide](/guide){.external target=_blank}

![Sunrise|320](/sunrise.png){width=50%}
```

The first link receives a class and a record. The image has typed width 320
and a separate `width="50%"` record; neither overwrites the other. Whitespace
before `{` prevents attachment. Bare automatic links have no attribute suffix.
Reference definitions can supply inherited [attributes](attributes.md).

## Bracket precedence

At a matching `]`, these alternatives are tried in order:

1. A valid direct destination/title tail.
2. A resolving full or collapsed reference tail.
3. An adjacent attribute container, producing a [span](bracketed-spans.md).
4. A valid [citation group](citations.md).
5. A resolving shortcut reference, when no reference tail blocks it.
6. A defined [footnote call](footnotes.md).
7. Ordinary bracket text.

An unresolved full-reference tail does not prevent later alternatives. For an
image opener, alternatives other than a link/image leave a literal `!` before
the resulting node. Complete cross links, inline footnotes, and directive labels
own their brackets under their respective rules.
