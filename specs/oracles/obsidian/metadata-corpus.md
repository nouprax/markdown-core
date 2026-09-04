# Obsidian Properties oracle corpus

These are successful documented Properties forms, not Markdown Core goldens.
The parity gate extracts the header with the pinned frontmatter oracle, decodes
the value graph with the pinned YAML oracle, and parses only the remaining body
through the Obsidian Markdown oracle. Invalid candidates and exact scopes stay
in package-owned fixtures as required by the normative spec.

## Scalar records and body separation

```````````````````````````````` example
---
title: Galactic handbook
draft: false
rating: 4.5
empty:
---
# Body
.
````````````````````````````````

## Lists, arbitrary names, and inert internal-link text

```````````````````````````````` example
---
aliases:
  - Doggo
  - "[[Canis familiaris]]"
values: [1977, 3.14, text]
arbitrary user key: value
---
Body
.
````````````````````````````````

## Text-shaped dates and empty values

```````````````````````````````` example
---
date: 2026-09-03
when: 2026-09-03T12:34:56
yes-word: yes
nothing: null
empty-text: ""
empty-list: []
---
.
````````````````````````````````

## JSON root mapping

```````````````````````````````` example
---
{"name":"value","enabled":true,"count":3}
---
After
.
````````````````````````````````

## Explicit empty metadata

```````````````````````````````` example
---
---
.
````````````````````````````````

## Only the first block attaches

```````````````````````````````` example
---
first: one
---
---
second: two
---
.
````````````````````````````````
