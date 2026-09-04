# Remark directive attributes

Status: normative target profile contract for attaching shared anchors and
attributes to Remark-family directives. The shared
[anchor](../anchors.md) and [attributes](../attributes.md) contracts own the
universal consumer fields, source grammar, normalization, and merge operation;
this module owns only directive attachment, scope, and fallback.

Authority for the directive envelope and attachment position is
`remark-directive@4.0.0` and its micromark dependency surface, as pinned by the
[Remark extension index](../remark.md) and repository oracle. Its different
attribute member tokenizer is deliberately outside that authority boundary.

## Accepted grammar

Directive attribute containers use the shared Pandoc grammar without a profile
override. In particular:

```markdown
:x{#one.two} <!-- anchor="one.two" -->
:x{.one.two} <!-- classes=["one.two"] -->
:x{#one:two} <!-- anchor="one:two" -->
:x{#doc .wide key="value"} <!-- anchor, classes, and records -->
:x{-} <!-- classes=["unnumbered"] -->
```

Dots and colons remain inside a shorthand. Generic bare names are malformed;
`name=` is a valid empty record assignment; and `-` is the special
`unnumbered` class member. These deliberate differences from
`micromark-extension-directive` prevent a second attribute model from entering
through the Remark profile.

## Attachment

An attribute container may occur immediately after the directive name or its
optional label in each directive envelope:

```markdown
:video{id=123 muted=true}
:cite[See _source_]{#smith .paper data-kind=ref}

::youtube[Video]{vid=01ab2cd3efg .featured}

:::spoiler{#outer .warning}
Block content.
:::
```

The owner mapping is:

| Directive source form            | Owner            |
| -------------------------------- | ---------------- |
| inline text directive            | `Directive`      |
| leaf block directive             | `DirectiveBlock` |
| container block directive opener | `DirectiveBlock` |

The normalized values populate the owner's universal `anchor` and `attributes`
fields. ID members populate `anchor`; classes and other assignments populate
`attributes`. `{}` attaches successfully but produces `anchor=null` and
`Attributes.empty`. An ID-only container may produce `Attributes.empty` beside
a non-null anchor. The public AST does not retain a separate `hasAttributes`
bit.

Intervening whitespace ends the suffix position. The attribute container is
not label content and is not part of block content. A successfully attached
container is included in the directive's scope. A quoted value may cross a
source line where the directive scanner already permits that suffix to
continue; the shared grammar admits at most one non-blank line ending between
members. A quoted value may cross multiple non-blank lines and normalizes each
line ending to one ASCII space.

## Fallback and option behavior

The directive envelope commits independently from its optional attribute
suffix. An invalid or unclosed container attaches nothing and remains
available to ordinary parsing; it does not erase an otherwise valid directive
or manufacture a partial `Attributes` value.

With directive recognition disabled, no directive attachment rule runs and
attribute-looking source remains governed by the inherited parser. Source
owned by code, HTML tokens/blocks, or an already completed construct is not a
directive suffix.

Recognition reuses the directive scanner and the one shared Pandoc attribute
parser. It must not rescan a completed node, retain the older Remark attribute
tokenizer as a mode, or normalize attributes in a binding.

## Required conformance cases

Tests must cover inline, leaf, and container directives; suffixes with and
without labels; `{}`; retained dots; numeric ID shorthand; Unicode
letter-started names; `{-}`; rejected bare names; accepted empty assignments;
quoted-only entity decoding; escapes; ordered duplicate classes and records;
immediate versus spaced suffixes; multiline permitted/rejected forms;
malformed and unclosed fallback; exact owner scopes; option-off behavior;
code/HTML ownership; allocation failure; and size-doubling attribute inputs.
