# Remark directive attributes

Status: normative target profile contract for attaching shared attributes to
Remark-family directives. The [shared attributes contract](../attributes.md)
owns the universal field, grammar, values, normalization, and merge operation;
this module owns only directive source acceptance, attachment, scope, and
fallback.

Authority: `remark-directive@4.0.0` and its micromark dependency surface, as
pinned by the [Remark extension index](../remark.md) and repository oracle.

## Accepted grammar

Directive attribute containers accept every production in the shared grammar:
ID and class shorthand, assignments, and bare names. A bare name has an empty
string value. There is no Pandoc-specific `{-}` rewrite; `-` is the ordinary
attribute name `-` with an empty value.

## Attachment

An attribute container may occur immediately after the directive name or its
optional label in each directive envelope:

```markdown
:video{id=123 muted=true}
:cite[See *source*]{#smith .paper data-kind=ref}

::youtube[Video]{vid=01ab2cd3efg .featured}

:::spoiler{#outer .warning}
Block content.
:::
```

The owner mapping is:

| Directive source form | Owner |
| --- | --- |
| inline text directive | `Directive` |
| leaf block directive | `DirectiveBlock` |
| container block directive opener | `DirectiveBlock` |

The normalized values populate the owner's universal attributes array. `{}`
attaches successfully but leaves that array empty. The public AST does not
retain a separate `hasAttributes` bit.

Intervening whitespace ends the suffix position. The attribute container is
not label content and is not part of block content. A successfully attached
container is included in the directive's scope. A quoted value may cross a
source line where the directive scanner already permits that suffix to
continue; an unquoted line ending remains a shared separator.

## Fallback and option behavior

The directive envelope commits independently from its optional attribute
suffix. An invalid or unclosed container attaches nothing and remains
available to ordinary parsing; it does not erase an otherwise valid directive
or manufacture a partial attribute array.

With directive recognition disabled, no directive attachment rule runs and
attribute-looking source remains governed by the inherited parser. Source
owned by code, HTML tokens/blocks, or an already completed construct is not a
directive suffix.

Recognition reuses the directive scanner and the shared single-pass attribute
parser. It must not rescan a completed node or normalize attributes in a
binding.

## Required conformance cases

Tests must cover inline, leaf, and container directives; suffixes with and
without labels; `{}`; shorthand, assignments, bare names, Unicode, quoting,
escapes, entities, duplicates, and classes; immediate versus spaced suffixes;
multiline permitted/rejected forms; malformed and unclosed fallback; exact
owner scopes; option-off behavior; code/HTML ownership; allocation failure;
and size-doubling attribute inputs.
