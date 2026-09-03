# Remark syntax extensions

Status: normative target index for syntax adopted from the Remark/micromark
extension family. This is an extension family, not a replacement Markdown
dialect. Inherited CommonMark and GFM behavior remains owned by their existing
contracts.

The repository pins `remark-directive@4.0.0` through its lockfile and records
comparison policy in the
[Remark/micromark oracle](../../specs/oracles/remark/README.md). The upstream
[remark-directive project](https://github.com/remarkjs/remark-directive) owns
the directive envelope syntax; registered repository deltas remain explicit.

Shared values live outside this profile:

- [Anchors](anchors.md) owns the universal `Markup.anchor` field.
- [Destinations](destinations.md) owns the tagged target value used by ordinary
  inherited `Link` nodes.
- [Attributes](attributes.md) owns the universal `Markup.attributes` field,
  shared Pandoc-derived grammar, classes-and-records consumer shape,
  normalization, and merge operation. This profile contributes only directive
  attachment positions.

## Modules

| Extension                      | Normative module                          |
| ------------------------------ | ----------------------------------------- |
| directive attribute attachment | [Remark attributes](remark/attributes.md) |
