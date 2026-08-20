# Phase 4 Directive Attribute-List to String-Map JSON Migration

## Outcome

Directive Markdown source now follows the generic directive attribute-list
grammar. Braces contain attributes such as `id=123`, `muted=true`, and
`title="My Video"`; they do not contain JSON source text.

The C API exposes the parsed result as a normalized JSON object string whose
keys and values are strings. For example:

```markdown
:video{id=123 muted=true title="My Video"}
```

produces `{"id":"123","muted":"true","title":"My Video"}`. Numeric- and
boolean-looking source values remain strings.

## Source grammar and normalization

The parser accepts bare attributes and unquoted, single-quoted, or
double-quoted values. A bare attribute has an empty-string value. HTML-style
`#id` and `.class` shortcuts are outside the supported grammar. `id` and
`class` are ordinary keys, and every repeated key uses its last value.

The normalized JSON output is deterministic. Keys retain first-appearance
order while a repeated key updates its value. An explicit `{}` returns
`"{}"`; an absent attribute list returns `NULL`.

These syntax rules do not give attributes HTML behavior. Safe and unsafe HTML
rendering never projects directive members to wrapper attributes. CommonMark
emits normalized directive attribute-list syntax, and XML transport-escapes
the normalized JSON representation.

## C API contract

`markdown_core_extensions_get_directive_attributes` returns `NULL` for a
non-directive node or an absent attribute list. Otherwise it returns the
normalized string-map JSON object owned by the directive node. The pointer
remains valid until attributes are replaced or the node is freed.

`markdown_core_extensions_set_directive_attributes` accepts a complete JSON
object containing JSON string keys and JSON string values. The setter parses
and normalizes the object. Non-string values, nested values, invalid escapes,
and trailing content are rejected. Failure returns `0` and leaves the prior
payload unchanged.

## Scanner safety and complexity

The outer attribute-list scanner is non-recursive and scans quoted and
unquoted content once to find the matching closing brace. Attribute parsing
allocates one bounded record per source member. Duplicate resolution uses the
core byte-key hash index for expected linear normalization. Excessive probe
depth falls back to pointer sorting, bounding adversarial collision behavior
at O(n log n) instead of O(n²).

The size-doubling suite covers long quoted values, consecutive backslashes,
unclosed quoted values, many unique attributes, and many duplicate attributes.
Its scaling thresholds are intended to detect accidental quadratic rescanning
or duplicate handling.

## Test coverage

Directive spec and API tests cover inline, leaf, and container directives;
labels; absent and empty attributes; bare/unquoted/single-quoted/double-quoted
values; numeric- and boolean-looking strings; rejected HTML-style shortcuts;
ordinary id/class keys; duplicate resolution; Unicode; JSON setter normalization; escaped NUL;
transactional failures; malformed and unclosed fallback; non-directive error
paths; HTML non-projection; and CommonMark/XML transport.

The C++ consumer includes the public extension header, parses Markdown
attribute-list syntax, receives normalized JSON, replaces it through the JSON
setter, and verifies the normalized result.

## Validation

The corrected Phase 4 validation set is recorded after the final full run:

- Release CTest: 21/21 passed;
- C/C++ API batch: 630 assertions passed;
- directive fixture/pathological/API subset: 4/4 passed;
- size-doubling suite: 11/11 passed, including many-key cases;
- UBSan CTest: 21/21 passed;
- ASan executable CTest path: 17/17 passed;
- C formatting and warning-as-error lint: passed;
- SwiftPM, Gradle model, install/AAR, and package-content audits: passed;
- full repository `pnpm run verify`: passed;
- `git diff --check`: passed.

In the reviewed local performance run, 64 KiB through 512 KiB long-value cases
ranged from approximately 3.3–5.2 ms to 5.7–23.5 ms. Many-unique-key input
ranged from 3.8–8.5 ms and many-duplicate-key input from 3.8–9.2 ms. All cases
stayed below the adjacent-growth and total-growth thresholds.

On macOS the four Python/ctypes `_library` tests remain excluded from the ASan
run because the platform ASan runtime cannot install interceptors when an
instrumented library is loaded with `dlopen`. They are covered by Release and
UBSan plus executable ASan equivalents.
