---
title: "Parse stage comparison"
lane: staged
pinned: true
target_bytes: 65536
scales: [1, 2]
cases: [lorem1, directive, mixed-extended]
reference: "cmark 0.31.2"
empty: ""
---

The report reads the front matter for the lane name and the pinned
reference, and the body is parsed as ordinary Markdown afterwards.

Every case is measured at both scales, so a shape that grows with the
document is separated from one that grows with a structure.
