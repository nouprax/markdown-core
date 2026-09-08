# Selected metadata syntax oracle corpus

These inputs are within Markdown Core's fixed-field metadata grammar and the
pinned YAML Document/CST oracle's valid intersection. They compare decoded
values, exact numeric spellings, named field assignment and literal prose. They do not
claim complete Obsidian, Pandoc or YAML compatibility. Recovery, ignored input,
allocation failures and binding coordinates are tested by product fixtures.

## Scalar records and body separation

```````````````````````````````` example
---
title: Galactic handbook
subtitle: A guide
state: false
time: 4.5
comment:
---
# Body
.
````````````````````````````````

## Lists and inert internal-link text

```````````````````````````````` example
---
authors:
  - Doggo
  - "[[Canis familiaris]]"
keywords: [1977, 3.14, text]
name: value
---
Body
.
````````````````````````````````

## Text-shaped dates and empty values

```````````````````````````````` example
---
date: 2026-09-03
time: 2026-09-03T12:34:56
state: yes
comment: null
abstract: ""
keywords: []
---
.
````````````````````````````````

## Quoted field names

```````````````````````````````` example
---
"name": value
"state": true
"time": 3
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

## Exact numbers and decoded field names

```````````````````````````````` example
---
name: 9007199254740993
time: 1.0
state: 1e2
date: -0
"tit\u006ce": text
keywords: [9007199254740993, 1.0, 1e2, -0]
---
.
````````````````````````````````

## Only the first block attaches

```````````````````````````````` example
---
name: one
---
---
second: two
---
.
````````````````````````````````

## Literal prose

```````````````````````````````` example
---
abstract: |
  First paragraph.

  Second paragraph.
comment: |
  # literal
  name: inside
state: ready
---
Body
.
````````````````````````````````

## Authors and keywords as single strings

```````````````````````````````` example
---
authors: Ada
keywords: language
---
.
````````````````````````````````
