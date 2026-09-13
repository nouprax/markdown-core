# Markdown Core Requirements

## 1. Document Status

This document contains implementation requirements and handoff information for new sessions working
on `Markdown Core`.

Settled product, architecture, and naming decisions use “must” or direct statements. Any new unresolved
question discovered during implementation must be recorded explicitly and confirmed before the
corresponding implementation begins. Do not expand scope implicitly.

## 2. Project Position

`Markdown Core` is cross-platform Markdown parsing infrastructure.

It permanently maintains the following together in one monorepo:

- A C Markdown parser engine.
- A read-only C AST API.
- A Swift immutable AST binding.
- A Kotlin immutable AST binding.
- An ECMAScript/TypeScript immutable AST binding.
- Shared semantic conformance tests across the three platforms.

The project is a unified Markdown AST upstream for downstream renderers and applications, not a
temporary binding repository.

## 3. Project Identity and Origins

### 3.1 New Project Identity

- Repository: `nouprax/markdown-core`.
- Project: `Markdown Core`.
- The project owner controls the GitHub organization `nouprax`.
- The npm organization `nouprax` has been created to own the `@nouprax` scope.
- The project owner controls `nouprax.com`, and DNS ownership verification for the Maven Central
  `com.nouprax` namespace is complete.
- The project has names, package coordinates, public APIs, and a version line independent of `cmark-gfm`.
- New public product names must not continue using `cmark-gfm`.
- New public C APIs must not add symbols named `cmark_*`.
- Before the initial release, no `cmark`, `cmark-gfm`, `CMARK_*`, or `cmark_*` product/implementation
  naming may remain outside legal/historical attribution and Git history.

### 3.2 Origins

Markdown Core's C engine originated from [DongyuZhao/cmark-gfm](https://github.com/DongyuZhao/cmark-gfm).
The following commit was the baseline when this project was established:

```text
711032b2a16cf25c3df75033833eba086b17ca6a
```

That fork had already diverged substantially from the original `cmark-gfm`, with no plan to merge
back. From the establishment of Markdown Core onward:

- The C engine is first-party Markdown Core source, not a vendored third-party dependency.
- Swift, Kotlin, and ES bindings are permanently maintained alongside the C engine in the same repository.
- The project no longer follows `cmark-gfm` naming or versions.
- There is no product plan to upgrade the C dependency from a master commit to an upstream release tag.

### 3.3 History and Licensing

- Preserve all applicable copyright and license notices from cmark/cmark-gfm in full. Renaming or
  rewriting does not justify removing them.
- The root `LICENSE` and any necessary `THIRD_PARTY_NOTICES`/`COPYING` must contain applicable license
  text and attribution.
- `README.md` and `UPSTREAM.md` must explicitly state that Markdown Core derives from cmark/cmark-gfm,
  has rewritten parts of the inherited code, and is now an independent project with no plan to merge upstream.
- `UPSTREAM.md` must record the fork source, baseline commit, divergence, major rewrites/deletions,
  and inherited licenses.
- Original Git commit history may be retained.
- Establish an independent release lineage. Old SemVer tags from the original project must not
  participate in version resolution for the new SwiftPM package.

### 3.4 Migrating Legacy Internal Names

To reduce regression risk from mechanical renaming across mature C code, perform renaming as a
separate phase, without parser semantic changes. Complete the full rebrand before the initial release:

- Retain no `cmark_*` functions, types, globals, or internal symbols.
- Retain no `CMARK_*` macros, enum constants, or include guards.
- Retain no `cmark`/`cmark-gfm` names in source files, headers, directories, CMake targets, library
  names, pkg-config files, CLI binaries, tests, scripts, or package metadata.
- Retain no old branding in runtime errors, help text, or generated artifacts.
- New public and internal C names must use `markdown_core_*` / `MARKDOWN_CORE_*` or another documented
  Markdown Core internal prefix.
- Renderer code may remain temporarily only until AST dump and test migration are complete. Remove
  it from sources, builds, CLI, public headers, and exported symbols before the initial release.
- The new project retains no public AST mutation API.

The `cmark` name is permitted only in these historical/legal contexts:

- Original licenses and attribution in `LICENSE`, `COPYING`, or `THIRD_PARTY_NOTICES`.
- Origin and rewrite statements in `README.md` and `UPSTREAM.md`.
- Documents under `docs/` explicitly marked as migration/history.
- Git commit history.

## 4. Project Goals

1. Establish SwiftPM, Kotlin/Gradle, and npm platform packages in one monorepo.
2. All three packages compile and call the C engine from the same commit.
3. Expose semantically consistent AST types, fields, and tree structures across all three platforms.
4. Provide `Document.parse` as the only public parsing entry point on each platform.
5. Expose only immutable ASTs, with no AST mutation API.
6. Establish a unified cross-platform AST conformance test suite.
7. Establish unified versioning and coordinated releases.
8. Refer to `/Users/donz/Repos/GitHub/code-block` for multi-package management, root commands, and
   toolchain setup.
9. Clean up the C engine's historical directories while establishing the new project, with clear
   ownership of core, extensions, tests, and platform packages.
10. Remove all private Microsoft/MS Markdown extensions and their options, tests, and generated code.
11. Provide native C AST dumps and migrate parser/spec expectations from rendered output to AST dumps.
12. After test migration, remove all HTML, XML, CommonMark, LaTeX, man, and plaintext rendering capabilities.
13. Complete repository-wide rebranding, retaining cmark names only for necessary legal/historical attribution.

## 5. Non-goals

The new platform packages do not include:

- Public Markdown-to-HTML conversion APIs.
- Public conversion APIs from Markdown to other text, view, or rendering formats.
- HTML, XML, CommonMark, LaTeX, man, or plaintext renderer implementations.
- UI renderers, SwiftUI/Compose/React components, or styling systems.
- AST editing, insertion, deletion, replacement, or reordering APIs.
- AST serialization to JSON as the production protocol between C and platform bindings.
- Three independently implemented parsers.
- Continued source-level or release-level synchronization with the original `cmark-gfm`.
- Compatibility aliases for the old `cmark_*` C ABI, library/CLI names, or package coordinates.

AST dumps are public diagnostic tree representations and test expectation contracts. They are not
Markdown content renderers, persistence formats, or production C-to-binding transport protocols.

## 6. Distribution Naming

### 6.1 Naming Family

The three platform packages use symmetric names with ecosystem prefixes:

| Ecosystem | Package / artifact name | Public module/package |
| --- | --- | --- |
| SwiftPM | manifest package `swift-markdown-core` | product/module `MarkdownCore` |
| Kotlin/Maven Central | `com.nouprax:kotlin-markdown-core` | `com.nouprax.markdown.core` |
| npm | `@nouprax/es-markdown-core` | ESM exports |
| C | `markdown-core` | `markdown_core_*` |

Ecosystem prefixes belong to distribution identity, not AST type names. For example, Swift uses
`Document`, not `SwiftDocument`.

### 6.2 SwiftPM Naming and Organization Boundaries

- SwiftPM does not use npm/Maven-style organization scopes.
- The package URL `https://github.com/nouprax/markdown-core` expresses organization identity.
- The root `Package.swift` manifest package name is `swift-markdown-core`.
- When consumers reference the repository URL, SwiftPM derives the identity `markdown-core` from
  the repository basename. Use `markdown-core` wherever an explicit dependency package identity is needed.
- The public product and importable module are both `MarkdownCore`.
- Do not use `NoupraxMarkdownCore`, `NoupraxSwiftMarkdownCore`, or `nouprax-swift-markdown-core`.
  `nouprax` does not appear in Swift AST type names.

### 6.3 Maven Central Naming and Ownership

- The Maven Central namespace/groupId is `com.nouprax`.
- Ownership of `com.nouprax` has been verified through `nouprax.com` DNS, independently of the GitHub
  organization name.
- The Maven artifactId is `kotlin-markdown-core`.
- The KMP umbrella/root publication coordinate is `com.nouprax:kotlin-markdown-core:<version>`.
  The same release includes target-specific publications generated by the Kotlin Multiplatform plugin.
- The Kotlin package namespace is `com.nouprax.markdown.core`.
- Do not use `io.github.nouprax` or the original personal namespace.

### 6.4 npm Naming and Ownership

- The npm organization/scope is `nouprax` / `@nouprax`.
- The npm organization exists. GitHub organization ownership does not substitute for npm scope ownership.
- The full npm package name is `@nouprax/es-markdown-core`.
- The first public release must be a public scoped package.

### 6.5 Meaning of ES

The `es` in `es-markdown-core` denotes ECMAScript runtime artifacts:

- The package is usable by both JavaScript and TypeScript consumers.
- Runtime delivery consists of JavaScript/ESM and WebAssembly.
- The package must include TypeScript declarations.
- Do not use `ts-markdown-core`: TypeScript is a type/source layer, not a separate runtime platform.

### 6.6 Usage Examples

Swift:

```swift
import MarkdownCore

let document = try Document.parse(markdown)
```

Kotlin:

```kotlin
import com.nouprax.markdown.core.Document

val document = Document.parse(markdown)
```

ES/TypeScript:

```typescript
import { Document } from "@nouprax/es-markdown-core";

const document = Document.parse(markdown);
```

## 7. Versioning and Releases

### 7.1 New Version Line

- Markdown Core uses its own SemVer version line.
- New versions do not inherit the meaning of `cmark-gfm` versions.
- The npm `1.0.0` bootstrap established the new release lineage. Protected `v1.0.1` failed closed before
  artifact building and was not published; the first coordinated release across all four platforms is `1.0.2`.
- The new repository does not migrate, retain, or archive old `cmark-gfm` tags.
- Its tag namespace contains only Markdown Core release attempts. Failed `v1.0.1` remains immutable;
  the first coordinated version is `v1.0.2`.

### 7.2 Coordinated Releases

The C engine, C AST API, and three platform packages must be verified at the same repository commit.

The default policy uses one project version:

```text
Markdown Core X.Y.Z
├── C engine / facade source X.Y.Z
├── swift-markdown-core X.Y.Z
├── kotlin-markdown-core X.Y.Z
└── @nouprax/es-markdown-core X.Y.Z
```

Parsing behavior, AST contract, or C facade changes must be verified across all three bindings before release.

### 7.3 Publishing Authentication and Secrets

All public publishing must run through the protected `release` environment in GitHub Actions:

- Only protected release tags or confirmed manual workflows may trigger it.
- Publishing jobs run only after every build/test/conformance/consumer check passes.
- The `release` environment should require a reviewer and prevent self-review when available.
- Registry credentials and signing private keys may be stored only as GitHub environment secrets,
  never in the repository, plaintext workflows, Gradle properties, `.npmrc`, shell history, or build artifacts.
- PRs, fork builds, and ordinary CI jobs must not obtain release secrets.
- Workflows use minimum `permissions`, explicitly disabling unneeded GitHub token permissions.
- Never log tokens, private keys, passphrases, or complete authentication commands.
- `docs/releasing.md` must document credential creation, GitHub secret names, purpose, permissions,
  expiry, rotation, revocation, and compromise response, without recording secret values.

#### 7.3.1 npm

- Prefer npm Trusted Publisher and GitHub Actions OIDC for `@nouprax/es-markdown-core`; do not retain
  long-lived npm write tokens in GitHub.
- Bind the trusted publisher to organization `nouprax`, repository `markdown-core`, the exact workflow
  filename, and the `release` environment.
- The npm publishing job must use a GitHub-hosted runner with only `contents: read` and OIDC's
  required `id-token: write`.
- Use Node/npm versions satisfying trusted-publishing requirements. `repository.url` in `package.json`
  must point exactly to `nouprax/markdown-core`.
- Retain npm provenance attestations when publishing through OIDC.
- If trusted publishing cannot be configured before the bootstrap publish, allow only a short-lived,
  minimally scoped granular token or a manual first publish with 2FA. Revoke bootstrap tokens
  immediately after trusted publishing is verified.
- Once trusted publishing is stable, the npm package should require 2FA and prohibit traditional
  token publishing.

#### 7.3.2 Maven Central

- Use expiring user tokens generated by Central Publisher Portal, never account passwords.
- Store the Portal token username/password separately as `release` environment secrets:
  `MAVEN_CENTRAL_USERNAME` and `MAVEN_CENTRAL_PASSWORD`.
- Sign Maven artifacts with PGP/GPG. Every artifact requiring a Central signature must have its `.asc` file.
- Store the ASCII-armored private signing key and passphrase separately as `MAVEN_SIGNING_KEY` and
  `MAVEN_SIGNING_PASSWORD`.
- Publish the public signing key to a Maven Central-supported public key server and verify
  retrievability before release.
- Private signing keys need a passphrase, expiration/transition plan, and offline backup. CI secrets
  must not be the only copy.
- Give Portal tokens reasonable expiration dates, rotate them after personnel, workflow, or publishing
  tool changes, and revoke them immediately on compromise.

#### 7.3.3 SwiftPM, C Artifacts, and GitHub Releases

- SwiftPM releases use the repository's SemVer Git tag and need no separate Swift registry token.
- Prefer the workflow-provided `GITHUB_TOKEN` for GitHub Releases, source archives, checksums, and
  required C artifacts.
- Only jobs creating GitHub Releases or uploading artifacts may receive `contents: write`.
  Other jobs retain `contents: read`.
- Do not create long-lived PATs by default. If a GitHub capability gap requires one, use a
  fine-grained, minimally scoped, expiring token and document the reason.

## 8. Overall Architecture

```text
Markdown UTF-8 source
          │
          ▼
 Markdown Core C engine
          │
          ▼
 read-only markdown_core C AST API
       ┌──┼──┐
       ▼  ▼  ▼
    Swift Kotlin ECMAScript/WASM
       │  │  │
       ▼  ▼  ▼
 immutable platform AST
```

The shared C layer is responsible for:

- Parsing Markdown.
- Consistently registering and enabling supported syntax extensions.
- Normalizing core and extension nodes into one read-only C AST API.
- Defining explicit memory ownership and string lifetimes.
- Ensuring identical parser behavior across all three platform bindings.

Platform bindings are responsible for:

- Calling the shared C parse API.
- Building native immutable ASTs from the read-only C AST.
- Freeing the temporary C document after building the platform AST.
- Keeping public ASTs independent of raw pointers or manual freeing.
- Expressing the AST through native type systems without changing semantics.

## 9. C AST API Requirements

### 9.1 API Nature

The C layer must provide direct AST parsing/traversal APIs. It must not pass ASTs to platform bindings
through JSON or other text serialization formats.

New public C headers should expose only:

- Document parsing.
- Document destruction.
- Root-node access.
- Node-kind access.
- Child/sibling traversal.
- Read-only properties for the current node kind.
- Source position/range access.
- Parse failure information.

New public C headers must not expose:

- Node property mutation APIs.
- Append, prepend, insert, unlink, replace, or other mutation APIs.
- HTML or other renderer APIs.
- Internal `cmark_node` implementation details that platform consumers must understand directly.

### 9.2 Conceptual API

The following outlines the requirements. Concrete type and function names are determined during design:

```c
typedef struct markdown_core_document markdown_core_document;
typedef struct markdown_core_node markdown_core_node;
typedef struct markdown_core_error markdown_core_error;
typedef struct markdown_core_parse_options markdown_core_parse_options;
typedef struct markdown_core_scope markdown_core_scope;

typedef enum markdown_core_node_kind markdown_core_node_kind;

markdown_core_document *markdown_core_document_parse(
    const uint8_t *source,
    size_t length,
    const markdown_core_parse_options *options,
    markdown_core_error **error
);

const markdown_core_node *markdown_core_document_root(
    const markdown_core_document *document
);

void markdown_core_document_free(markdown_core_document *document);

markdown_core_node_kind markdown_core_node_get_kind(
    const markdown_core_node *node
);

const markdown_core_node *markdown_core_node_first_child(
    const markdown_core_node *node
);

const markdown_core_node *markdown_core_node_next_sibling(
    const markdown_core_node *node
);

markdown_core_scope markdown_core_node_get_scope(
    const markdown_core_node *node
);
```

The facade must provide an explicit default initializer or constant for `markdown_core_parse_options`.
Consumers must not copy internal parser bit flags or use uninitialized structs for defaults.
The final header must express the start/end positions of `markdown_core_scope` with explicit public
value types.

### 9.3 Ownership

- `Document.parse` passes UTF-8 bytes and an explicit length to C, without relying on NUL termination.
- The C document owns the C AST and string memory created during parsing.
- C string views must be explicit UTF-8 byte views equivalent to
  `{ const uint8_t *data; size_t length; }`, with no NUL-termination guarantee.
- Node handles and borrowed string views must not outlive their owning C document.
- Free the C document after building the platform AST.
- Platform ASTs returned to users must not contain C pointers that users need to free.
- No failure path may leak parser, document, node, string, or error memory.

### 9.4 Error Model

- The C facade uses explicit status/errors, not a sentinel empty document to indicate success.
- Errors include at least an error code stable within the current release and a UTF-8 diagnostic
  message. Input-position-related errors also include `Scope` or `Position`.
- C-allocated errors must be freed with dedicated `markdown_core_error_free`, which explicitly permits null.
- Swift maps failures to typed thrown errors, Kotlin to typed exceptions/errors, and ES synchronously
  throws typed `Error` objects.
- Error codes/messages do not promise cross-release ABI compatibility, but must have consistent
  semantics across the three bindings within a release.

### 9.5 ABI Boundary

- Platform bindings depend only on the new read-only `markdown_core_*` facade.
- Rename or remove the historical `cmark_*` ABI during rebranding. It must not exist in initial release artifacts.
- C node kinds and property accessors cover core and custom extension nodes.
- Public ABI changes and all three binding changes belong in the same commit.
- Markdown Core does not promise C binary ABI compatibility across releases.
- Each release rebuilds the C engine and all bindings from a clean checkout. Mixing new bindings
  with old C binaries is unsupported.
- C API changes do not add compatibility shims, deprecated aliases, or ABI-version bridges. All three
  bindings update together in the same release.

### 9.6 Native AST Dumps

- Before implementing, audit whether the C engine already has a native AST dump API independent of renderers.
- `cmark_render_xml` and CLI `-t xml` do not satisfy this requirement: they belong to the renderer
  system scheduled for removal.
- If no independent AST dump API exists, add one at the `markdown_core_*` C boundary.
- AST dumps primarily detect parser behavior drift. Given identical Markdown, `ParseOptions`, and
  canonical schema, any change to public node types, hierarchy, order, properties, nullability, text,
  or source ranges must produce a visible dump diff.
- Dumps cover every behavior-bearing canonical AST field: core/extension node kinds, original child
  order, all public node properties, and complete `Scope`. Do not omit, collapse, or reorder semantic
  data to stabilize goldens.
- Output properties even when they equal defaults. Optional properties include their field name and
  `null`; absence of a field must not substitute for null.
- Explicitly distinguish `null`, empty strings `""`, empty children (`children=0`), `false`, numeric `0`,
  and nonempty values.
- String escaping, Unicode, numbers, enums, booleans, and newlines must be stable across platforms.
- C tests and the test CLI must call AST dumps without Swift, Kotlin, or JavaScript dependencies.
- Dumps use canonical UTF-8 file-tree text, not HTML/XML renderers or JSON. Each AST node occupies one
  line; `├──`, `└──`, `│   `, and four spaces express true parent/child hierarchy rather than nested
  parentheses or flat node/event lists.
- The root line has no connector. Other nodes use `├──` or `└──` depending on whether they are the last
  sibling. Use `│   ` where an ancestor has later siblings, otherwise four spaces.
- Each node line has a fixed order: concrete node kind, `scope=<range>`, every semantic property
  declared for that kind in the canonical schema, and `children=<count>`. Separate fields with one ASCII space.
- Always emit `children=<count>` as decimal without leading zeros. The number and order of nested
  direct-child lines must match it. `children=0` explicitly denotes a leaf/empty children.
- Preserve original parser child order; never reorder siblings by kind, scope, or properties.
- Property order must not depend on hash/map iteration, registration order, or memory layout.
  Explicitly fix each kind's field order in schema documentation and golden tests.
- Write `scope` as `startLine:startColumn..endLine:endColumn`. Preserve all four integer coordinates
  and their semantics exactly from the native C parser in the same Markdown Core release.
- Every real node has a nonoptional scope. Do not use `null` or inherit parent scope as a fallback.
  The facade, dumps, and all three bindings must not scan source, correct, expand, reject, or
  reinterpret particular coordinate combinations.
- Strings use JSON string escaping rules to express quotes, backslashes, control characters, and
  newlines explicitly. Preserve non-ASCII Unicode directly as UTF-8.
- Enums use fixed lowercase symbolic names from the canonical schema; booleans use only `true`/`false`;
  integers use decimal without leading zeros; optional values use `null`.
- Use the specified Unicode file-tree connectors, LF newlines, and one final newline. Outside string
  contents, emit no trailing whitespace, environment-dependent paths, pointers/addresses, locales,
  platform newlines, or nondeterministic IDs.

Canonical tree example:

```text
Document scope=1:1..2:5 children=2
├── Heading scope=1:1..1:7 level=1 children=1
│   └── Text scope=1:3..1:7 literal="Hello" children=0
└── Paragraph scope=2:1..2:5 children=1
    └── Text scope=2:1..2:5 literal="world" children=0
```

- All semantic properties appear inline as `name=value` between `scope` and `children`. Fields omitted
  from this example must be defined per type in the canonical schema; implementations cannot omit them.
- Property names contain no whitespace. String values are double-quoted, so spaces in their contents
  cannot break field boundaries. The canonical schema must define exact representations for compound
  values, keeping them on one line with no literal newlines inside properties.
- Spec/fixture harnesses manage Markdown source, explicit `ParseOptions`, and the expected tree dump
  together as one case. Options need not appear in the tree, but golden interpretation must not depend
  on process-global defaults.
- Golden comparison is byte-for-byte and provides structured/line-by-line diffs on failure to locate
  drift in a node, field, or scope.
- Targeted fixtures must prove that every node kind, every property's null/empty/non-default states,
  every extension, Unicode, and range boundaries affect the dump. Goldens generated by the current
  parser alone do not prove completeness.
- AST dumps are public diagnostics in all three packages, but their text is not a persistence/transport
  ABI. Without an intentional canonical AST change, dump schema, field order, and normalization rules
  must not drift.
- Intentional parser behavior changes include corresponding golden diffs and explanations in a
  reviewed commit. Intentional dump-schema changes also update schema documentation, implementations,
  completeness tests, and every golden. Do not add compatibility/normalization modes that hide drift,
  or overwrite goldens in bulk without human review.
- C headers explicitly document AST dump memory ownership, errors, and freeing APIs.

## 10. Public AST Model

### 10.1 Model Reference

The AST definition references:

```text
/Users/donz/Repos/GitHub/markdown-renderer/
packages/swift-markdown-render/Sources/SwiftMarkdownRender/Tree
```

Rename the reference model's `MarkdownElement` to `Markup` in this project.

Markdown Core's AST contains only parsing semantics, without `markdown-renderer` rendering or
presentation-specific state.

### 10.2 Naming Rules

- Name the abstract AST node type `Markup`.
- Remove the `MarkdownElement` prefix from concrete types.
- Do not add generic `Element`, `Node`, or `Markup` suffixes to concrete types.
- Use the same conceptual names across all three platforms.
- C facade kinds and native node types use the same conceptual names: base names for non-block nodes
  and the `_BLOCK` suffix for block nodes.
- Provide no inherited cmark compatibility aliases or unnamespaced short-name macros.
- Native extensions register independent node types; retain no generic `CUSTOM` / `CUSTOM_BLOCK` nodes.
- Swift `Code`/`CodeBlock`, `Formula`/`FormulaBlock`, `HTML`/`HTMLBlock`, and
  `Directive`/`DirectiveBlock` each live in separate, matching files. Shared native decoding helpers
  also have their own files.
- Ecosystem distribution prefixes do not enter AST type names.
- Retain concrete `List`; do not rename it to `ListBlock`, `MarkupList`, or another platform-specific
  name for Kotlin.
- Kotlin resolves conflicts with `kotlin.collections.List` using the fully qualified
  `com.nouprax.markdown.core.List` or an import alias. Library implementation code explicitly uses
  `kotlin.collections.List` for collections.

Examples:

| Reference type | Markdown Core type |
| --- | --- |
| `MarkdownElement` | `Markup` |
| `MarkdownDocument` | `Document` |
| `MarkdownElementParagraph` | `Paragraph` |
| `MarkdownElementHeading` | `Heading` |
| `MarkdownElementText` | `Text` |
| `MarkdownElementCodeBlock` | `CodeBlock` |
| `MarkdownElementInlineCode` | `Code` |
| `MarkdownElementLink` | `Link` |
| `MarkdownElementImage` | `Image` |

### 10.3 Node Coverage

The initial AST must cover the semantics of nodes in the reference `MarkdownElement` model:

- `Document`
- `BlockQuote`
- `Paragraph`
- `Heading`
- `ThematicBreak`
- `List` / `ListItem`
- `CodeBlock`
- `HTMLBlock`
- `FormulaBlock`
- `Table` / `TableRow` / `TableCell`
- `DirectiveBlock`
- `FootnoteDefinition`
- `Text`
- `SoftBreak`
- `LineBreak`
- `Code`
- `HTML`
- `Formula`
- `Emphasis`
- `Strong`
- `Strikethrough`
- `Link`
- `Image`
- `Directive`
- `FootnoteReference`

Supporting value types cover the concepts required by the reference model:

- Source positions/ranges.
- List flavor, start, tightness, and task checked state.
- Table alignment.
- Formula mode.
- Directive attributes.
- Link/image destination and title.
- Code-block info/language, literal, and fence-closed state.

`TableRow` and `TableCell` are `Markup` types with complete `Scope`, owned through strongly typed
`Table.header`/`Table.rows` and `TableRow.cells` edges. They participate in Visitor dispatch and
Walker callbacks without requiring public generic `children`. Directive labels do not introduce
synthetic `DirectiveLabel` Markup. `Directive`/`DirectiveBlock` retain labels through typed, optional
`label` Markup collections, distinguishing absent labels from explicitly empty ones. The standard
Walker traverses these typed properties; consumers do not inspect node kinds to discover structure.

Directive attribute source grammar uses generic key-value attribute-list semantics: `{key=value}`,
bare attributes, and single/double-quoted values are supported; HTML-style `#id` and `.class`
shortcuts are not. Every result is a string-to-string mapping. For example,
`{id=123 muted=true title="My Video"}` becomes
`{"id":"123","muted":"true","title":"My Video"}` for consumers, with `true` remaining a string.
For every duplicate key, the last value wins. `id` and `class` are ordinary keys with no special behavior.

The representation across C and platform bindings is an optional normalized JSON string, not source
text or a cross-platform `JSONValue` tree. Objects contain only string keys and string values: no
numbers, booleans, nulls, nested objects, or arrays. Consumers decode the JSON string to a typed string
map. Attribute names, including `id`, `class`, and `data-*`, have no HTML rendering semantics, and the
core must not project them into HTML attributes. C API setters accept the same string-map JSON schema
and return normalized JSON on success; failure leaves the node unchanged.

Directive, formula, and code nodes share `embedded`/`standalone` placement modes. Placement belongs to
the directive/formula/code itself, not its label. Inline and block concrete kinds remain separate to
preserve typed dispatch and valid structures.

### 10.4 Immutability

- Swift AST properties are publicly read-only, preferring value types, `let`, and `Sendable`.
- Kotlin ASTs use sealed types and read-only `val` properties, without leaking internal mutable collections.
- ES/TypeScript AST types recursively use `readonly`, and public APIs provide no mutation entry points.
- ES/TypeScript does not perform `Object.freeze`, recursive freezing, or Proxy-based runtime
  immutability. Immutability is a type/API contract, not JavaScript runtime enforcement.
- No platform exposes setters, mutable children, or native mutation handles.
- No platform requires users to manage C/WASM memory for ASTs manually.

### 10.4.1 Binding Source Ownership

- Split each platform's public model by AST concept. Swift value types read C accessors directly
  through extensions in their corresponding files. Kotlin's serialized bridge and ES's WASM pointer
  boundary each use one exhaustive decoder to construct public models directly; model files do not
  depend on private bridges.
- `Code`/`CodeBlock`, `Formula`/`FormulaBlock`, `HTML`/`HTMLBlock`, and `Directive`/`DirectiveBlock`
  each require separate files. Keep tightly coupled `List`/`ListItem` together in `List`,
  `FootnoteDefinition`/`FootnoteReference` together in `Footnote`, and `Table`/`TableRow`/`TableCell`
  together in `Table`.
- Each platform retains an exhaustive native-kind router so adding a kind causes a compilation or
  test failure. Swift's router only dispatches; Kotlin and ES decoders also serve as the sole schema
  owners for their private boundaries.
- Kotlin's private bridge decodes by kind directly into immutable public models. Do not create
  `Any`, generic `WireNode`, universal field-slot records, or a second intermediate AST. Model files
  must not depend on `WireReader` or interpret bridge layout.
- ES decodes WASM node pointers directly into readonly discriminated-union models. Do not create
  generic wire trees or leak `NativeExports`/`DecodeContext` into models. Stateful decoders reuse
  parse-lifetime scratch memory and strictly validate raw kinds, enums, and booleans. Maintain one
  TypeScript source graph, with runtime JavaScript and recursive readonly declarations generated by
  `tsc`, never parallel handwritten `.js` + `.d.ts` files. The root entry is only a public barrel.

### 10.5 `Scope` and Complete Source Ranges

- Every `Markup` has nonoptional `scope: Scope`.
- `Scope` contains both `start: Position` and `end: Position`, not just the start.
- `Position` contains at least integer `line` and `column`.
- Preserve `Position` values and semantics directly from the native C parser in the same release.
  Public ASTs do not define additional semantics for particular coordinate combinations.
- The C facade and all three bindings copy coordinates directly without scanning source, correcting,
  expanding, rejecting particular combinations, or introducing separate interpretation layers.
- AST dumps output each node's scope, and cross-platform conformance tests compare both start and end.

### 10.6 Visitor/Walker

- Swift, Kotlin, and ES/TypeScript expose visitor and walker APIs.
- Visitors support typed dispatch for every concrete `Markup` type.
- Visitors are exhaustive: every concrete `Markup` handler is required. Do not provide `defaultVisit`,
  optional handlers, or catch-all fallbacks through protocol/default implementations. Adding a kind
  must make unchanged visitors fail compilation.
- Walkers provide standard depth-first traversal and entering/exiting events. Consumers need not
  implement child-access rules for each container type.
- Visitors/walkers read ASTs without replace/remove/mutate callbacks.
- Traversal order is identical across platforms, verified by equivalent focused conformance cases
  in each package.
- All three platforms expose `TreeDumper.dump(markup)` and `Markup.dump()`. Any Markup may be a subtree
  root using the same canonical grammar. Implementations reuse exhaustive Visitors and Walkers,
  without calling C dumps. Text is for diagnostics/logging/snapshots, not persistence or transport.

The complete node/field/nullability/ownership, ParseOptions, Visitor, and Walker contracts frozen in
Phase 5 are in `docs/specs/canonical-ast.md`. Deterministic file-tree dump grammar, field order, and
escaping are in `docs/specs/canonical-ast-dump.md`.

## 11. Parsing API

### 11.1 Single Entry Point

All three platforms expose only this conceptual API:

```text
Document.parse(markdownSource, options = ParseOptions.default)
```

Do not publicly expose:

- A second parser object.
- Synonymous parsing entry points.
- HTML/rendering APIs.
- C parser/node handles.

### 11.2 Input and Output

- Input is a native platform string.
- Bindings pass input to C as standard UTF-8.
- Successful parsing returns an immutable `Document`.
- All three platforms produce semantically equivalent ASTs from identical input.
- Parse or binding-construction failures report errors explicitly, never silently returning empty documents.

### 11.3 Parse Configuration

- Consumers may pass immutable `ParseOptions` on each `Document.parse` call.
- Fields, defaults, and semantics match across all three platforms; no platform-specific parser options.
- Bindings only map typed `ParseOptions` to the C facade. C implements actual parser flags and
  extension attachment centrally.
- `Document.parse(source)` equals `Document.parse(source, ParseOptions.default)` without creating a
  second parse entry point.
- Since `scope` is required on every `Markup`, source-position tracking is always enabled and cannot
  be disabled by consumers.

`ParseOptions.default` enables all general AST syntax supported by Markdown Core:

- Smart punctuation.
- Footnotes.
- Strip HTML comments.
- Tables.
- Strikethrough.
- Autolinks.
- Task lists.
- Formula extension.
- Dollar formula delimiters.
- LaTeX formula delimiters.
- Directive extension.

Consumers may enable or disable these AST-affecting capabilities individually through `ParseOptions`.
Every option combination must produce the same AST across all three platforms.

The following historical options do not enter the new `ParseOptions`:

- `unsafe`: affected only the old renderer. ASTs always retain parsed raw HTML, URLs, and other raw
  data; downstream consumers/renderers decide safety policy.
- `github-pre-lang`: affected only old HTML `<pre>`/`<code>` rendering.
- `full-info-string`: affected only the old HTML renderer. `CodeBlock` always retains the complete
  raw info string without an option toggle.
- Any removed renderer-specific option.

### 11.4 Synchronous Parsing and Scheduling Responsibility

- Swift, Kotlin, and ES/TypeScript `Document.parse` APIs are synchronous.
- ES/TypeScript returns `Document`, not `Promise<Document>`.
- Initialize WASM before `Document.parse` can be called, using ESM initialization/top-level await,
  embedded WASM, or another approach that does not add a second public parser API.
- Consumers decide whether to parse in async tasks, background queues, coroutines, or Web Workers.
- Markdown Core exposes no `parseAsync`, callback parsing, or internal scheduler.

### 11.5 Private MS Syntax to Remove

Markdown Core supports no private Microsoft/MS Copilot Markdown syntax. Removal includes at least:

- `ms_copilot_accordion`
- `ms_copilot_annotation`
- `ms_copilot_citation`
- `CMARK_OPT_MS_COPILOT_ACCORDION`
- `CMARK_OPT_MS_COPILOT_ANNOTATION`
- `CMARK_OPT_MS_COPILOT_CITATION`
- `CMARK_OPT_MS_FORMULA_DELIMITERS`
- MS single-backslash formula delimiter scanners and formula-parser branches.
- Related node kinds, accessors, setters, registration, CLI flags, headers, CMake/Make inputs,
  generated scanners, tests, fixtures, and documentation.

Removal does not mean disabled by default: remove the code and public symbols completely. Then run
a case-insensitive source audit to confirm no MS-specific parser surface remains.

## 12. Platform Package Requirements

### 12.1 `swift-markdown-core`

- Build and distribute through Swift Package Manager.
- SwiftPM source URL: `https://github.com/nouprax/markdown-core`.
- Manifest package name: `swift-markdown-core`; repository-derived package identity: `markdown-core`.
- Swift product/module: `MarkdownCore`.
- SwiftPM artifacts and modules have no `Nouprax` prefix.
- The Swift target depends on the shared C target in the same repository.
- Public APIs expose only Swift ASTs and `Document.parse`.
- Consumers need not import the C target directly.
- An independent consumer package test verifies actual release import behavior.
- Apple support covers only the latest two officially released iOS and macOS major generations.
- Initial `1.0.2` deployment minimums are iOS 18 and macOS 15, covering iOS 18/26 and macOS 15/26.
- Each year, after new major OS releases and CI/toolchain availability, raise minimum deployment
  targets to retain the latest two generations. Do not maintain compatibility branches for older OSes.

### 12.2 `kotlin-markdown-core`

- Publish through Maven Central.
- Namespace/groupId: `com.nouprax`, verified through `nouprax.com` DNS ownership.
- Maven base artifact: `kotlin-markdown-core`.
- KMP umbrella/root coordinate: `com.nouprax:kotlin-markdown-core:<version>`.
- Kotlin package namespace: `com.nouprax.markdown.core`.
- Deliver a Kotlin Multiplatform library, not an Android-only AAR.
- Gradle is the sole build, test, variant-modeling, and publication-orchestration system for the
  Kotlin/KMP module. Maven Central hosts Maven-format publications and does not replace Gradle builds.
- Each release publishes coordinated KMP root metadata, JVM JAR, Android AAR, and declared
  Kotlin/Native target artifacts. AAR serves Android and JAR serves JVM; neither alone is a universal package.
- The Kotlin Multiplatform Gradle plugin generates root and target-specific publications. Keep its
  coordinate conventions, such as `com.nouprax:kotlin-markdown-core-jvm:<version>` for JVM. Freeze and
  document final Android/Native suffixes in Phase 12 through a `publishToMavenLocal` artifact audit,
  without handcrafting coordinates inconsistent with plugin metadata.
- Gradle/KMP consumers depend on the root coordinate and let Gradle Module Metadata select variants.
  Do not remove or damage `.module` metadata. Also run a real Maven consumer through the
  repository-owned Maven Wrapper to verify the JVM target coordinate, effective model, dependency
  resolution, and lifecycle directly through Maven POM dependencies.
- All root/target publications share the group, base artifact identity, version, license, SCM, and
  release provenance, and appear together in one Central deployment. Do not publish root metadata
  while omitting referenced target artifacts.
- Expose AST types, `ParseOptions`, `Document.parse`, Visitor, and Walker in `commonMain`.
- The first release includes Android and general JVM targets. JVM artifacts package loadable native
  libraries for supported desktop OS/architecture combinations.
- Kotlin/Native targets are part of KMP delivery and call the same C facade through cinterop. Freeze
  the exact target/architecture matrix in the implementation plan together with CI.
- Kotlin/JS and Kotlin/Wasm are not required in the first Kotlin release. `@nouprax/es-markdown-core`
  covers Web/Node delivery.
- Public APIs expose only Kotlin ASTs and `Document.parse`.
- Native C interaction details do not enter public APIs.
- The Kotlin-to-C encoding path preserves standard UTF-8.
- Android/JNI must not treat JNI modified UTF-8 as the UTF-8 representation of Markdown source.
- Independent consumers verify published artifacts and native library packaging.
- Consumer tests cover at least KMP Gradle, JVM Gradle with Gradle Module Metadata, real JVM Maven
  through the repository-owned Maven Wrapper, and Android Gradle/AAR. Verify native OS/architecture/ABI
  variant selection.
- Android AARs carry supported native libraries under `jni/<abi>/` or an equivalent standard location.
  JVM publications provide stable OS/architecture runtime selection, either through platform runtime
  artifacts with Gradle attributes or by bundling all supported natives when size evidence justifies
  it. Verify the final design through both Gradle Module Metadata and real Maven resolution/lifecycle,
  including explicit errors for unsupported platforms. Maven consumers use a version- and
  distribution-checksum-pinned Wrapper, without requiring a global `mvn` installation.

### 12.3 `@nouprax/es-markdown-core`

- Publish through npm.
- Publish under the `@nouprax` scope owned by the established npm organization `nouprax`.
- Deliver ESM JavaScript, TypeScript declarations, and executable WebAssembly.
- Execute the C engine and read-only C facade through WebAssembly.
- Public APIs expose only ES/TypeScript ASTs and `Document.parse`.
- `Document.parse` synchronously returns `Document`, not `Promise<Document>`; consumers own concurrency
  and worker scheduling.
- TypeScript ASTs use recursive `readonly` without `Object.freeze` or other runtime freezing.
- Do not expose WASM pointers, the Emscripten runtime, or internal initialization objects.
- Do not transfer ASTs between WASM and JavaScript through JSON.
- Consumer tests verify exports, types, Node, and target browser environments.

## 13. Monorepo Setup

### 13.1 Target Layout

The new repository places the C engine and all three platform packages under `packages/`:

```text
markdown-core/
├── .editorconfig
├── .gitattributes
├── .gitignore
├── .clang-format
├── .swift-format
├── .swiftlint.yml
├── CMakeLists.txt
├── Package.swift
├── eslint.config.js
├── package.json
├── pnpm-workspace.yaml
├── prettier.config.mjs
├── .prettierignore
├── settings.gradle.kts
├── build.gradle.kts
├── gradle/
│
├── packages/
│   ├── markdown-core/
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   └── markdown_core.h
│   │   ├── core/
│   │   ├── extensions/
│   │   ├── tests/
│   │   │   ├── core/
│   │   │   ├── extensions/
│   │   │   ├── api/
│   │   │   ├── fixtures/
│   │   │   ├── canonical-ast/
│   │   │   ├── corpora/
│   │   │   └── runners/
│   │   ├── benchmarks/
│   │   └── fuzz/
│   ├── swift-markdown-core/
│   │   ├── Benchmarks/
│   │   ├── Sources/
│   │   └── Tests/
│   ├── kotlin-markdown-core/
│   │   ├── android-runtime/
│   │   ├── consumers/
│   │   ├── contracts/
│   │   └── src/
│   └── es-markdown-core/
│       ├── src/
│       ├── tests/
│       └── scripts/
├── samples/
├── scripts/
├── docs/
├── LICENSE
└── UPSTREAM.md
```

Path constraints:

- Move existing root `src/` to `packages/markdown-core/core/`.
- Move existing root `extensions/` to `packages/markdown-core/extensions/`.
- Move C engine tests from root `test/` to `packages/markdown-core/tests/`.
- Merge existing `api_test/` into `packages/markdown-core/tests/api/`.
- Move and rename `bench/` to `packages/markdown-core/benchmarks/`.
- Move `fuzz/` to `packages/markdown-core/fuzz/`.
- `packages/markdown-core/tests/` exclusively owns C parser correctness fixtures. Cross-product
  canonical goldens and manifests live only in `specs/canonical-ast/`, without runners. Each binding's
  focused API/correctness cases, consumers, and runners remain in its package. Do not recreate shared
  root-level `tests/`.
- The new stable facade header lives at `packages/markdown-core/include/markdown_core.h`.
- Moving directories must not make all historical internal C headers public.
- Root CMake orchestrates; `packages/markdown-core/CMakeLists.txt` owns C engine target definitions.

Use Git-aware moves to retain traceable history, with directory migration committed separately from
parser behavior changes.

### 13.2 Platform Boundaries

- JNI wrappers belong to `packages/kotlin-markdown-core/`, not the portable C engine.
- Emscripten/WASM glue and generation scripts belong to `packages/es-markdown-core/`.
- Swift AST adapters belong to `packages/swift-markdown-core/`.
- `packages/markdown-core/` remains independently buildable/testable through CMake, without reverse
  dependencies on Swift, Kotlin, Node, or Emscripten-specific glue.

### 13.3 Existing Wrappers

The old `cmark-gfm` repository's SwiftPM and Android/AAR setups do not define the new project's public design:

- Their C source/build integration may serve as a reference, but do not retain old package APIs,
  coordinates, or versions.
- New SwiftPM artifacts must use `swift-markdown-core` / `MarkdownCore`.
- New Kotlin artifacts must use `kotlin-markdown-core`.
- Remove old setups after replacements are complete and covered by consumer tests.

### 13.4 Root Tooling

Root commands cover at least:

- C engine/facade build and tests.
- Swift build/tests/consumer tests.
- Kotlin build/tests/consumer tests.
- ES/WASM build/typecheck/tests/consumer tests.
- Unified formatting and formatting checks.
- Shared AST fixture/conformance tests.
- A root command running all verification together.

### 13.5 Repository Hygiene, Formatting, and Lint

Establish and commit unified hygiene/tooling configuration before platform implementation begins.
Use official or mainstream language recommendations as the baseline, overriding only what the
monorepo needs to standardize explicitly.

#### 13.5.1 General Text Rules

- Root `.editorconfig` has `root = true` and at least UTF-8, LF, final newline, a trailing-whitespace
  policy, `indent_style = space`, `indent_size = 4`, and `tab_width = 4`.
- All manually maintained C/C++, Swift, Kotlin/Kotlin Script, JavaScript/TypeScript, JSON, YAML,
  CMake, and shell files use spaces with a base indentation width of 4.
- `Makefile` recipes are the only regular tab exception required by syntax. `.editorconfig`
  explicitly assigns tabs with display width 4 to `Makefile`/`*.mk`. Do not break Make syntax to remove all tabs.
- Markdown may disable trailing-whitespace trimming to retain hard line breaks; restrict that exception
  to `*.md`.
- Formatters must not rewrite generated, vendored, golden, binary, or package-manager cache files.
  Formatter/linter exclusions must agree and use explicit paths, without broadly skipping handwritten sources.
- Root `.gitattributes` normalizes text to LF and explicitly marks common binary artifacts so checkout
  platforms cannot change golden, fixture, or AST dump bytes.

#### 13.5.2 Ignore Rules and Published Contents

- Root `.gitignore` covers CMake/build directories, SwiftPM/Xcode derived data, Gradle/Kotlin/Android
  native build state, Node/pnpm caches, generated WASM outputs, coverage/test artifacts, IDE/OS
  metadata, `local.properties`, and local secret/env files.
- Do not ignore source code, test fixtures, reviewed AST goldens, Gradle Wrapper files, tool-version
  configuration, or lockfiles required for reproducible CI.
- Secret-free templates such as `.env.example` may be tracked. Ignore actual tokens, signing keys,
  `.env`, and machine-local registry credentials; secret scanning/CI policy prevents accidental commits.
- Prefer package `files` allowlists and `exports` for npm publishing, audited with `npm pack --dry-run`
  or equivalent consumers. `.npmignore`, which can accidentally broaden contents, must not be the sole boundary.
- Maven, SwiftPM, C installs, and GitHub Release artifacts also use allowlists/target ownership, with
  file inventories audited during release dry runs.

#### 13.5.3 Swift

- Use Apple/Swift's toolchain-provided `swift format` and commit root `.swift-format`.
- Generate `.swift-format` from the pinned toolchain's default/recommended configuration, with only
  documented overrides. Set `indentation.spaces` and `tabWidth` to 4.
- Provide mutating formatting and nonmutating checks: `swift format --recursive --in-place ...` and
  `swift format lint --recursive ...`, covering `Package.swift`, sources, tests, and consumers.
- Use root `.swiftlint.yml`, starting from default community rules and enabling only opt-in rules
  clearly appropriate to library APIs.
- `swift format` is the sole source of truth for layout/whitespace. Disable overlapping/conflicting
  SwiftLint style rules; use SwiftLint primarily for correctness, API hygiene, and conventions beyond
  the formatter's scope.
- Pin SwiftLint and Swift toolchain versions in CI/tool-version policy. Lint tools must not become
  consumer runtime dependencies after release.
- Do not create a SwiftLint baseline or globally hide initial violations through `disabled_rules`.
  Document necessary exceptions per rule at the smallest code scope.

#### 13.5.4 Kotlin

- Use JetBrains Kotlin Coding Conventions as the semantic style baseline: spaces, 4-space indentation,
  and no tabs.
- One pinned `ktlint`, integrated through a KMP-compatible Gradle plugin, handles formatting and style
  lint. Do not add a second formatter that rewrites the same sources.
- `.editorconfig` `[*.{kt,kts}]` sets `ktlint_code_style = ktlint_official`, `indent_style = space`,
  `indent_size = 4`, and `tab_width = 4`. If ktlint conflicts with official Kotlin conventions,
  document a narrow exception and follow Kotlin's conventions.
- Gradle provides `ktlintFormat` and `ktlintCheck` for all KMP source sets, Gradle Kotlin scripts,
  tests, and consumer fixtures.
- Pin ktlint/plugin versions through the version catalog or an equivalent single location. Do not
  create lint baselines or enable experimental rules by default.

#### 13.5.5 ECMAScript/TypeScript and C

- ES/TypeScript uses pinned Prettier with `useTabs: false` and `tabWidth: 4`, plus ESLint
  recommended/type-aware configuration compatible with the current TypeScript/ESM setup.
- Prettier owns layout; ESLint must not duplicate conflicting formatting rules. Provide `format`,
  `format:check`, and `lint:es` commands.
- C/C++ uses repository-pinned `.clang-format`, based on LLVM style or another agreed mainstream
  baseline, including `UseTab: Never`, `IndentWidth: 4`, and `TabWidth: 4`.
- C lint includes at least warnings-as-errors for supported compilers. Add reproducible `clang-tidy`
  checks where targets/toolchains are available, without blocking unsupported toolchains on
  platform-specific false positives.
- Exclude generated scanners, vendored code, and mechanically generated WASM/JNI glue only by explicit
  paths. Handwritten C facade, JNI, cinterop, and WASM glue undergo the corresponding formatting/lint.

#### 13.5.6 Root Commands and CI

- Provide unified root `format`, `format:check`, and `lint`, plus `format:swift`, `format:kotlin`,
  `format:es`, `format:c`, and corresponding check/lint subcommands for focused runs.
- `format` may change the working tree. `format:check` and `lint` are read-only and fail nonzero on violations.
- CI runs only nonmutating checks; it must not format and then report success or commit fixes automatically.
- Pin formatter, linter, compiler/toolchain, and package-manager versions directly or through
  lockfiles/catalogs. Upgrades are reviewable changes accompanied by repository-wide formatting diffs.
- Each language has one layout formatter. Separate formatter/linter responsibilities to prevent
  rewrite loops between developer machines, IDEs, Gradle, SwiftPM, and CI.
- Perform the inherited C engine's first four-space formatting migration after Git-aware directory
  moves, in a separate mechanical commit. Do not combine it with moves, symbol renames, parser
  behavior changes, or renderer deletion, preserving reviewable history.

### 13.6 Gradle/KMP Engineering Conventions

#### 13.6.1 Versions and Build Model

- Use Kotlin DSL: `settings.gradle.kts`, `build.gradle.kts`, and required convention plugins.
- Commit Gradle Wrapper scripts, JAR, and properties. Pin a stable release and its official distribution
  SHA-256. Local, IDE, and CI commands use `./gradlew`, without globally installed Gradle.
- Choose the latest stable mutually supported Gradle/Kotlin Multiplatform/Kotlin/AGP/JDK combination.
  Do not independently chase each latest version into an unsupported matrix.
- `gradle/libs.versions.toml` is the single version entry for plugins, Kotlin, AGP, ktlint, publishing
  plugins, and Kotlin/JVM dependencies. The Gradle term is version catalog, not version category.
- Catalogs centralize requested versions but do not replace constraints/locking. Avoid dynamic
  versions and changing/SNAPSHOT dependencies; enable and commit locking for configurations requiring
  stable resolution.
- Commit `gradle/verification-metadata.xml` and enable checksum/signature verification for plugins
  and dependencies. Human-review verification metadata diffs when adding/upgrading dependencies.
- Centralize repositories through `pluginManagement` and `dependencyResolutionManagement` in
  `settings.gradle.kts`; prohibit arbitrary unreviewed repositories in subprojects.
- Put repeated module logic in typed convention plugins/included `build-logic`, not broad
  `subprojects {}` mutation, `afterEvaluate`, or configuration-time side effects.

#### 13.6.2 Toolchains and Modern Gradle Features

- Pin the Gradle daemon JVM, Java/Kotlin compilation toolchain, and artifact bytecode target separately.
  None may depend implicitly on the developer's current `JAVA_HOME`.
- JVM/KMP targets use Gradle/Kotlin toolchain APIs, including `jvmToolchain`/typed compiler options.
  Declare Android compile/target/min SDK and native ABI matrices centrally in catalogs or typed constants.
- If auto-provisioning toolchains, declare trusted download repositories. CI and IDEs use the same
  toolchain-resolution policy.
- Target configuration-cache compatibility and check major build/test/lint/publication-dry-run tasks
  in CI. Do not hide errors to enable caching. For incompatible upstream plugins, document the exact
  task/plugin and conditions for removing the exemption.
- Enable local/CI build caches and lazy task registration. Custom tasks declare inputs, outputs, and
  environment-sensitive properties. Do not compile C, probe untrackable local state, or read release
  secrets during configuration.
- Enable incubating features, such as isolation modes not yet supported by the selected Kotlin/AGP
  plugins, only after compatibility verification. Using modern features does not mean placing
  unstable flags directly in release builds.

#### 13.6.3 IDE Import/Sync

- Clean checkouts import directly from the repository root as Gradle projects in supported stable
  IntelliJ IDEA and Android Studio. Do not require checked-in `.idea`, generated `.iml`, preliminary
  custom scripts, or manual build-file edits.
- The legacy `idea` plugin is unnecessary for generating project files; modern IDEs import Gradle/KMP
  models. Allow narrow IDEA DSL only for settings the standard model cannot express, with a documented reason.
- Gradle sync succeeds without Maven Central credentials, PGP keys, release environments, or prebuilt
  natives. Configure/execute signing, uploads, and expensive native packaging only when their
  execution tasks are requested.
- IDEs correctly display `commonMain/commonTest`, Android, JVM, and declared Native source sets,
  without duplicate content roots, incorrect generated-source roots, or unresolved expect/actual declarations.
- Document the supported IntelliJ IDEA, Android Studio, JDK, Gradle, Kotlin, KMP plugin, and AGP matrix.
  Rerun IDE import/sync regressions when any component changes.
- CI includes Gradle Tooling API/model-load or equivalent headless import smoke tests,
  `./gradlew projects`, KMP tooling metadata, and target compilation tests. The release checklist also
  requires a clean import/sync in both supported IntelliJ IDEA and Android Studio.
- IDE imports must not depend on implicit machine-local state beyond `local.properties`. Standard
  IDE/`ANDROID_HOME` configuration may supply the Android SDK path; document every other prerequisite.

#### 13.6.4 KMP Publication

- Use `org.jetbrains.kotlin.multiplatform`, Android KMP library plugin/DSL, `maven-publish`/signing,
  or a pinned publishing plugin supported by current official Kotlin workflows. Handwritten HTTP
  uploads cannot replace Gradle publications.
- Explicitly enable Android library publication to generate AARs; JVM generates JARs and Native
  generates corresponding KLIB/native artifacts. Root `kotlinMultiplatform` retains complete target
  references and Gradle Module Metadata.
- Publish first to an isolated local Maven repository. Audit POM, `.module`, JAR, AAR, KLIB,
  sources/docs, signatures, checksums, coordinates, variants, native payloads, and root-to-target
  references before allowing Maven Central access.
- One controlled job/host uploads the complete release publication set, avoiding duplicate
  coordinates from multiple hosts. Matrix jobs may build and verify artifacts, but final
  staging/deployment aggregates them and checks completeness.
- Verification proves Gradle/KMP/Android consumers select variants through the root coordinate,
  real Maven consumers driven by the repository-owned Wrapper use the target-specific JVM coordinate
  solely through Maven repositories/POMs, and all consumers obtain matching OS/architecture/ABI
  natives. Real Maven consumers are the final smoke test for promised effective-model, resolution,
  and lifecycle compatibility.

## 14. Cross-platform Consistency

The three platforms agree on:

- Node kinds and names.
- Parent/child hierarchy and child order.
- Optional-field nullability.
- String contents and Unicode semantics.
- List start/tight/task state.
- Heading level.
- Code-block info/language/literal/fence state.
- Links, images, and titles.
- Table headers/body/alignment.
- Formula, directive, and footnote nodes.
- Source position/range coordinate meanings.
- Syntax extensions and parser options.
- Parse failure behavior.

Conformance tests cover the same public behaviors. All four conformance targets enumerate the same
cases from `specs/canonical-ast/manifest.json`, without platform copies or a second manifest.

The root shared spec exclusively owns the complete canonical file-tree `.ast` golden corpus, without
a second JSON or other tree schema. Swift/Kotlin/ES expose `TreeDumper.dump(markup)` and `Markup.dump()`,
independently traverse their immutable ASTs, and compare byte-for-byte with shared `.ast` files. They
must not call C dumps, read another binding's output, or build production ASTs from dump text.

## 15. Quality and Safety Requirements

- Public C headers can safely be included by C and C++ consumers.
- Every C/WASM string boundary uses explicit lengths without relying on input NUL termination.
- Test ASCII, CJK, emoji, supplementary Unicode scalars, combining marks, and embedded NUL boundaries.
- Test empty input, large documents, deep nesting, and malformed Markdown.
- Test resource release on parse success, parse failure, and platform object-construction failure.
- Retain sanitizer-compatible test paths for the C engine/facade.
- AST dumps produce byte-for-byte stable canonical output for the same input, options, and platform.
- Migrate spec tests, extension specs, fuzzing, and parser/API tests formerly using rendered output
  to AST dumps or direct accessors.
- Delete renderer-specific tests after renderer removal. Do not retain renderers to preserve old expectations.
- Do not trade user-visible undefined behavior for zero-copy access.
- Prioritize clear ownership and semantic consistency initially; use benchmarks later to determine
  whether more complex cross-boundary optimization is needed.

## 16. Acceptance Criteria

The initial release is acceptable when:

1. The project uses the Markdown Core identity and records fork origins and licensing.
2. The C engine, C facade, and three platform packages live in one monorepo.
3. `swift-markdown-core`, `kotlin-markdown-core`, and `@nouprax/es-markdown-core` build from clean checkouts.
4. All three platforms expose Markdown parsing only through `Document.parse`.
5. Identical CommonMark/GFM/custom-extension fixtures produce equivalent ASTs across the three platforms.
6. Production parsing contains no AST JSON serialization/deserialization.
7. New public C APIs use only `markdown_core_*` naming and expose no AST mutation.
8. Public AST APIs on all three platforms expose no mutation.
9. Public APIs on all three platforms expose no HTML or other renderer.
10. Unicode is not corrupted on any platform.
11. Repeated parse/release and all known failure paths have no evident leaks or dangling pointers.
12. SwiftPM, Maven/Gradle, and npm each have independent consumer tests.
13. A root verification command checks C, Swift, Kotlin, ES/WASM, and conformance tests.
14. Project/package versions and C engine/facade source use one coordinated release; this is not a
    cross-release binary ABI promise.
15. The engine resides within `packages/markdown-core/{core,extensions,tests}`; old root `src/`,
    `extensions/`, and `test/` no longer supply production builds.
16. MS Copilot accordion, annotation, citation, and MS formula-delimiter sources, options, symbols,
    CLI flags, tests, and documentation are completely removed.
17. C exposes a native deterministic AST dump API and test CLI independent of renderers.
18. CommonMark/extension specs and other parser tests verify AST dumps or direct accessors, no longer
    rendered HTML/XML/CommonMark or similar output.
19. HTML, XML, CommonMark, LaTeX, man, and plaintext renderer implementations, public functions,
    extension callbacks, CLI formats, build inputs, and dedicated tests are removed.
20. A case-insensitive repository/artifact audit finds no cmark names outside
    `LICENSE`/`COPYING`/`THIRD_PARTY_NOTICES`, README/UPSTREAM origin statements, explicit migration
    history, and Git history.
21. `README.md` and `UPSTREAM.md` state the cmark/cmark-gfm origins and partial rewrite, with all
    applicable copyright/license notices preserved.
22. SwiftPM releases through `https://github.com/nouprax/markdown-core`, exporting `MarkdownCore`
    product/module without a `Nouprax` prefix.
23. Maven Central ownership is verified through `nouprax.com` DNS; the root
    `com.nouprax:kotlin-markdown-core:<version>` and all referenced target publications are released.
24. npm organization `nouprax` publishes public `@nouprax/es-markdown-core`.
25. npm OIDC is bound to the exact `nouprax/markdown-core` workflow/environment, with no unused
    long-lived publishing tokens.
26. Central Portal tokens and PGP signing secrets exist only in protected `release`; permissions,
    expiration, transition, and revocation processes have been verified.
27. SwiftPM/GitHub Releases use SemVer tags and minimally privileged `GITHUB_TOKEN`, without
    nonexpiring PATs.
28. `docs/releasing.md` fully documents publishing, secret names, permissions, bootstrap, dry runs,
    rotation, revocation, and compromise response, without secret values.
29. npm `1.0.0` bootstrap establishes lineage; protected `v1.0.1` failed before publishing and stays
    immutable; the first coordinated four-platform version is `1.0.2`. No old tags were migrated,
    retained, or archived.
30. Swift `1.0.2` supports iOS 18/26 and macOS 15/26 with corresponding CI; minimum deployment targets
    advance under the latest-two-major-generations policy.
31. Kotlin delivers a KMP library with public APIs in `commonMain`; the first release verifies at least
    Android, general JVM, and declared Native targets, rather than only an Android AAR.
32. All three `Document.parse` APIs are synchronous. ES initializes WASM before calls and exposes
    neither `parseAsync` nor a second parser entry point.
33. Kotlin retains `List`; consumer compilation tests prove coexistence with `kotlin.collections.List`
    through qualification/import aliases.
34. Every `Markup` exposes nonoptional `Scope(start, end)` with native C coordinate values and
    semantics unchanged; dumps and conformance tests prove bindings do not rewrite them.
35. All three platforms expose equivalent `ParseOptions`, typed Visitors, and read-only depth-first
    Walkers with entering/exiting events. Focused package cases cover defaults and disabling each option.
36. TypeScript ASTs recursively use `readonly`, without `Object.freeze`, recursive freezing, or
    Proxy-enforced runtime immutability.
37. C/C++ consumers and failure tests cover length-delimited UTF-8 views, explicit error/free models,
    and the clean-rebuild ABI policy.
38. Canonical dumps use the specified UTF-8 file-tree structure/connectors without property/index edge
    labels. Each node occupies one line with complete behavior-bearing fields, fixed order, strict
    scope, child count, escaping, LF, and a final newline. Four-platform byte-for-byte diffs against
    root shared manifests/goldens locate public behavior drift. Bindings expose `TreeDumper.dump(markup)`
    and convenient `Markup.dump()`, without defining a serialization API.
39. Root `.editorconfig`, `.gitattributes`, `.gitignore`, `.clang-format`, `.swift-format`,
    `.swiftlint.yml`, Prettier, and ESLint configurations are present and verified. Handwritten
    languages default to spaces and 4-space indentation; tabs of width 4 occur only where syntax
    requires them, such as Make recipes.
40. Swift separates `swift format`/SwiftLint; Kotlin uses one ktlint for format/check; ES separates
    Prettier/ESLint; C uses clang-format/compiler lint. Versions are pinned and no two formatters
    compete over the same sources.
41. Root `format:check` and `lint` pass read-only from clean checkouts, and CI modifies no files.
    Consumer/release dry runs prove ignore rules/allowlists retain required artifacts while excluding
    caches, local state, and secrets.
42. Gradle/KMP publishes complete root/target sets to Central. Root `.module`, JVM JAR, Android AAR,
    and declared Native artifacts reference each other correctly, share versions, and omit no publications.
43. Supported IntelliJ IDEA and Android Studio import clean checkouts from the root and sync without
    release secrets, generated `.idea`/`.iml`, or preliminary native builds.
44. Wrapper/checksum, version catalogs, JVM/toolchain policy, dependency verification/locking,
    configuration-cache checks, and centralized repositories are enabled and CI-verified, using the
    latest stable compatible matrix rather than unsupported independently latest versions.
45. KMP Gradle, JVM Gradle with Module Metadata, real JVM Maven, and Android Gradle consumers resolve
    correct publications/variants from local/staged Maven repositories and load matching natives.
    Maven is pinned and launched by the repository-owned Wrapper.

## 17. Task Plan

This is the default implementation sequence for new sessions after setup. Each phase uses flat
`Tasks` and `Acceptance` lists, with one task or criterion per physical line and no continuations or
nested lists. Dependent phases proceed only after all acceptance criteria pass. Phases 6–9 have a
mandatory order: establish AST dumps, freeze the unified test architecture and migrate CTest, migrate
parser assertions, then remove renderers.

### Phase 0: Establish the New Project Baseline

Tasks:

- [x] Establish `nouprax/markdown-core` and a new release lineage.
- [x] Use commit `711032b2a16cf25c3df75033833eba086b17ca6a` as the C engine baseline.
- [x] Migrate or retain no old tags; npm `1.0.0` bootstrap establishes the new lineage; protected `v1.0.1` failed before publication and remains immutable; the first coordinated tag is `v1.0.2`.
- [x] Preserve license/copyright notices.
- [x] Create `UPSTREAM.md` with fork origins, baseline commit, and the divergence decision.
- [x] Record a runnable baseline for CMake, Make, tests, sanitizers, and benchmarks before migration.
- [x] Inventory MS-specific sources, render functions, render-dependent tests, and CLI output formats for later removal.

Commands, results, and removal inventories are in `docs/migration/2026-07-11-phase-0-baseline.md`.

Acceptance:

- [x] The new repository runs the recorded C baseline verification without parser behavior changes.
- [x] MS-specific and renderer removal inventories are complete and traceable.

### Phase 1: Directory and Build Refactoring Only

Tasks:

- [x] Establish `packages/markdown-core/`.
- [x] Use Git-aware moves from `src/` to `packages/markdown-core/core/`.
- [x] Move `extensions/` to `packages/markdown-core/extensions/`.
- [x] Move C tests, `api_test/`, `bench/`, and `fuzz/` to their new owners.
- [x] Update CMake, Make, CI, scripts, and include paths.
- [x] Establish root CMake orchestration and target ownership in `packages/markdown-core/CMakeLists.txt`.
- [x] Create `.editorconfig`, `.gitattributes`, and `.gitignore` covering toolchains, local state, and secrets; standardize spaces and 4-space indentation while retaining Make recipe tabs.
- [x] Create `.clang-format`, `.swift-format`, `.swiftlint.yml`, Prettier, and ESLint configurations, pinning tool versions.
- [x] Integrate pinned ktlint through Gradle, with `ktlint_official`, 4-space indentation, `ktlintFormat`, and `ktlintCheck`.
- [x] Establish the Gradle Wrapper with a distribution checksum; choose and document the latest stable compatible Gradle/Kotlin/KMP/AGP/JDK matrix.
- [x] Create `gradle/libs.versions.toml`, centralized repositories, dependency locking, and `gradle/verification-metadata.xml`.
- [x] Configure daemon/compile toolchains, configuration/build caches, and lazy/custom-task input/output policy.
- [x] Add a credential-free Gradle Tooling API/model-load headless import smoke test and an IntelliJ IDEA/Android Studio clean-sync checklist.
- [x] Establish root `format`, `format:check`, `lint`, and language subcommands; integrate nonmutating checks into CI.
- [x] Create consistent, narrow generated/vendored/golden exclusions across tools and verify no handwritten sources are accidentally excluded.
- [x] Establish npm `files`/`exports` allowlists and equivalent content audits for other release artifacts.
- [x] After the directory-migration commit, migrate inherited handwritten C/C++ to clang-format four-space style in a separate formatting-only commit.
- [x] Preserve parser behavior, public behavior, and fixtures.

Acceptance:

- [x] All recorded premigration C builds, tests, and sanitizers pass again.
- [x] Root formatting checks and lint pass read-only from a clean checkout.
- [x] Handwritten sources follow the agreed 4-space style; ignore and allowlist audits pass.
- [x] This phase contains no intentional parser semantic changes.

Implementation commits, supplementary audits, Tooling API model-load evidence, and verification are
in `docs/migration/2026-07-11-phase-1-validation.md`.

### Phase 2: Remove Private MS Markdown Extensions

Tasks:

- [x] Remove `ms_copilot_accordion` sources, headers, node kinds, registration, and accessors.
- [x] Remove `ms_copilot_annotation` sources, headers, node kinds, registration, and accessors.
- [x] Remove `ms_copilot_citation` sources, headers, node kinds, registration, and accessors.
- [x] Remove `CMARK_OPT_MS_COPILOT_*` options and CLI flags.
- [x] Remove `CMARK_OPT_MS_FORMULA_DELIMITERS`, MS formula-parser branches, and single-backslash scanners.
- [x] Regenerate scanner artifacts and remove MS inputs from CMake, Make, and fuzz configurations.
- [x] Remove all MS-specific tests, fixtures, scripts, and documentation.
- [x] Run case-insensitive source audits for `ms_copilot`, `CMARK_OPT_MS_`, `ms-formula`, and other known identifiers.

Acceptance:

- [x] All C builds, tests, and sanitizers pass.
- [x] Known MS-specific parser surfaces are entirely absent from sources, builds, CLI, exported symbols, tests, and documentation.

Removal inventories, synchronized man-page fixes, source/install/AAR audits, and verification are in
`docs/migration/2026-07-11-phase-2-ms-removal.md`.

### Phase 3: Remove cmark Naming Repository-wide

Tasks:

- [x] Create a case-insensitive inventory of filenames, directories, symbols, types, macros, guards, targets, libraries, CLI, tests, scripts, generated files, and metadata.
- [x] Rename `cmark_*` functions/types/globals to `markdown_core_*` or documented internal prefixes.
- [x] Rename `CMARK_*` macros/enums/guards to `MARKDOWN_CORE_*` or documented internal prefixes.
- [x] Rename sources, headers, targets, library/pkg-config names, CLI binaries, tests, and scripts containing `cmark`/`cmark-gfm`.
- [x] Update CMake, Make, CI, includes, generated files, export maps, and install/package metadata.
- [x] Remove compatibility aliases for old ABI, library, CLI, and package names; retain no dual branding.
- [x] Preserve and verify original cmark/cmark-gfm license/copyright notices.
- [x] State explicitly in `README.md` and `UPSTREAM.md` that the project derives from cmark/cmark-gfm and has rewritten parts of that code.
- [x] Run case-insensitive `cmark` audits on the working tree, installed artifacts, static/shared symbol tables, CLI help, and package archives.
- [x] Review every remaining match, allowing only licenses, attribution, origin/migration documents, or Git history.

Acceptance:

- [x] All C builds, tests, and sanitizers pass.
- [x] Artifacts, implementation sources, and operational documentation contain no cmark naming.
- [x] Remaining matches occur only in licenses, copyright, README/UPSTREAM origins, explicit migration history, or Git history.

Commands, naming maps, initial inventory, and remaining exceptions are in
`docs/migration/2026-07-11-phase-3-rebrand.md`.

### Phase 4: Correct Directive Attribute-list → String-map JSON Semantics

Tasks:

- [x] Inventory directive behavior: bare/key-value attributes, single/double quotes, HTML-shortcut rejection, duplicate rules, C APIs, render callbacks, fixtures, and packaged headers.
- [x] Freeze the source attribute-list → normalized string-map JSON contract, specifying absent/`{}`, ownership, and deterministic serialization.
- [x] Freeze reviewed invalid/truncated attribute-list fallback fixtures for inline, leaf-block, and container-block directives.
- [x] Define length, overflow, transactional-failure, and leak-free resource-safety contracts for nonrecursive attribute scanning/parsing.
- [x] Store parsed results in typed `directive_attribute` payloads, maintaining normalized JSON/XML caches without retaining original Markdown attribute-list text.
- [x] Implement bare, unquoted, single/double-quoted values and uniform last-duplicate semantics, treating `id`/`class` as ordinary keys.
- [x] Migrate inline, leaf-block, container-block, and label-associated attribute paths to the shared attribute-list contract.
- [x] Update getters to return `NULL` for absent attributes and normalized string-map JSON for present ones, documenting lifetime/ownership.
- [x] Update setters to accept only complete string-map JSON, normalize successful input, and leave nodes unchanged on failure.
- [x] Prohibit directive HTML-attribute projection; CommonMark emits normalized attribute lists, while XML only transport-escapes normalized JSON.
- [x] Synchronize public, SPM, Android Prefab, install, and package headers; audit removal of incorrect opaque-source wording.
- [x] Rewrite directive fixtures for every shape, absent/`{}`, bare/quoted values, shortcut rejection, duplicates, Unicode, and malformed fallback.
- [x] Expand C API tests for source-to-JSON normalization, transactional failure, ownership, replacement, escaped NUL, duplicates, and wrong node kinds.
- [x] Add invalid/pathological tests for malformed, truncated, or unclosed attributes and sanitizer failure modes.
- [x] Add size-doubling complexity tests for valid/unclosed long values, consecutive backslashes, many unique keys, and many duplicate keys, ruling out O(n²) scanning/deduplication.
- [x] Update C++ consumers and wrappers still directly consuming attributes to verify public normalized string-map JSON.
- [x] Revise the Phase 4 report with corrected behavior diffs, reviewed goldens, API contracts, resource limits, commands, and package audits.
- [x] Rerun Release C tests, C/C++ consumers, ASan, UBSan, format/lint, source/install/package/AAR audits, and `git diff --check`.

Acceptance:

- [x] Complete C spec/API regressions freeze directive attribute-list → normalized string-map JSON semantics.
- [x] Size-doubling tests rule out O(n²) behavior for valid, invalid, unclosed, and many-key adversarial inputs.
- [x] Release, C/C++ consumers, ASan, UBSan, format/lint, and package-header audits pass.
- [x] Public, installed, and packaged headers match implementations.

Implementation and verification are in `docs/migration/2026-07-11-phase-4-directive-json.md`.

### Phase 5: Freeze the Canonical AST Contract

Tasks:

- [x] Inventory retained `Markup` nodes, fields, and nullability against `markdown-renderer`.
- [x] Confirm the canonical AST contains no removed MS-specific nodes or fields.
- [x] Retain concrete `List` and add Kotlin qualification/import-alias compilation tests.
- [x] Freeze nonoptional `Scope(start, end)` for every `Markup`, preserving native C coordinate values and semantics.
- [x] Freeze shared `ParseOptions`, default-enabled capabilities, and renderer-only option removal rules.
- [x] Define typed Visitor and read-only depth-first Walker contracts for all three platforms.
- [x] Freeze each node kind's file-tree dump schema, complete behavior-bearing fields, field order, connectors, child count, escaping, and strict scope output.
- [x] Establish C package-owned Markdown fixtures and canonical file-tree `.ast` goldens; specify that C dumps and public binding `TreeDumper` implementations independently produce the same tree text, bindings compare only local focused snapshots, and they must not read C goldens, call C dumps, or use tree text for production transport.

Acceptance:

- [x] Every retained node has explicit kind, fields, ownership, and coordinate semantics.
- [x] Every retained node has at least one fixture.

Frozen contracts, C package-owned tree goldens, independent binding adapter rules, fixture coverage,
Phase 6 inputs, and verification are in `docs/migration/2026-07-11-phase-5-canonical-ast.md`.

### Phase 6: Implement the Read-only C AST Facade and Native Dumps

Tasks:

- [x] Audit existing C APIs for an AST dump interface independent of renderers.
- [x] Do not treat `cmark_render_xml` or CLI `-t xml` as the new dump interface.
- [x] Create `include/markdown_core.h`.
- [x] Implement typed parse options, default initialization, and `markdown_core_document_parse/free`.
- [x] Implement root, kind, child/sibling traversal, and property accessors.
- [x] Implement errors, UTF-8 string views, and source-location APIs.
- [x] If no independent dump exists, add native deterministic dump/free APIs and a test CLI mode.
- [x] Ensure canonical file-tree connectors express real hierarchy and cover every core/extension behavior-bearing field, child order/count, null/empty/default distinctions, Unicode, and each node's `Scope`.
- [x] Add completeness fixtures for every kind and field, proving field changes produce visible diffs.
- [x] Add C dump goldens, C/C++ consumers, ownership tests, and sanitizers; C dumps must match Phase 5's C-owned `.ast` goldens byte-for-byte.
- [x] Hide unnecessary internal symbols and expose no mutation/render APIs to platform packages.

Acceptance:

- [x] The facade independently parses all C-owned fixtures and traverses complete ASTs read-only.
- [x] Native dumps produce stable, readable file trees without calling renderers.
- [x] Completeness fixtures prove changes to any public behavior-bearing field produce visible diffs.
- [x] All paths pass sanitizers.

Implementation and verification are in `docs/migration/2026-07-11-phase-6-c-ast-facade.md`.

### Phase 7: Freeze Unified Test Architecture and Consolidate CTest Suites

Tasks:

- [x] Remove the inherited Pro Git benchmark and all traces: Makefile `progit`, runtime `git clone`, `BENCHFILE`/`benchinput.md` generation/dependency/cleanup rules, root checkouts/generated inputs, related caches/temp/output, operational docs, and `.gitignore` entries; do not run inherited `make bench` before removal; afterward Phase 7 no longer references or special-cases that corpus source.
- [x] Freeze the historical root correctness routing that used `pnpm test` to aggregate repository-wide correctness and delegate to fixed language entries; the later execution-platform routing revision in this phase supersedes that model.
- [x] Revise routing from the repository audit so three verification families map directly to named native execution-platform targets, with suite/case discovery and filtering owned by native runners.
- [x] Freeze independent `benchmark:<platform>` routing only for declared trusted measurement environments, never implicitly run by correctness or ordinary `verify`; CI schedules separate benchmark jobs/workflows, with no empty unsupported-platform targets.
- [x] Freeze runner ownership: CTest for C, `swift test` + Swift Testing for Swift, Gradle/KMP tasks for Kotlin, and package-native Node/browser/type runners for ES; pnpm, shell, Make, or another platform runner must not recreate a second suite graph.
- [x] Freeze target semantics: `test:*` runs complete correctness, `conformance:*` complete contract/spec checks, and `benchmark:*` complete performance workloads; none may degrade to build/lint, silent skipping, or empty/no-op targets.
- [x] Freeze cross-platform suite/workload taxonomy and names: correctness includes at least `api`, `ast`, `consumer`, `errors`, `ownership`, `unicode`, `robustness`, `pathological`, and `packaging`; conformance separately verifies public contract/spec/schema mapping; performance uses independent `benchmark` workloads/labels; platform-specific extensions must not give unrelated names to identical semantics.
- [x] Implement mutually exclusive native correctness, conformance, and benchmark targets, with stress-shaped inputs represented separately as correctness robustness cases and benchmark workloads.
- [x] Freeze discovery/filter contracts: runners list suites/cases, select by suite/name/label/filter, return machine-readable status and actionable diffs; root docs map equivalent pnpm/native/IDE/CI commands so CI can shard by feature/cost groups such as `api`, `spec`, `extensions`, `pathological`, and `benchmark`.
- [x] Freeze local fixture ownership: C exclusively owns parser canonical `.ast` goldens; bindings use only local focused schema-mapping cases, without reading/copying C goldens, calling other adapters, constructing production ASTs from dumps, or adding root shared fixtures.
- [x] Freeze common execution policy for UTF-8 byte comparison, LF/final newline, temporary-directory ownership, timeouts, expected failures, process cleanup, serial/resource locks, warmup/repeats, and deterministic diagnostics; helpers remain native to each platform, without cross-language test bridges.
- [x] Freeze IDE/AI/editor contracts through committed and verified CMake/CTest presets, Swift Testing discovery, Gradle tasks, and ES configuration; repository tasks map directly to VS Code/CLion, Xcode, IntelliJ/Android Studio, and general CLI use without personal IDE state.
- [x] Replace build-only `pnpm test:swift` with real `swift test`; organize using Swift Testing `@Suite`/`@Test`, without new XCTestCase foundations; retain `swift build` as a separate build task.
- [x] Use named Gradle/KMP platform tasks and ES Node/browser correctness plus separate conformance/benchmark scripts; root `family:<platform>` tasks delegate directly without language aggregates.
- [x] Make `test:c-host`, `conformance:c-host`, and `benchmark:c-host` configure/build one CMake graph and delegate to mutually exclusive CTest presets, without duplicating test lists or parameters in package scripts, Make, CI, or wrappers.
- [x] Inventory all C tests: native units, read-only facade, legacy API, C/C++ consumers, CommonMark/extension specs, regressions, entities, pathological/complexity cases, CLI, fuzz smoke, sanitizers, and benchmarks, recording runners, fixtures, timeouts, resources, and expectations.
- [x] Establish stable CTest suites/labels including at least `api`, `facade`, `consumer`, `spec`, `extensions`, `regression`, `pathological`, `fuzz`, and `benchmark`; every test is independently discoverable, runnable, and diagnosable through `ctest -R` and `ctest -L`.
- [x] Migrate all Python runners and ctypes/CLI bridges under `packages/markdown-core/tests/` to unified CMake-built/invoked implementations; prefer native C/C++ for API/accessor/ownership tests and already-pinned standard tooling for data-driven specs, without new frameworks or package dependencies.
- [x] Remove `find_package(Python3)`, `PYTHON_EXECUTABLE`, doctest runners, and Python-missing skip branches; with `MARKDOWN_CORE_TESTS=ON`, every declared suite exists and missing required tools fail configuration explicitly.
- [x] Establish shared test support for binary/library discovery, options, fixtures, UTF-8 comparisons, canonical diffs, timeouts, expected failures, temporary files, process cleanup, and diagnostics; suites must not duplicate subprocess/ctypes glue.
- [x] Register C API, spec, extension, entity, regression, pathological, directive-complexity, inline-delimiter, dump-CLI, and C/C++ consumer tests as appropriate CTest suites, preserving granularity rather than collapsing everything into one opaque script.
- [x] Migrate fuzz smoke to deterministic parse/traverse/dump/free CTest suites; long campaigns remain explicit nondefault tasks using the same harness/corpus.
- [x] Freeze vendored-corpus policy: one-time explicit maintenance/import workflows may snapshot network corpora into `tests/corpora/<name>/`, but correctness, benchmarks, CI, IDEs, and ordinary build/test commands remain fully offline and never clone/download/update corpora at runtime.
- [x] Commit each corpus manifest with canonical URL, immutable commit/tag, imported paths, original/normalized SHA-256, byte size, import command/version, update process, copyright/attribution, full license, and redistribution/package policy; import only licenses explicitly allowing commercial use, modification, repository redistribution, and automated testing under project policy, preferring project-authored, CC0/public-domain, MIT, BSD, or Apache-2.0 content.
- [x] Any clone runs only in a temporary directory of an explicit opt-in maintenance command at a pinned revision; copy only manifested files, then remove `.git`, checkouts, archives, intermediate concatenations, caches, and temporary directories; never create or retain source checkouts at the root.
- [x] Add a Phase 7 benchmark preflight guard that fails on unmanaged checkouts, loose generated inputs, missing manifests/licenses/hashes, hash mismatches, corpora in release packages, or network access by ordinary test/bench commands; accept only policy-compliant, manifested, verified `tests/corpora/` snapshots.
- [x] Generate/select corpora comparable in size to historical large-input benchmarks with commercial-use/modification/redistribution rights; remove runtime network fetching, loose generated inputs, Python statistics, and Make-only orchestration, using reviewed deterministic vendored corpora, existing samples, and reproducible in-program datasets.
- [x] After corpus migration, remove transient traces and hidden entry points: unmanaged checkouts/generated inputs, related caches/temp/output, runtime download/generation/cleanup rules, and masking `.gitignore` entries; retain only explicitly tracked, licensed, manifested, hashed corpus files.
- [x] Establish an ordinary but independently scheduled CTest `benchmark` suite for representative documents, large inputs, deep nesting, extensions, and adversarial size doubling; fix warmup/repeats, timeouts, serial/resource locks, and output formats, using stable complexity/relative-regression assertions rather than volatile absolute wall-clock thresholds.
- [x] Make `make test`, `make bench`, sanitizers, CI, and IDE tasks delegate to the same CMake graph and CTest presets/labels; `make test` means correctness, and `make bench` maps to `benchmark:c-host`/CTest `benchmark`, with no second Make runner.
- [x] Add topology audits for consistency between CMake declarations, CTest discovery, pnpm tasks, and CI; prohibit `.py` runners, disabled/optional/silently skipped required suites, runtime corpus network dependencies, unmanaged checkouts/generated inputs across workspace/build/package/cache, and masking `.gitignore` entries; verify corpus manifests/licenses/hashes and package exclusion.
- [x] Add a repository-wide architecture audit proving every implemented `test:*`/`conformance:*`/`benchmark:*` task directly invokes a named native target, executes a nonempty selection, and supports independent discovery/filtering; families are mutually exclusive and CI/IDE mappings omit or duplicate no execution.

Acceptance:

- [x] Repository-wide architecture, entry names, runner ownership, taxonomy, fixtures, filters, timeouts, failures, and IDE discovery contracts are frozen.
- [x] Root routing exposes only peer `test:<platform>`, `conformance:<platform>`, and `benchmark:<platform>` families; platform IDs identify both language and real execution destination.
- [x] There are no cross-host aggregates, intermediate routing layers, language aggregates, suite-level tasks, `:full`, public `stress` tasks, or generic routers; explicit maintenance tasks stay outside routing.
- [x] Platform tasks directly invoke named native targets; suite/case discovery/filtering belongs only to CTest, SwiftPM/xcodebuild, Gradle/KMP/instrumentation, or ES package-native runners.
- [x] Correctness, contract/spec conformance, and benchmark discovery are mutually exclusive; only independent workflows schedule benchmarks, and no entry may be build-only, no-op, or silently skipped.
- [x] Large, deep, and repeated inputs verify results as correctness robustness cases and measure performance as separate benchmark workloads.
- [x] The Pixel 10 Pro XL Gradle Managed Devices group provisions API 36/4 KB and API 36/16 KB emulators; local and CI use identical tasks, GMD owns lifecycle, and cleanup runs only when needed.
- [x] Linux x64 has no local container/emulation substitute; actual execution belongs to Phase 19 CI acceptance.
- [x] CI explicitly maps all declared platforms and applicable conformance targets; topology audits verify tasks, native targets, destinations, exclusive selections, and nonempty execution; remote green evidence belongs to Phase 19.
- [x] C correctness, conformance, and benchmarks share one CMake/CTest graph with exclusive labels/presets; every C suite is discoverable without Python or runtime networking.
- [x] External corpora are tracked, pinned, licensed, attributed, manifested, hashed offline snapshots excluded from release packages, with no unmanaged checkouts, archives, loose generated inputs, or caches.
- [x] CLI, IDE, AI, and CI use the same platform tasks and native discovery; Phase 7 closes after routing, targets, CI mappings, and topology audits are implemented.

The frozen contract is in `docs/specs/test-architecture.md`; migration inventories, decisions, and
verification are in `docs/migration/2026-07-11-phase-7-test-architecture.md`.

### Phase 8: Migrate All Parser Tests to AST Dumps

Tasks:

- [x] Classify render-dependent assertions from the Phase 7 inventory across specs, extensions, APIs, fuzzing, roundtrips, CLI, and custom/historical harnesses.
- [x] Change CommonMark spec expectations from HTML to canonical AST dumps.
- [x] Change all extension spec expectations to canonical AST dumps.
- [x] Replace XML-renderer AST inspection with dumps or direct accessors.
- [x] Replace HTML/CommonMark/plaintext parser-behavior assertions with AST dumps.
- [x] Change fuzzing to parse, traverse/dump, and free, without renderer calls.
- [x] Convert roundtrip tests to AST determinism/fixtures or delete them when they no longer test parser behavior.
- [x] Delete renderer-only assertions without disguising them as AST tests.
- [x] Human-review bulk expectation migration; do not overwrite goldens from the current parser without review and create self-confirming tests.

Acceptance:

- [x] All retained parser, spec, API, and fuzz tests pass without HTML, XML, CommonMark, LaTeX, man, or plaintext renderers.
- [x] Unapproved parser drift produces actionable canonical-tree golden diffs, never hidden by normalization.

Classification, api_engine details, fixture reviews, and verification are in
`docs/migration/2026-07-12-phase-8-ast-dump-tests.md`.

### Phase 9: Remove All Rendering Functions and Implementations

Tasks:

- [x] Remove HTML rendering and `cmark_markdown_to_html`.
- [x] Remove XML rendering and verify dumps do not depend on it.
- [x] Remove CommonMark rendering.
- [x] Remove LaTeX rendering.
- [x] Remove man rendering.
- [x] Remove plaintext rendering.
- [x] Remove the generic rendering framework, headers, and unused buffers/helpers.
- [x] Remove HTML/XML/CommonMark/plaintext callbacks from syntax-extension APIs and implementations.
- [x] Remove CLI `-t/--to` formats, help text, and output routing, retaining only parsing/AST diagnostics.
- [x] Remove renderers from CMake, Make, installed headers, export maps, docs, benchmarks, and tests.
- [x] Audit sources and exported symbol tables for remaining render APIs.

Acceptance:

- [x] Clean builds compile no content renderer.
- [x] Public headers and exported symbols contain no render APIs.
- [x] CLI supports no rendering formats.
- [x] All AST-based tests still pass.

Inventories, CLI/API narrowing, artifact audits, and verification are in
`docs/migration/2026-07-12-phase-9-renderer-removal.md`.

### Phase 10: Fix Known C Engine/Facade Defects and Freeze Concurrency Contracts

This phase blocks all platform AST binding implementation. Test warmup, serialization, or binding
locks may serve only as temporary diagnostics, never as facade calling contracts or grounds for
closing confirmed C defects.

Tasks:

- [x] Create a Phase 10 defect ledger collecting every C engine/facade defect confirmed by migration reports, sanitizers, concurrency tests, and manual audits at phase start; record reproduction, root cause, impact, regressions, and closure evidence, without substituting platform workarounds.
- [x] Fix concurrent first `markdown_core_document_parse`: unsynchronized `static int registered` in `markdown_core_core_extensions_ensure_registered` lets threads race on registries, node-type counters, and flags, potentially crashing in `markdown_core_register_node_flag`.
- [x] Wrap the **entire** core-extension registration transaction in a process-level once mechanism compatible with the full platform matrix and C baseline; do not merely lock `markdown_core_register_node_flag` or require consumer warmup.
- [x] Freeze registry lifecycle: after successful registration, descriptors remain process-lifetime immutable for facade parsing; resolve `markdown_core_release_plugins`/once-state divergence so initialized state cannot reference freed registries, race with freeing, or incompletely reregister.
- [x] Audit all process-global mutable parse state, including node-type/flag allocation, extension registries, default allocators, and lazy caches; bring startup-only mutation into the same initialization boundary or prove independent thread safety.
- [x] Document concurrency and ownership in public headers: independent documents may parse/traverse/dump/free concurrently; shared-document/node reads, exclusion against free, error/object ownership, and global lifecycle must not depend on undocumented conventions.
- [x] Add native no-warmup first-parse concurrency CTests using a fresh process and a barrier releasing multiple threads through parse, extension attachment, traversal, dump, and free; verify identical results without crashes/races, plus post-initialization stress and lifecycle regressions.
- [x] Add ThreadSanitizer configuration/CI where supported, alongside ASan, UBSan, Release, shared/static, C/C++ consumers, and packages; platforms without TSan still compile and run the same native regression rather than silently skipping concurrency contracts.
- [x] Remove Swift Testing's global facade warmup after C fixes so parallel tests exercise actual first calls; do not move workarounds into production Swift, Kotlin, or ES bindings.
- [x] Add regressions and fixes for remaining ledger defects; reaudit facade failures, overflow, resource release, determinism, and cross-platform behavior; any unresolved known correctness, memory-safety, thread-safety, or lifecycle defect blocks Phase 11.
- [x] Add a Phase 10 report covering threading, portable once, registry lifecycle, diffs, reproduction, sanitizer/TSan results, platform/package audits, and removal of all workarounds.

Acceptance:

- [x] Concurrent first and subsequent facade parsing requires no warmup or external lock.
- [x] Registry initialization/freeing has no race or state divergence.
- [x] Native concurrency, supported TSan, ASan, UBSan, Release, shared/static, consumers, and packages all pass.
- [x] Every C defect confirmed at Phase 10 start is closed in its ledger.
- [x] Swift test warmup is removed; bindings depend only on public C contracts.

The ledger is `docs/migration/2026-07-12-phase-10-defect-ledger.md`; threading, portable once, registry
lifecycle, fixes, defect-sensitivity checks, and sanitizer/TSan results are in
`docs/migration/2026-07-12-phase-10-concurrency-contract.md`.

### Phase 11: Implement `swift-markdown-core`

Tasks:

- [x] Create SwiftPM product/module `MarkdownCore`.
- [x] Set `1.0.2` minimums to iOS 18/macOS 15 and configure iOS 18/26 and macOS 15/26 CI.
- [x] Implement Swift `Markup` and all immutable node types.
- [x] Implement `Document.parse`.
- [x] Implement Swift `ParseOptions`, typed Visitor, and read-only Walker.
- [x] Expose `TreeDumper.dump(markup)` and delegate every `Markup.dump()` to it.
- [x] Copy the complete C AST into a Swift value tree, then free the C document.
- [x] Add AST, Unicode, failure, ownership, and `Sendable` tests; Swift tests independently use exhaustive Visitor + Walker to produce canonical text against local focused snapshots.
- [x] Follow Phase 7: separate `MarkdownCoreTests` and `MarkdownCoreConformanceTests`; `test:swift-macos`/`test:swift-ios-simulator` run only correctness and paired `conformance:swift-*` tasks run only conformance.
- [x] Separate performance workloads; `benchmark:swift-macos` delegates to a native benchmark executable outside `test:swift-macos`.
- [x] Add an independent SwiftPM consumer test.

Acceptance:

- [x] Consumers call `Document.parse` with only `import MarkdownCore`, without exposed C handles.
- [x] All Swift-owned tests pass.
- [x] The declared iOS 18/26 and macOS 15/26 matrix passes verification.

Design, ownership, public APIs, canonical goldens, consumers, benchmarks, deployment matrix, and
acceptance are in `docs/migration/2026-07-12-phase-11-swift-binding.md`.

### Phase 12: Implement `kotlin-markdown-core`

Tasks:

- [x] Establish `commonMain`, Android, general JVM, and Native targets with the Kotlin Multiplatform plugin, freezing the publishable OS/architecture matrix.
- [x] Configure root, JVM JAR, Android AAR, and each Native publication, retaining Gradle Module Metadata.
- [x] Use `publishToMavenLocal` to freeze/document coordinates, suffixes, POMs, `.module`, sources/docs, and natives.
- [x] Implement Kotlin sealed immutable AST types.
- [x] Separate model, Visitor/Walker, and wire-codec directories; model files own only public types, and one exhaustive decoder constructs them directly from kind-specific `MKC2` payloads, removing `Any`/`WireNode` intermediate trees.
- [x] Implement the standard UTF-8 native bridge and `Document.parse`.
- [x] Implement `commonMain` `ParseOptions`, typed Visitor, and read-only Walker.
- [x] Expose `TreeDumper.dump(markup)` in `commonMain` and delegate all `Markup.dump()` calls to it.
- [x] Keep JNI wrappers in the Kotlin package, outside portable C.
- [x] Add AST, Unicode, failure, ownership, and packaging tests; Kotlin tests independently use exhaustive Visitor + Walker for canonical text against local focused snapshots.
- [x] Add KMP Gradle, JVM Gradle Module Metadata, and Android Gradle/AAR consumers; required-CI integration of real JVM Maven consumers belongs to Phase 19.
- [x] Follow Phase 7 with paired named Gradle correctness/conformance tasks per execution platform, such as `jvmTest`/`jvmConformanceTest` and `macosArm64Test`/`macosArm64ConformanceTest`; root `test:kotlin-*`/`conformance:kotlin-*` delegate directly.
- [x] Separate performance workloads; `benchmark:kotlin-jvm` delegates to Gradle benchmarks outside correctness targets.

Acceptance:

- [x] `commonMain` APIs are published and verified for Android, general JVM, and the declared Native OS/architecture matrix.
- [x] Root/target publications are complete with consistent metadata.
- [x] Gradle/KMP, JVM/Maven, and Android/AAR consumers select variants, load natives, call `Document.parse`, and pass package-owned tests.
- [x] Supported IDEs cleanly import/sync.

Implementation, coordinates, compatibility policy, CI matrix, consumers, benchmarks, and acceptance
are in `docs/migration/2026-07-12-phase-12-kotlin-binding.md`.

### Phase 13: Implement `@nouprax/es-markdown-core`

Tasks:

- [x] Implement synchronous `Document.parse(...): Document`, with WASM initialized before calls are possible.
- [x] Compile C engine/facade to WebAssembly using Emscripten or equivalent tools.
- [x] Build ES objects through direct C/WASM traversal without JSON or dump bridges.
- [x] Separate readonly models, WASM runtime, stateful exhaustive decoder, and Walker into TypeScript modules; only `tsc` generates ESM/declarations, models do not interpret WASM boundaries, and the root barrel owns no Markup field construction.
- [x] Implement recursive readonly declarations without `Object.freeze` or recursive runtime freezing.
- [x] Implement TypeScript `ParseOptions`, typed Visitor, and read-only Walker.
- [x] Hide pointers, memory, and runtime initialization details.
- [x] Expose `TreeDumper` in TypeScript, give each runtime Markup a nonenumerable `dump()`, and independently generate canonical text through exhaustive `visit` + Walker against local focused snapshots in conformance tests.
- [x] Add independent npm consumers: runtime and TypeScript consumers install real `npm pack` tarballs, with NodeNext resolving `exports.types`, never `paths` into repository build outputs.
- [x] Follow Phase 7 with `test:es-node`/`test:es-browser` correctness and separate `conformance:es-node`; names, filters, fixtures, timeouts, and diagnostics match the frozen contract.
- [x] Separate performance workloads; `benchmark:es-node` delegates to package-native benchmarks outside correctness.

Acceptance:

- [x] After WASM initialization, Node and target browsers synchronously use the sole `Document.parse` entry.
- [x] The package exposes no WASM implementation and performs no runtime freezing.
- [x] All npm package-owned tests pass.

Implementation, WASM ABI, APIs, tests/consumers, benchmarks, CI, and acceptance are in
`docs/migration/2026-07-12-phase-13-es-binding.md`.

### Phase 14: Cross-platform Conformance and Performance Baselines

Tasks:

- [x] Establish peer `conformance:<platform>` tasks that delegate only to native contract/schema targets, never implicitly included by correctness.
- [x] Compare node order, fields, nullability, Unicode, and source locations across all four platforms.
- [x] Add correctness robustness cases for large documents, deep nesting, and repeated parse/release, with separate timed large/deep workloads and no public `stress` task.
- [x] Record parsing/binding performance and memory baselines for native C, Swift copying, JNI, and WASM.
- [x] Decide whether cross-boundary optimization is needed only from benchmark evidence.

Acceptance:

- [x] C-owned tests verify parser behavior.
- [x] Each binding's correctness cases verify its code behavior.
- [x] Independent `conformance` targets verify public schema mappings.
- [x] Large/deep inputs have separate robustness assertions and timed workloads.
- [x] `benchmark` provides reproducible performance and memory baselines.

Commands, independent canonical traversal, robustness/benchmark boundaries, workloads, results, and
optimization decisions are in `docs/migration/2026-07-12-phase-14-conformance-performance.md`.

### Phase 15: Refactor Test Ownership and Shared Contract Layout

Goal: each tested package owns its tests and data; the root no longer stores cross-package test
sources or fixtures. The C parser/facade is the semantic source of truth. Binding tests verify only
mapping to that public C schema/behavior, without duplicating parser correctness responsibilities.

Tasks:

- [x] Inventory root `tests/` suites, consumers, compile contracts, fixtures, and support code, assigning one package owner to each; cross-platform use does not justify ambiguous root runners or fixtures.
- [x] Consolidate canonical Markdown/`.ast` goldens, coverage manifests, CommonMark/extension corpora, pathological/fuzz/robustness inputs, and C/C++ consumers in `packages/markdown-core/tests/`, defining/verifying only C parser/facade semantics through one CMake/CTest graph.
- [x] Move Swift AST/API/Unicode/failure/ownership/`Sendable`/robustness and SwiftPM consumers into package-owned test layouts and native manifests/suites; independent consumer tests directly depend on public `MarkdownCore`, without purposeless executable/`main.swift` intermediaries; use minimal local field/nullability/scope/error/ownership cases rather than copying the C corpus.
- [x] Consolidate Kotlin common/JVM/Android/Native tests, compile contracts, packaging, and Gradle/Maven/Android consumers under `packages/kotlin-markdown-core/`, including its nested Android runtime module, owned by KMP source sets/local consumer builds and testing only native/JNI mapping and platform delivery.
- [x] Move sibling `packages/kotlin-markdown-core-android-native/` into `packages/kotlin-markdown-core/android-runtime/` as an internal module owned solely by the Kotlin package; retain a separate module only because Android-KMP lacks `externalNativeBuild`, never presenting it as a second product/package.
- [x] Rename internal Android Gradle paths, AAR artifactId, metadata, and dependencies from `android-native` to `android-runtime`; verify the KMP Android publication uses it only as a four-ABI JNI `.so` runtime dependency and consumers still declare only public root `com.nouprax:kotlin-markdown-core`.
- [x] Keep ES Node/browser/types/conformance/ownership/robustness/packaging/npm consumers under `packages/es-markdown-core/`; runtime and type consumers install packed artifacts and resolve exports, verifying C/WASM mapping without copying C goldens.
- [x] Redefine binding conformance around the public C node-kind/field/nullability/scope/error schema, proving every mapping with local focused cases; prohibit C golden reads, C runner calls, other-binding output comparisons, or full parser corpus copies.
- [x] Delete root `tests/` and update all old paths in CMake, SwiftPM resources, Gradle inputs, ES, consumers, CI, audits, README, and migration docs.
- [x] Expose only explicit platform tasks, without cross-host aggregates or root suite/case inventories, normalization, shared fixtures, or cross-language harnesses.
- [x] Audit layout against root test sources/runners/fixtures, cross-package test/data imports, copied C parser corpora, and tests bypassing package APIs to read C dumps or another binding's output.
- [x] Run each package's complete correctness, consumers, independent conformance, robustness, and packaging from clean checkouts, then root aggregate entries, proving moves preserve native discovery, filters, diagnostics, and coverage.

Acceptance:

- [x] At Phase 15 closure, no root `tests/` or root/shared fixture remains; Phase 18's later canonical-corpus ownership transfer explicitly supersedes this conclusion.
- [x] C exclusively owns parser/facade correctness corpora and, until Phase 18, canonical goldens.
- [x] Each binding uses its own focused cases for public mappings, ownership, and packaging.
- [x] One package's native tooling owns every test, runner, consumer, and compile contract without overlapping responsibility or coverage loss.

Ownership inventories, layouts, conformance coverage, audits, and verification are in
`docs/migration/2026-07-12-phase-15-test-ownership.md`.

### Phase 16: Remove Legacy Setup and Narrow Public Surfaces

Tasks:

- [x] Remove old SwiftPM, `spm/`, `swift/`, and Android/AAR setups after new consumer coverage exists.
- [x] Remove or internalize old wrappers/bindings.
- [x] Export only immutable ASTs and diagnostic `TreeDumper`/`Markup.dump()`, with no mutation, renderers, native handles, or serialization protocols.
- [x] Audit exported symbols, headers, Swift/Kotlin APIs, and npm exports.
- [x] Clean obsolete docs, scripts, CI, and coordinates.
- [x] Retain required licenses/attribution.

Acceptance:

- [x] Clean checkouts depend only on the new monorepo setup.
- [x] Old wrappers no longer participate in builds, tests, or releases.

Removal inventories, private headers, public allowlists, dynamic-symbol audits, obsolete-reference
cleanup, and acceptance are in `docs/migration/2026-07-12-phase-16-public-surface.md`.

### Phase 17: Fix Repository Audit Findings and Freeze a Single C Library Boundary

This phase blocks release preparation. The full post-Phase-16 audit confirmed directory migration,
four-platform correctness, sanitizers, consumers, and most package checks, but found gaps in C install
metadata, CMake consumers, CI enforcement, licensing, and migrated configuration. Close them here;
do not defer fixes to release workflows or re-expose inherited `cmark-gfm` extension libraries to
bypass the single-library contract.

The audit also required revising Phase 7 routing. That task was recorded and completed in Phase 7,
which owns test architecture, rather than C package remediation. Root canonical-contract consolidation
belongs to Phase 18 and remote execution evidence to Phase 19. Neither reopens Phase 7 or duplicates
routing work here.

Tasks:

- [x] Freeze and implement one public C library, `libmarkdown-core`, for installation, linking, and discovery; core/GFM/formula/directive may use private object/static targets, but these are not separately installed/exported or listed in pkg-config/CMake, and consumers never link `libmarkdown-core-extensions`.
- [x] Fix `markdown-core.pc` to link only `-lmarkdown-core`, describe parser/immutable AST duties, and add independent pkg-config compile/link/run consumers for clean shared-only/static installs.
- [x] Provide a standard CMake config package with one reviewed public imported target and complete include usage requirements; exclude CLI/private engine/extensions/internal headers from exports, verified by an external `find_package(... CONFIG REQUIRED)` consumer.
- [x] Extend package audits with real pkg-config and CMake configure/build/link/run consumers for shared-only/static installs; filenames or symbol allowlists alone cannot pass broken metadata.
- [x] Fix cmake-format in `packages/markdown-core/extensions/CMakeLists.txt` and pass root read-only C/CMake/Swift/Kotlin/ES formatter/linter entries; final clean-checkout revalidation belongs to Phase 21.
- [x] Wire `audit-public-surface.sh` and `audit-package-contents.sh` directly into required CI, retaining topology audits and matching root `verify`, not merely declaring checks in local scripts.
- [x] Update CodeQL to actual C/C++, Java/Kotlin, JavaScript/TypeScript, and Swift products; remove source-less Python/Ruby jobs and use reproducible real builds for compiled languages.
- [x] Supply `COPYING-CMAKE-SCRIPTS` licensing referenced by `CheckFileOffsetBits.cmake`, or legally equivalent traceable inclusion in existing notices; audit source/install/release attribution completeness.
- [x] Fix repository Kotlin Project dependency notation with typed `DependencyHandler.project(String)`; use `--warning-mode=fail` to attribute remaining warnings precisely to stable AGP 9.2.1, and assign stable AGP 9.3 with the upstream fix plus warning-free prerelease acceptance to Phase 20, without preview toolchains.
- [x] Align `docs/toolchains.md` with actual JDK 26, Android compile/target SDK 37, and Gradle paths; remove nonexistent `:android:dependencies` and recheck IDE/toolchain policy.
- [x] Remove obsolete root `tests/`, legacy `android/`, `spm/`, `xml2md_gfm.xsl`, old build-phase/local Gradle-home paths from attributes/ignore/Prettier/ESLint/audit configurations; add final newlines, restrict exclusions to real generated/vendored/golden paths, and declare root Node-script ESLint globals.
- [x] Review `samples/*` in `pnpm-workspace.yaml`: remove empty globs if samples are not workspace packages, otherwise supply real verified manifests with clear ownership.
- [x] Classify inherited C source/test TODO/FIXME entries individually, fixing defects or recording owned, justified nonblocking ledger items rather than ambiguous migration notes.
- [x] Publish to isolated local Maven through the repository Wrapper, run KMP Gradle/JVM Module Metadata/Android consumers, and actually load JVM natives; real Maven smoke belongs to Phase 19 required CI.
- [x] Complete the Phase 17 report with reproductions, causes, fixes, regressions, closure evidence, and explicit Phase 18/19/20/21 boundaries for shared contracts, remote execution, release, and final physical cleanup.

Acceptance:

- [x] Installed C consumers see only one `libmarkdown-core` and public facade.
- [x] Shared-only/static pkg-config and standard CMake consumers are implemented and wired into CI.
- [x] Format/lint/audit/CodeQL configuration, licensing, toolchain docs, and Gradle warnings are resolved within this phase's scope.
- [x] Four-platform correctness, ASan/UBSan/TSan, deployment matrices, consumers, and release-content audits have reproducible entries.
- [x] Shared canonical specs belong to Phase 18, remote green CI to Phase 19, publishing to Phase 20, and Git snapshot/physical cleanup/final clean verification to Phase 21; these do not block this phase's implementation closure.

Tasks and original evidence are in `docs/migration/2026-07-12-phase-17-repo-audit-remediation.md`.

### Phase 18: Establish Shared Cross-platform Canonical AST Conformance Specs

Promote canonical Markdown/`.ast` pairs from private C fixtures to a root product contract. This phase
owns shared data and coverage manifests only, without root runners or copying parser correctness
corpora into bindings. Native `conformance:<platform>` targets continue consuming the same spec
through public parsing, immutable AST, Visitor, Walker, and TreeDumper paths.

Tasks:

- [x] Establish root `specs/canonical-ast/`, moving pairs, README, and coverage manifests; delete private C copies and prohibit binding copies.
- [x] Freeze manifest discovery, options, ordering, UTF-8/LF/final newline, and coverage schema for all 28 Markup kinds, behavior-bearing fields, null states, scope, escaping, and child order.
- [x] Make C conformance compare all shared cases through public parse/document dump APIs; C retains exclusive CommonMark/extension/regression/pathological/fuzz/robustness correctness corpora.
- [x] Make Swift macOS/iOS Simulator conformance consume shared specs through `Document.parse`, Visitor/Walker, and `TreeDumper`, removing local expected-tree literals.
- [x] Make Kotlin JVM, Android host/emulator, macOS ARM64, and Linux x64 conformance consume the same spec, removing local expected-tree literals.
- [x] Make TypeScript/ES Node conformance use public npm APIs and `TreeDumper` with shared specs, removing local literals without C dumps or other-binding outputs.
- [x] Generate simulator/emulator/native bundle resources from the same root source, without cwd dependence, out-of-bound symlinks, downloads, or manually maintained tracked copies.
- [x] Retain local Visitor-exhaustiveness, Walker-event, error/ownership/lifetime unit tests; shared specs belong only to conformance, outside correctness/benchmarks, with no new `spec:*` task.
- [x] Provide explicit golden maintenance and fail-closed audits: rewriting creates human-review diffs only; reject runners, duplicate corpora, empty discovery, unlisted cases, specs in packages, or missing platform integration.
- [x] Update Phase 5/7/8/11–17, canonical AST/dump, architecture, and ownership docs to supersede C-exclusive goldens and binding-only local snapshots.

Acceptance:

- [x] `specs/canonical-ast/` is the sole canonical AST corpus.
- [x] All four native conformance targets enumerate the same nonempty manifest and pass byte-for-byte.
- [x] Deliberately breaking mappings, Visitor dispatch, Walker hierarchy/order, scope/escaping, or TreeDumper grammar fails the corresponding platform's conformance.
- [x] Specs stay out of releases; correctness, conformance, and benchmarks remain mutually exclusive.

Implementation and acceptance are in `docs/migration/2026-07-13-phase-18-shared-canonical-ast-spec.md`.

### Phase 19: Establish Quality Gates and Nonblocking PR Observability

Tasks:

- [x] Run the full required-CI matrix and obtain green Kotlin Linux x64 and repository-managed Android emulator correctness/conformance evidence; missing hosts, simulators, browsers, or emulators fail rather than silently skip.
- [x] Obtain green required-CI public-surface/package-content/pkg-config/CMake consumer audits and CodeQL; prior phases may wire workflows, but this phase accepts remote execution results.
- [x] Add the repository-owned Maven Wrapper and minimal JVM consumer, running `verify` from the same isolated local repository and calling `Document.parse`; required CI runs this real smoke without globally installed `mvn`.
- [x] Establish unique stable `Required gates` and `CodeQL gate` aggregates, failing on failed/canceled/skipped dependencies; rulesets do not depend on changing matrix names.
- [x] Listen to both `pull_request` and `merge_group` in all blocking workflows for safe merge-queue support.
- [x] Commit an importable default-branch ruleset requiring only `Required gates`/`CodeQL gate`, excluding benchmarks, binary sizes, coverage trends, and other informational pipelines.
- [x] Import and activate the live GitHub ruleset, verify failed gates block merging and current-base policy works, and record minimal bypass ownership; committed JSON alone does not prove activation.
- [x] Add nonblocking PR metrics for C, Swift, Kotlin/JVM, and ES/WASM benchmarks and C shared-library/JVM JAR/ES-WASM sizes; missing metrics or regressions may inform but cannot alter required gates.
- [x] Implement fork-safe two-stage commenting: read-only `pull_request` runs untrusted code and uploads numeric data; privileged `workflow_run` never checks out or executes PR/artifact code, validates allowlisted numbers, and creates/updates one comment.
- [x] Audit stable names, ruleset contexts, `merge_group`, nonblocking metric boundaries, commenter permissions, and scheduled benchmark exclusion from PR gates.

Acceptance:

- [x] The active default-branch ruleset requires only `Required gates` and `CodeQL gate`.
- [x] Full correctness/conformance/sanitizer/consumer/package/security matrices fail closed on PRs and merge groups; failed/missing checks block merging.
- [x] Benchmarks and binary-size pipelines always remain nonblocking and safely update one informational comment for same-repository and fork PRs.
- [x] Privileged commenters never execute untrusted code or artifacts.

Configuration, permission boundaries, required/nonrequired inventories, and activation steps are in
`docs/migration/2026-07-13-phase-19-quality-gates.md`.

### Phase 20: Establish Release Support and CI

Tasks:

- [x] Establish lineage through npm `1.0.0` bootstrap; retain protected failed-before-publication `v1.0.1` and prepare first coordinated `1.0.2`; verify no old tags and explicit absence of a C ABI compatibility promise.
- [x] Align C, SwiftPM, Maven, and npm versions, rejecting artifact drift against root `VERSION`.
- [x] After AGP 9.3 stable releases, upgrade/reverify compatibility; cache-cold models, Android host/device tests, publications, and consumers pass `--warning-mode=fail`, proving AGP 9.2.1's upstream Project notation warning is gone, without a 9.3 preview release toolchain.
- [x] Verify SwiftPM URL, derived identity `markdown-core`, and product/module `MarkdownCore`; run plugin/conformance/product-only consumers from source archives.
- [x] Verify Maven Central `com.nouprax` ownership through `nouprax.com` DNS TXT.
- [x] Verify `com.nouprax:kotlin-markdown-core:<version>` coordinates.
- [x] Verify root, JVM, Android, and all Native publications coexist in one Central bundle with correct POM/Module Metadata references and target coordinates.
- [x] Run KMP Gradle, JVM Module Metadata, real JVM Maven through the repository Wrapper, and Android AAR consumers from staged/local Maven repositories.
- [x] Run release clean-import smoke in Android Studio Quail 2 2026.1.2 or equivalent IntelliJ IDEA 2026.1, verifying KMP source sets and IDE execution of root `allKotlinTests`; shared `All Kotlin tests` is only a platform-neutral shortcut, not a sample app; do not duplicate acceptance across IDEs using the same JetBrains Gradle/KMP importer.
- [x] Create expiring Central Portal user tokens and set `MAVEN_CENTRAL_USERNAME` / `MAVEN_CENTRAL_PASSWORD` environment secrets.
- [x] Create a passphrase-protected PGP key, publish its public key, and set `MAVEN_SIGNING_KEY` / `MAVEN_SIGNING_PASSWORD` environment secrets.
- [x] Verify Maven POMs, sources/javadoc artifacts, checksums, and required `.asc` signatures.
- [x] Verify npm organization `nouprax` publishing access and public scoped `@nouprax/es-markdown-core` configuration.
- [x] Complete npm bootstrap, bind the exact `nouprax/markdown-core` release workflow/environment as trusted publisher, and revoke bootstrap CLI sessions/tokens.
- [x] Grant npm jobs `id-token: write` / `contents: read` without traditional token reads; the registry requires 2FA and prohibits tokens, and GitHub Actions OIDC published `1.0.2` with SLSA provenance.
- [x] Create protected `release` with reviewers, tag/branch restrictions, and Maven-only secrets; the environment, `v*.*.*` tag-only policy, active tag ruleset, and four Maven secrets are enabled.
- [x] Use workflow `GITHUB_TOKEN` with `contents: write` in GitHub Release jobs only when needed.
- [x] Add credential-free release dry runs verifying contents, versions, signatures, checksums, and registry metadata.
- [x] Create `docs/releasing.md` documenting authentication, secret names, rotation, revocation, offline key backups, and compromise response.
- [x] Establish changelogs, release notes, and prerelease checks.
- [x] Run complete build/test/conformance/consumer checks from clean checkouts.
- [x] Verify artifacts contain no unexpected public headers, symbols, renderers, or runtime files.

Acceptance:

- [x] Phase 19 gates pass.
- [x] Release builds, registry staging, signatures, checksums, provenance, and dry runs are green at the same release commit.
- [x] Coordinated C, `swift-markdown-core`, `kotlin-markdown-core`, and `@nouprax/es-markdown-core` artifacts can be generated and verified.
- [x] npm OIDC, Central Portal/PGP, and minimum-permission GitHub Release flows have completed end-to-end verification.
- [x] No long-lived, undocumented, or unprotected publishing credential remains.

Implementation and acceptance are in `docs/migration/2026-07-13-phase-20-release-support.md`.

### Phase 21: Final Repository Closure and Clean-checkout Verification

Tasks:

- [x] Review all tracked deletions and untracked package sources, ensuring every deliverable is recorded and no deletion is accidental; this records the final Git/physical snapshot without reopening Phase 17 design decisions.
- [x] Before installing dependencies, remove ignored `.build/`, `build/`, `.gradle/`, `.pnpm-store/`, `.swiftpm/`, `.tools/`, `node_modules/`, and package `build/dist/.cxx` outputs; use `scripts/audit-repository.sh --physical` to reject empty directories and build/cache/dependency/IDE remnants.
- [x] Rerun clean-snapshot, secret, large-file, symlink/broken-link, mode, final-newline, coordinate, and license audits through `pnpm audit:repository:clean`; then install pinned dependencies and rerun root `verify`, headers/symbols, contents, test/CI topology, consumers, and release dry runs.
- [x] Link README to `docs/development-environment.md`, which categorizes required/platform-specific/repository-managed-or-optional dependencies and pins C/C++, CMake/Ninja/pkg-config, Xcode/Swift, JDK, Android SDK/NDK/images, Gradle/Maven Wrappers, Node.js, pnpm, Emscripten, version sources, and verification commands; provide idempotent noninteractive `scripts/init-environment.sh` with at least read-only `--check` and supported-host `--install`/bootstrap; quality/release CI reuses it after official setup actions, without installing Xcode, reading release secrets, or requiring global Gradle/Maven.
- [x] Consolidate Phase 0–20 closure, accepted exceptions, remote CI/ruleset/release evidence, and final coordinates, leaving no unowned TODOs, temporary credentials, preview toolchains, or local-only acceptance conditions.

Acceptance:

- [x] Phase 19 quality gates and Phase 20 release support are complete.
- [x] The final Git snapshot is reviewed and the physical checkout has no generated/cache/IDE remnants before dependency installation.
- [x] Unified environment tooling reproduces quality/release toolchains from a clean host.
- [x] After pinned dependency installation, repository verification, consumers, packages, security, and release dry runs are green.
- [x] README and the closure report serve as the sole entry points for new contributors and release maintainers.

Implementation and acceptance are in `docs/migration/2026-07-13-phase-21-final-closure.md`.

## 22. Settled Design Decisions

These decisions are frozen inputs for the initial implementation, no longer open setup questions:

1. npm `1.0.0` bootstrap establishes lineage; protected `v1.0.1` failed before publication and remains
   immutable; the first coordinated release is `1.0.2`. Do not migrate, retain, or archive old tags.
2. Swift supports only the latest two officially released iOS/macOS major generations, initially
   iOS 18/26 and macOS 15/26.
3. Kotlin delivers a KMP library, not Android-only AAR, with APIs in `commonMain` and initial Android,
   general JVM, and declared Native support.
4. All three `Document.parse` APIs are synchronous. ES initializes WASM before exposing the entry;
   consumers decide async scheduling.
5. Retain AST `List`; Kotlin consumers use `com.nouprax.markdown.core.List` or import aliases to resolve
   standard-collection conflicts.
6. Every `Markup` exposes complete nonoptional `Scope(start, end)`, not just start. Preserve native C
   values and semantics from the same release without binding interpretation layers.
7. Consumers pass shared immutable `ParseOptions` per call. Defaults enable smart punctuation,
   footnotes, strip HTML comments, tables, strikethrough, autolinks, task lists, formulas,
   dollar/LaTeX delimiters, and directives. Source tracking is always enabled. Renderer-only
   `unsafe`, `github-pre-lang`, and `full-info-string` flags do not enter new options.
8. All three platforms expose typed Visitors and read-only depth-first Walkers.
9. TypeScript uses recursive `readonly` without runtime freezing.
10. The facade uses length-delimited UTF-8 views and explicit errors/freeing, without cross-release
    binary ABI promises. Every release rebuilds cleanly and provides no compatibility shims.
11. Native dumps use canonical UTF-8 file-tree text with `├──`, `└──`, and `│` expressing real hierarchy.
    One line per node contains kind, strict `Scope`, all behavior-bearing fields, and child count in
    schema-defined order, never omitting defaults/null/empty states. Byte-for-byte diffs for identical
    sources/options reveal behavior drift; intentional changes require reviewed golden diffs, with
    no compatibility modes hiding differences.
12. Directive source uses `{key=value}` attribute-list grammar with bare/quoted values and no
    HTML-style `#id`/`.class` shortcuts. Parsing normalizes it to a JSON object string containing only
    string keys/values. Duplicate keys use last-wins, `id`/`class` have no special semantics, and HTML
    projection or nested/non-string JSON is prohibited. Complexity tests rule out nonlinear ordinary
    scanning and O(n²) deduplication.

If implementation audits show these decisions cannot be met, revise and review this document before
changing direction.
