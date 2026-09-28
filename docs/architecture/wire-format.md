# MCB2: the native AST wire format

Every binding that cannot hold C node handles receives a parse as one byte
message, **MCB2**, and builds its immutable value tree from it without calling
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

## Primitives

All integers are little-endian and unaligned.

| Wire type | Encoding |
| --- | --- |
| `u8`, `u32`, `i32`, `i64` | fixed width |
| `Bool` | `u8`, 0 or 1 |
| `Int` | `i64` |
| `Double` | IEEE 754 binary64 bits |
| `String` | `u32` byte length, then that many UTF-8 bytes |
| an enum `E` | `u8` index into `enums.E`, from 0 |

## Message

```
"MCB2"  u32 message length  u8 status  body
```

The message length counts every byte of the message, the header included, so
a reader that received only a pointer knows where the message ends. A reader
rejects a message whose length differs from the bytes it holds.

- Status 1 is a parse failure. The body is `u32` error code (the facade's
  `markdown_core_error_code`) and the `String` message, and the message ends.
- Status 0 is a document. The body is a sequence of node records in
  **post-order**, running to the end of the message.

## Values

A value type of the contract encodes structurally from its declaration:

- A value with `fields` writes each field in order.
- A value with `branches` writes a `u8` branch index -- the branch's position
  in the declaration, from 0 -- and then that branch's fields.
- `Scope` is `i32` start line, start column, end line, end column.

A field of type `T` writes `T`. `T?` writes a `u8` presence, 0 or 1, and `T`
when present. `[T]` writes a `u32` count and that many `T`.

## Node records

A record is:

```
u8 kind ordinal   Scope   anchor: String?   attributes: Attributes   fields
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

A field typed with a kind accepts only that kind. `[Markup]` accepts content
kinds only: every kind other than `Document` that no field of the contract
names as its type.

Because children precede parents, a reader never needs recursion or a second
pass, and the encoder never needs to know a subtree's size before writing it.

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
and branch ranges, resource ordinals, and integers that its own model cannot
represent. The semantic invariants of the AST (heading levels, table spans,
list facts) are the parser's to keep and the C suites' to test; a reader does
not re-derive them.
