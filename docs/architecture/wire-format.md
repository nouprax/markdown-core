# MCB3: the native AST wire format

Every binding that cannot hold C node handles receives a parse as one byte
message, **MCB3**, and builds its immutable value tree from it without calling
back into native code. ES (WebAssembly) and Kotlin (JVM, Android and Native)
use it; Swift walks the C facade directly.

There is one encoder, `packages/markdown-core/wire/`, a pure consumer of
`include/markdown_core.h`, and one message shape. The message is a projection
of the [canonical AST contract](../specs/canonical-ast.json): record layouts
are not listed here because they *are* the contract's kinds, values and
fields, in the contract's order. Adding a field to a kind changes the wire the
same way it changes every binding's model.

## Ownership and lifetime

```c
uint8_t *markdown_core_wire_parse(const uint8_t *source, size_t length);
void markdown_core_wire_free(uint8_t *message);
```

`markdown_core_wire_parse` parses `source` and returns one caller-owned
message. It returns NULL only when not even an error message can be
allocated; every other failure, including a result too large to encode, is an
error message. The message borrows nothing: the document is freed before the
call returns.

```c
uint8_t *markdown_core_wire_session_new(const uint8_t *source, size_t length, markdown_core_text_unit unit,
                                        markdown_core_session **session);
uint8_t *markdown_core_wire_session_edit(markdown_core_session *session, const size_t *edits, size_t count,
                                         const uint8_t *texts);
uint8_t *markdown_core_wire_session_append(markdown_core_session *session, const uint8_t *text, size_t size);
```

A session's steps answer the same way: each returns one caller-owned message
of the document the step published, or of the step's failure. The binding
holds the C session itself and releases it with `markdown_core_session_free`.
An edit batch is `count` triples of sizes in `edits` -- start, end and the
size of the edit's text, as `markdown_core_text_edit` has them -- with the
texts one after another in `texts`, in the order listed. Every message is a
whole document: the binding builds its value tree from it as it does after a
parse.

## Primitives

All integers are little-endian and unaligned.

| Wire type | Encoding |
| --- | --- |
| `u8`, `u32`, `u64`, `i32`, `i64` | fixed width |
| `Bool` | `u8`, 0 or 1 |
| `Int` | `i64` |
| `Double` | IEEE 754 binary64 bits |
| `String` | `u32` byte length, then that many UTF-8 bytes |
| an enum `E` | `u8` index into `enums.E`, from 0 |

## Message

```
"MCB3"  u32 message length  u8 status  body
```

The message length counts every byte of the message, the header included, so
a reader that received only a pointer knows where the message ends.

- Status 1 is a failure. The body is one `u32`, the facade's
  `markdown_core_status` (`ALLOCATION_FAILED` 1, `OUT_OF_BOUNDS` 2,
  `KIND_MISMATCH` 3), and the message ends. A parse fails only with
  `ALLOCATION_FAILED`, which also reports a document too large for the
  message length.
- Status 0 is a document. The body is a sequence of node records in
  **post-order**, ending with the `Document`'s record, followed by the
  document's definition tables.

## Values

A value type of the contract encodes structurally from its declaration:

- A value with `fields` writes each field in order.
- A value with `branches` writes a `u8` branch index -- the branch's position
  in the declaration, from 0 -- and then that branch's fields.
- `Extent` is `i32` lead and `u32` span, in bytes of UTF-8 source.

A field of type `T` writes `T`. `T?` writes a `u8` presence, 0 or 1, and `T`
when present. `[T]` writes a `u32` count and that many `T`.

## Node records

A record is:

```
u8 kind ordinal   u64 id   Extent   anchor: String?   attributes: Attributes   fields
```

followed by the kind's fields in the contract's order. A **node-valued**
field -- one whose type names a kind, or `Markup` -- writes counts instead of
nodes:

| Field type | Written |
| --- | --- |
| `K` | nothing; `K?` writes its `u8` presence |
| `[K]`, `[Markup]` | `u32` count; `[Markup]?` writes a presence and, when present, the count |
| `[[Markup]]` | `u32` collection count, then a `u32` count per collection |

The nodes themselves are the records immediately before this one: every
node-valued field's nodes, field after field in the contract's order, each
field's nodes in stored order, each node's own subtree before it. A reader
keeps a stack of built nodes; a record's counts sum to the number it takes
off the top of that stack, which it hands out to its fields in order before
pushing the node it builds. The message is valid when, at its end, the stack
holds exactly one node and it is the `Document`.

The same holds for a node-valued field inside a value: an inline note's
`Citation` writes its referent's `FootnoteTarget.note` branch index and
nothing for the `Footnote`, which is the first node the record takes, ahead
of its prefix and suffix.

A field typed with a kind accepts only that kind. `[Markup]` accepts content
kinds only: every kind other than `Document` that no field of a kind names as
its type. A field of a value, such as `FootnoteTarget.note`, does not make its
kind a non-content kind, so `Footnote` is content where it is written.

Because children precede parents, a reader never needs recursion or a second
pass, and the encoder never needs to know a subtree's size before writing it.

## Definition tables

After the `Document` record, the message writes the document's footnote
table and then its specimen table. Each is a `u32` count and that many `u64`
ids: every `Footnote`, inline notes included, and every `Specimen` of the
document, in source order. A reader resolves each id to the node it built
with that id; an id that names no node of that kind is invalid.

Scopes are not on the wire. A binding computes them from the extents and the
source, as the facade's scope query does.

## Shared resources

Every occurrence of a reference definition reads its destination, title and
definition attributes through one shared resource (the facade's
`markdown_core_node_resource`). The wire keeps that sharing, so a definition
referenced many times crosses the boundary once and a reader materializes it
once:

- A `Link` or `Embedded` record writes a `u32` **resource ordinal** in place
  of its `dest` field and writes nothing for `title`.
- Resources are numbered from 0 in the order the message first names them.
  An ordinal equal to the number named so far defines the next resource and
  is followed by its `Destination`, its title `String?`, its anchor `String?`
  and its `Attributes`. A smaller ordinal refers to one already defined; a
  larger one is invalid.
- The record's own anchor and attributes are the occurrence's primary
  contribution. The node's anchor is the primary anchor when present and the
  resource's otherwise; its classes and records are the resource's followed
  by the primary ones, as the facade's merge defines.

## What a reader checks

A reader validates the *encoding*: the header, lengths, counts against the
bytes and the stack, kinds against the fields that take them, `Bool` and enum
and branch ranges, resource ordinals, definition table ids, and integers that
its own model cannot represent. The semantic invariants of the AST (heading levels, table spans,
list facts) are the parser's to keep and the C suites' to test; a reader does
not re-derive them.
