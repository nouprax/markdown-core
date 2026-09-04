# Obsidian Properties oracle corpus

These are successful documented Properties forms, not Markdown Core goldens.
The parity gate recognizes the exact Obsidian envelope, decodes its payload
through the pinned source-preserving YAML document oracle, and parses only the
remaining body through the Obsidian Markdown oracle. The corpus exercises
decoded values, exact scalar sources, mapping-pair order, aliases, and empty
document states. Binding-coordinate scopes, allocation failures, and global
resource limits stay in package-owned fixtures as required by the normative
spec.

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

## Comment-only metadata

```````````````````````````````` example
---
# Maintainer note
---
.
````````````````````````````````

## Source-faithful names, numbers, order, and aliases

```````````````````````````````` example
---
zeta: last
1: &large 9007199254740993
1.0: 1.0
1e2: 1e2
-0: -0
~: tilde
true: boolean-name
"escaped\u0020name": *large
numbers: [9007199254740993, 1.0, 1e2, -0]
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
