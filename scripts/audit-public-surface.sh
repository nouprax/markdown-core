#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

fail() {
    echo "Public surface audit failed: $1" >&2
    exit 1
}

public_headers=$(find packages/markdown-core/include -maxdepth 1 -type f -print | sort)
if [ "$public_headers" != "packages/markdown-core/include/markdown_core.h" ]; then
    printf '%s\n' "$public_headers" >&2
    fail "the C package must install exactly one facade header"
fi

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

node - packages/markdown-core/include/markdown_core.h \
    packages/markdown-core/core/exports/markdown_core.map "$temp_dir" <<'NODE'
import fs from "node:fs";
import path from "node:path";

const [, , headerPath, mapPath, outputDirectory] = process.argv;
const header = fs.readFileSync(headerPath, "utf8");
const map = fs.readFileSync(mapPath, "utf8");
const declared = [
    ...header.matchAll(/MARKDOWN_CORE_API[\s\S]*?\b(markdown_core_[a-z0-9_]+)\s*\(/g)
].map((match) => match[1]).sort();
const exported = [...map.matchAll(/^\s+(markdown_core_[a-z0-9_]+);$/gm)]
    .map((match) => match[1])
    .sort();
fs.writeFileSync(path.join(outputDirectory, "declared.txt"), `${declared.join("\n")}\n`);
fs.writeFileSync(path.join(outputDirectory, "exported.txt"), `${exported.join("\n")}\n`);
if (declared.join("\n") !== exported.join("\n")) {
    throw new Error("C header declarations and export allowlist differ");
}
for (const symbol of declared) {
    if (/_(set|insert|append|prepend|replace|unlink|new|render)_/.test(symbol)) {
        throw new Error(`Mutating or rendering C symbol is public: ${symbol}`);
    }
}
NODE

CLANG_MODULE_CACHE_PATH="$temp_dir/swift-module-cache" \
    swift package --disable-sandbox dump-package >"$temp_dir/swift-package.json"
node - "$temp_dir/swift-package.json" <<'NODE'
import fs from "node:fs";

const manifest = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const products = manifest.products.map((product) => `${product.name}:${product.targets.join(",")}`);
if (products.join("\n") !== "MarkdownCore:MarkdownCore") {
    throw new Error(`Unexpected SwiftPM products: ${products.join(", ")}`);
}
NODE

# The model and walker stay mutation-free; the session directory is the one
# deliberate exception (append/replace are its reviewed edit surface) and is
# pinned exactly below instead.
if grep -R -n -E \
    'public (func|var|let|static func).*\b(render|set[A-Z]|insert|append|prepend|replace|unlink|nativeHandle|pointer|memory|wasm)' \
    --exclude-dir=Session \
    packages/swift-markdown-core/Sources/MarkdownCore; then
    fail "Swift exports mutation, renderer, or native implementation details"
fi
session_surface=$(grep -R -h -o -E \
    'public (mutating func|final class|struct|func|var|let|init|typealias|private\(set\) var)[^{=]*' \
    packages/swift-markdown-core/Sources/MarkdownCore/Session | sed -E 's/[[:space:]]+$//' | sort -u)
expected_session_surface='public final class MarkupSession
public func append(_ text: String) throws
public func commit() throws -> Commit
public func footnoteInfo(of id: MarkupID) -> FootnoteInfo?
public func footnoteReferences(of definition: MarkupID) -> [FootnoteReference]
public func footnotes() -> [FootnoteDefinition]
public func makeAsyncIterator() -> Updates<Tokens>
public func node(for id: MarkupID) -> (any Markup)?
public func replace(_ range: Range<Int>, with text: String) throws
public func scope(of node: some Markup) -> Scope
public func updates<Tokens: AsyncSequence>(
public init(options: ParseOptions
public let added: [MarkupID]
public let afterRevision: UInt64
public let beforeRevision: UInt64
public let bubbled: [MarkupID]
public let changed: [MarkupID]
public let changes: Delta
public let definition: MarkupID?
public let document: Document
public let lineage: UInt64
public let number: Int?
public let options: ParseOptions
public let referenceCount: Int
public let referenceOrdinal: Int?
public let removed: [MarkupID]
public mutating func next() async throws -> Commit?
public private(set) var document: Document
public struct Commit: Sendable
public struct Delta: Sendable, Hashable
public struct FootnoteInfo: Sendable, Hashable
public struct Updates<Tokens: AsyncSequence>: AsyncSequence, AsyncIteratorProtocol
public var length: Int
public var revision: UInt64'
if [ "$session_surface" != "$expected_session_surface" ]; then
    printf '%s\n' "$session_surface" >&2
    fail "Swift session surface drifted from the reviewed pin"
fi
grep -q 'public enum TreeDumper' packages/swift-markdown-core/Sources/MarkdownCore/Walker/TreeDumper.swift \
    && grep -q 'public static func dump(_ document: Document)' \
        packages/swift-markdown-core/Sources/MarkdownCore/Walker/TreeDumper.swift \
    && grep -q 'public func dump() -> String' \
        packages/swift-markdown-core/Sources/MarkdownCore/Walker/TreeDumper.swift \
    || fail "Swift does not expose the reviewed Document diagnostic dump API"
grep -q 'public struct TableRow: Markup' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Table.swift \
    && grep -q 'public struct TableCell: Markup' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Table.swift \
    && grep -q 'visit(_ node: TableRow)' packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupVisitor.swift \
    && grep -q 'visit(_ node: TableCell)' packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupVisitor.swift \
    || fail "Swift table rows and cells are not first-class Markup visitor nodes"
if grep -R -n 'defaultVisit' packages/swift-markdown-core/Sources/MarkdownCore; then
    fail "Swift MarkupVisitor exposes a catch-all fallback"
fi
test "$(grep -c 'mutating func visit' packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupVisitor.swift)" -eq 28 \
    || fail "Swift MarkupVisitor is not exhaustive over all 28 Markup kinds"

grep -q 'explicitApi()' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin explicit API mode is disabled"
# The model and walker stay mutation-free; the session directory is the one
# deliberate exception (append/replace are its reviewed edit surface) and is
# pinned exactly below instead.
if grep -R -n -E \
    'public (fun|val|var).*\b(render|set[A-Z]|insert|append|prepend|replace|unlink|nativeHandle|pointer|memory|wasm)' \
    --exclude-dir=session \
    packages/kotlin-markdown-core/src/commonMain; then
    fail "Kotlin exports mutation, renderer, or native implementation details"
fi
kotlin_session_surface=$(grep -R -h -o -E \
    'public (fun|val|var|class|object) [A-Za-z.]+' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/session | sort -u)
expected_kotlin_session_surface='public class Commit
public class Delta
public class FootnoteInfo
public class MarkupSession
public fun MarkupSession.footnoteInfo
public fun MarkupSession.footnoteReferences
public fun MarkupSession.footnotes
public fun append
public fun commit
public fun node
public fun replace
public fun updates
public val added
public val afterRevision
public val beforeRevision
public val bubbled
public val changed
public val changes
public val definition
public val document
public val length
public val lineage
public val number
public val options
public val referenceCount
public val referenceOrdinal
public val removed
public val revision
public var document'
if [ "$kotlin_session_surface" != "$expected_kotlin_session_surface" ]; then
    printf '%s\n' "$kotlin_session_surface" >&2
    fail "Kotlin session surface drifted from the reviewed pin"
fi
grep -q 'public object TreeDumper' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/TreeDumper.kt \
    && grep -q 'public fun dump(document: Document): String' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/TreeDumper.kt \
    && grep -q 'public fun dump(): String' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Document.kt \
    || fail "Kotlin does not expose the reviewed Document diagnostic dump API"
grep -q 'visitor.visitTableRow(this)' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Table.kt \
    && grep -q 'visitor.visitTableCell(this)' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Table.kt \
    && grep -q 'visitTableRow' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/Visitor.kt \
    && grep -q 'visitTableCell' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/Visitor.kt \
    || fail "Kotlin table rows and cells are not first-class Markup visitor nodes"
if grep -R -n 'defaultVisit' packages/kotlin-markdown-core/src/commonMain; then
    fail "Kotlin Visitor exposes a catch-all fallback"
fi
test "$(grep -c 'public fun visit' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/Visitor.kt)" -eq 28 \
    || fail "Kotlin Visitor is not exhaustive over all 28 Markup kinds"

if grep -R -E -n 'readonly children' packages/es-markdown-core/src/model; then
    fail "ES exposes generic children"
fi
# Node values carry identity, never positions; scopes are document-mediated.
if grep -R -n 'readonly scope: Scope' packages/es-markdown-core/src/model; then
    fail "ES node values store scopes"
fi
grep -q 'static dump(document: Document, node: Markup = document)' packages/es-markdown-core/src/tree-dumper.ts \
    && grep -q 'readonly scope: (node: Markup) => Scope' packages/es-markdown-core/src/model/document.ts \
    && grep -q 'readonly dump: () => string' packages/es-markdown-core/src/model/document.ts \
    || fail "ES does not expose the reviewed Document diagnostic dump API"
es_session_surface=$(
    {
        grep -R -h -E '^export (class|interface|function) [A-Za-z]+' packages/es-markdown-core/src/session
        grep -h -E '^    [a-zA-Z].*[{;]$' \
            packages/es-markdown-core/src/session/markup-session.ts \
            packages/es-markdown-core/src/session/commit.ts \
            packages/es-markdown-core/src/session/footnote-info.ts \
            | grep -v '^    private '
    } | sed -E 's/ \{$//; s/;$//; s/^    //' | sort -u
)
expected_es_session_surface='append(text: string): void
async *updates(input: AsyncIterable<string> | Iterable<string>): AsyncIterableIterator<Commit>
close(): void
commit(): Commit
constructor(options: ParseOptions = {})
export class MarkupSession
export class ScopeResolver
export function adoptDocument(value: DocumentValue, resolver: ScopeResolver): Document
export interface Commit
export interface Delta
export interface FootnoteInfo
export interface ScopeEntry
footnoteInfo(id: MarkupID): FootnoteInfo | null
footnoteReferences(definition: MarkupID): FootnoteReference[]
footnotes(): FootnoteDefinition[]
get document(): Document
get length(): number
get revision(): number
node(id: MarkupID): Markup | null
readonly added: readonly MarkupID[]
readonly afterRevision: number
readonly beforeRevision: number
readonly bubbled: readonly MarkupID[]
readonly changed: readonly MarkupID[]
readonly changes: Delta
readonly definition: MarkupID | null
readonly document: Document
readonly lineage: bigint
readonly number: number | null
readonly options: Readonly<Required<ParseOptions>>
readonly referenceCount: number
readonly referenceOrdinal: number | null
readonly removed: readonly MarkupID[]
replace(byteStart: number, byteEnd: number, replacement: string): void'
if [ "$es_session_surface" != "$expected_es_session_surface" ]; then
    printf '%s\n' "$es_session_surface" >&2
    fail "ES session surface drifted from the reviewed pin"
fi
grep -q 'TableRow extends MarkupBase<"tableRow">' packages/es-markdown-core/src/model/table.ts \
    && grep -q 'TableCell extends MarkupBase<"tableCell">' packages/es-markdown-core/src/model/table.ts \
    && grep -q 'visitTableRow(this:' packages/es-markdown-core/src/visitor.ts \
    && grep -q 'visitTableCell(this:' packages/es-markdown-core/src/visitor.ts \
    || fail "ES table rows and cells are not first-class Markup visitor nodes"
if grep -R -E -n 'defaultVisit|visit[A-Z][A-Za-z]+\?' packages/es-markdown-core/src; then
    fail "ES Visitor exposes a catch-all or optional typed handlers"
fi
test "$(grep -c '^    visit[A-Z].*(this:' packages/es-markdown-core/src/visitor.ts)" -eq 28 \
    || fail "ES Visitor is not exhaustive over all 28 Markup kinds"

node - packages/es-markdown-core/package.json packages/es-markdown-core/src/index.ts <<'NODE'
import fs from "node:fs";

const [, , manifestPath, runtimePath] = process.argv;
const manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
const rootExport = manifest.exports?.["."];
const exportKeys = Object.keys(manifest.exports ?? {}).sort();
if (exportKeys.join("\n") !== ".\n./markdown-core.wasm") {
    throw new Error(`Unexpected npm export paths: ${exportKeys.join(", ")}`);
}
if (rootExport?.types !== "./dist/index.d.ts" || rootExport?.import !== "./dist/index.js") {
    throw new Error("npm root export does not point at the reviewed ESM and declaration files");
}
const runtime = fs.readFileSync(runtimePath, "utf8");
const runtimeExports = [
    ...[...runtime.matchAll(/^export (?:class|const) ([A-Za-z0-9_]+)/gm)].map(
        (match) => match[1]
    ),
    ...[...runtime.matchAll(/^export \{ ([^}]+) \} from /gm)].flatMap((match) =>
        match[1].split(",").map((name) => name.trim())
    )
].sort();
const expectedRuntime = ["Document", "MarkupSession", "ParseError", "TreeDumper", "WalkEvent", "Walker", "visit"].sort();
if (runtimeExports.join("\n") !== expectedRuntime.join("\n")) {
    throw new Error(`Unexpected ES runtime exports: ${runtimeExports.join(", ")}`);
}
NODE

if grep -q '"paths"' packages/es-markdown-core/tests/types/tsconfig.json \
    || grep -R -n -E '(\.\./)+dist/index\.d\.ts' packages/es-markdown-core/tests/types; then
    fail "ES type consumer bypasses installed-package exports.types resolution"
fi

grep -q '^group = "com.nouprax"$' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin group coordinate drifted"
grep -q 'artifactId = "kotlin-markdown-core-android-runtime"' \
    packages/kotlin-markdown-core/android-runtime/build.gradle.kts \
    || fail "internal Android runtime coordinate drifted"

cmp LICENSE packages/es-markdown-core/LICENSE >/dev/null \
    || fail "npm package license attribution differs from the repository license"

echo "Public surface audit passed."
