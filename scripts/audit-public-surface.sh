#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

fail() {
    echo "Public surface audit failed: $1" >&2
    exit 1
}

# How many Markup kinds a visitor must cover, read from the C kind enum
# rather than written down here. Three bindings assert against this number,
# and a literal repeated three times is a number that goes stale in three
# places at once — which is what adding the reference kinds did.
markup_kind_count=$(grep -E '^    MARKDOWN_CORE_KIND_[A-Z_0-9]+' packages/markdown-core/include/markdown_core.h |
    grep -vc 'MARKDOWN_CORE_KIND_NONE')
if [ "$markup_kind_count" -lt 1 ]; then
    fail "could not read the Markup kind inventory from the C facade header"
fi

public_headers=$(find packages/markdown-core/include -maxdepth 1 -type f -print | sort)
if [ "$public_headers" != "packages/markdown-core/include/markdown_core.h" ]; then
    printf '%s\n' "$public_headers" >&2
    fail "the C package must install exactly one facade header"
fi

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

node - packages/markdown-core/include/markdown_core.h \
    packages/markdown-core/core/exports/markdown_core.map \
    packages/markdown-core/core/exports/markdown_core.exports "$temp_dir" <<'NODE'
import fs from "node:fs";
import path from "node:path";

const [, , headerPath, mapPath, exportsPath, outputDirectory] = process.argv;
const header = fs.readFileSync(headerPath, "utf8");
const map = fs.readFileSync(mapPath, "utf8");
const declared = [
    ...header.matchAll(/MARKDOWN_CORE_API[\s\S]*?\b(markdown_core_[a-z0-9_]+)\s*\(/g)
].map((match) => match[1]).sort();
const exported = [...map.matchAll(/^\s+(markdown_core_[a-z0-9_]+);$/gm)]
    .map((match) => match[1])
    .sort();
const darwinExported = fs
    .readFileSync(exportsPath, "utf8")
    .split("\n")
    .filter((line) => line.length > 0)
    .map((line) => line.replace(/^_/, ""))
    .sort();
fs.writeFileSync(path.join(outputDirectory, "declared.txt"), `${declared.join("\n")}\n`);
fs.writeFileSync(path.join(outputDirectory, "exported.txt"), `${exported.join("\n")}\n`);
fs.writeFileSync(path.join(outputDirectory, "darwin-exported.txt"), `${darwinExported.join("\n")}\n`);
if (declared.join("\n") !== exported.join("\n")) {
    throw new Error("C header declarations and the .map export allowlist differ");
}
if (declared.join("\n") !== darwinExported.join("\n")) {
    throw new Error("C header declarations and the Darwin .exports allowlist differ");
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

# The model and walker stay mutation-free. There is no exception directory any
# more: the session's append/replace surface is gone, and a document is edited
# into a successor rather than mutated. (The exclusion that used to sit here
# named a directory that no longer exists, which is a filter that quietly
# filters nothing.)
if grep -R -n -E \
    'public (func|var|let|static func).*\b(render|set[A-Z]|insert|append|prepend|replace|unlink|nativeHandle|pointer|memory|wasm)' \
    packages/swift-markdown-core/Sources/MarkdownCore; then
    fail "Swift exports mutation, renderer, or native implementation details"
fi
# The document entry points, pinned. There is no session type any more: a
# document is created from text and edited into its successor, so the surface
# that used to be MarkupSession's is Document's, and the four footnote/reference
# answer queries are gone entirely — nothing outside their own tests ever
# called them, and resolution is the consumer's to do.
document_surface=$(grep -R -h -o -E \
    'public (mutating func|consuming func|final class|struct|enum|func|var|let|init|typealias|private\(set\) var)[^{=]*' \
    packages/swift-markdown-core/Sources/MarkdownCore/Document.swift \
    packages/swift-markdown-core/Sources/MarkdownCore/Commit.swift \
    packages/swift-markdown-core/Sources/MarkdownCore/Diagnostic.swift | sed -E 's/[[:space:]]+$//' | sort -u)
expected_document_surface='public enum DiagnosticCode: Int32, Sendable, Hashable
public enum ParseErrorCode: Int32, Sendable
public final class Document: Markup, @unchecked Sendable
public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result
public func edit(_ markdown: String) throws -> Commit
public func hash(into hasher: inout Hasher)
public func node(_ id: MarkupID) -> (any Markup)?
public init(
public init(_ markdown: String, options: ParseOptions
public init(rawValue: UInt32)
public let afterRevision: UInt64
public let autolinks: Bool
public let beforeRevision: UInt64
public let code: DiagnosticCode
public let code: ParseErrorCode
public let content: [any Markup]
public let crossLinks: Bool
public let delta: Delta
public let diagnostics: [Diagnostic]
public let diffs: [Diff]
public let directives: Bool
public let document: Document
public let embeds: Bool
public let footnotes: Bool
public let formulas: Bool
public let id: MarkupID
public let markup: MarkupID
public let message: String
public let options: ParseOptions
public let parts: DiffParts
public let rawValue: UInt32
public let revision: UInt64
public let scope: Scope
public let scope: Scope?
public let smartPunctuation: Bool
public let strikethrough: Bool
public let tables: Bool
public let taskLists: Bool
public struct Commit: Sendable
public struct Delta: Sendable, Hashable
public struct Diagnostic: Sendable, Hashable
public struct Diff: Sendable, Hashable
public struct DiffParts: OptionSet, Sendable, Hashable
public struct ParseError: Error, Sendable, CustomStringConvertible
public struct ParseOptions: Sendable, Hashable
public var description: String
public var errorDescription: String?
public var isRetired: Bool
public var series: UInt64'
if [ "$document_surface" != "$expected_document_surface" ]; then
    printf '%s\n' "$document_surface" >&2
    fail "Swift document surface drifted from the reviewed pin"
fi
grep -q 'public enum MarkupDumper' packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupDumper.swift \
    && grep -q 'public static func dump(_ document: Document)' \
        packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupDumper.swift \
    && grep -q 'public func dump() -> String' \
        packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupDumper.swift \
    || fail "Swift does not expose the reviewed Document diagnostic dump API"
grep -q 'public struct TableRow: Markup' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Table.swift \
    && grep -q 'public struct TableCell: Markup' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Table.swift \
    && grep -q 'visit(_ node: TableRow)' packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupVisitor.swift \
    && grep -q 'visit(_ node: TableCell)' packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupVisitor.swift \
    || fail "Swift table rows and cells are not first-class Markup visitor nodes"
grep -q 'public struct DirectiveLabel: Markup' \
    packages/swift-markdown-core/Sources/MarkdownCore/Markup/DirectiveLabel.swift \
    && grep -q 'public let label: DirectiveLabel?' \
        packages/swift-markdown-core/Sources/MarkdownCore/Markup/Directive.swift \
    && grep -q 'public let label: DirectiveLabel?' \
        packages/swift-markdown-core/Sources/MarkdownCore/Markup/DirectiveBlock.swift \
    && grep -q 'visit(_ node: DirectiveLabel)' \
        packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupVisitor.swift \
    || fail "Swift directive labels are not first-class typed Markup edges"
if grep -R -n 'defaultVisit' packages/swift-markdown-core/Sources/MarkdownCore; then
    fail "Swift MarkupVisitor exposes a catch-all fallback"
fi
test "$(grep -c 'mutating func visit' packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupVisitor.swift)" \
    -eq "$markup_kind_count" \
    || fail "Swift MarkupVisitor is not exhaustive over all $markup_kind_count Markup kinds"
grep -q 'public func walk<V: MarkupVisitor>(' \
    packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupWalker.swift \
    && grep -q ') where V.Result == Void {' \
        packages/swift-markdown-core/Sources/MarkdownCore/Walker/MarkupWalker.swift \
    || fail "Swift MarkupWalker lacks the scope-free typed visitor overload"

grep -q 'explicitApi()' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin explicit API mode is disabled"
grep -q 'abiValidation {' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin metadata and KLIB ABI validation is disabled"
grep -q 'keepLocallyUnsupportedTargets.set(true)' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin ABI validation does not preserve unavailable Native targets"
grep -q '"checkKotlinAbi"' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin aggregate tests do not run the metadata and KLIB ABI gate"
test -s packages/kotlin-markdown-core/api/jvm/kotlin-markdown-core.api \
    || fail "Kotlin/JVM metadata ABI snapshot is missing"
test -s packages/kotlin-markdown-core/api/kotlin-markdown-core.klib.api \
    || fail "Kotlin KLIB ABI snapshot is missing"
grep -q 'visitor: MarkupVisitor<Unit>' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/MarkupWalker.kt \
    || fail "Kotlin MarkupWalker lacks the scope-free typed visitor overload"
grep -Fq '// Targets: [linuxX64, macosArm64]' \
    packages/kotlin-markdown-core/api/kotlin-markdown-core.klib.api \
    || fail "Kotlin KLIB ABI snapshot does not cover both published Native targets"
grep -q 'officialClasses' packages/kotlin-markdown-core/build.gradle.kts \
    && grep -q 'verifyJvmImplementationHidden' packages/kotlin-markdown-core/build.gradle.kts \
    && grep -q 'verifyAndroidImplementationHidden' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin/JVM ABI gates do not enforce the documented Java surface"
grep -q 'consumerKeepRules.apply' packages/kotlin-markdown-core/build.gradle.kts \
    && grep -q 'publish = true' packages/kotlin-markdown-core/build.gradle.kts \
    && grep -Fq -- '-keepclasseswithmembernames,allowoptimization class com.nouprax.markdown.core.JvmNative {' \
        packages/kotlin-markdown-core/consumer-rules.pro \
    && test "$(grep -Fc 'native <methods>;' packages/kotlin-markdown-core/consumer-rules.pro)" -eq 1 \
    || fail "Kotlin Android publication lacks the exact private-JNI consumer rule"
if grep -Eq 'class[[:space:]]+\*[[:space:]]*\{' packages/kotlin-markdown-core/consumer-rules.pro; then
    fail "Kotlin Android publication carries a broad class keep rule"
fi
grep -q 'assembleRelease' scripts/check-kotlin-consumers.sh \
    && grep -q 'assembleUnused' scripts/check-kotlin-consumers.sh \
    && grep -q 'verify-android-jni-shrinking.mjs' scripts/check-kotlin-consumers.sh \
    || fail "Kotlin Android consumer does not verify release R8/JNI linkage"
grep -q 'applyDefaultHierarchyTemplate()' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin custom JVM/Android source set bypasses the default Native hierarchy"
node - \
    packages/kotlin-markdown-core/src/native/markdown_core_kotlin.map \
    packages/kotlin-markdown-core/src/native/markdown_core_kotlin.exports \
    packages/kotlin-markdown-core/src/native/markdown_core_kotlin_jni.c \
    packages/kotlin-markdown-core/src/jvmSharedMain/kotlin/com/nouprax/markdown/core/CBridge.jvmShared.kt <<'NODE'
import fs from "node:fs";

const [, , mapPath, exportsPath, cPath, kotlinPath] = process.argv;
const inventories = new Map([
    [
        "ELF export map",
        [...fs.readFileSync(mapPath, "utf8").matchAll(
            /^\s*Java_com_nouprax_markdown_core_JvmNative_([A-Za-z0-9]+);$/gmu,
        )].map((match) => match[1]),
    ],
    [
        "Darwin exports",
        [...fs.readFileSync(exportsPath, "utf8").matchAll(
            /^_Java_com_nouprax_markdown_core_JvmNative_([A-Za-z0-9]+)$/gmu,
        )].map((match) => match[1]),
    ],
    [
        "JNI definitions",
        [...fs.readFileSync(cPath, "utf8").matchAll(
            /\bJava_com_nouprax_markdown_core_JvmNative_([A-Za-z0-9]+)\s*\(/gu,
        )].map((match) => match[1]),
    ],
    [
        "Kotlin external declarations",
        [...fs.readFileSync(kotlinPath, "utf8").matchAll(
            /\bexternal fun ([A-Za-z][A-Za-z0-9]*)\s*\(/gu,
        )].map((match) => match[1]),
    ],
]);
// The four inventories are held to EACH OTHER, and the count comes from the
// first of them. A literal count here was a fourth copy of the same fact, and
// changing the JNI surface left it stale in three files at once.
let expected;
for (const [label, names] of inventories) {
    if (names.length === 0 || new Set(names).size !== names.length) {
        throw new Error(`${label} must list at least one JNI method, each exactly once`);
    }
    const sorted = names.toSorted();
    expected ??= sorted;
    if (sorted.join("\n") !== expected.join("\n")) {
        throw new Error(
            `${label} differs from the other JvmNative inventories: ` +
                `[${sorted.join(", ")}] vs [${expected.join(", ")}]`
        );
    }
}
NODE
# The model and walker stay mutation-free. There is no exception directory any
# more: the session's append/replace surface is gone, and a document is edited
# into a successor rather than mutated.
if grep -R -n -E \
    'public (fun|val|var).*\b(render|set[A-Z]|insert|append|prepend|replace|unlink|nativeHandle|pointer|memory|wasm)' \
    packages/kotlin-markdown-core/src/commonMain; then
    fail "Kotlin exports mutation, renderer, or native implementation details"
fi
# The Kotlin public surface is pinned by kotlinx binary-compatibility-validator
# in api/jvm/kotlin-markdown-core.api and api/kotlin-markdown-core.klib.api,
# with full JVM signatures over every file and a task that regenerates them.
# A name-only grep over three of those files re-derived a strict subset of the
# same thing by hand, and cost a manual edit every time alphabetical order
# shifted under it.
grep -q 'public object MarkupDumper' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/MarkupDumper.kt \
    && grep -q 'public fun dump(document: Document): String' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/MarkupDumper.kt \
    && grep -q 'public fun dump(): String' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Document.kt \
    || fail "Kotlin does not expose the reviewed Document diagnostic dump API"
test "$(grep -c 'visitor.visit(this)' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Table.kt)" -eq 3 \
    && grep -q 'public fun visit(node: TableRow): Result' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/MarkupVisitor.kt \
    && grep -q 'public fun visit(node: TableCell): Result' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/MarkupVisitor.kt \
    || fail "Kotlin table rows and cells are not first-class Markup visitor nodes"
if grep -R -n 'defaultVisit' packages/kotlin-markdown-core/src/commonMain; then
    fail "Kotlin MarkupVisitor exposes a catch-all fallback"
fi
test "$(grep -c 'public fun visit' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/MarkupVisitor.kt)" \
    -eq "$markup_kind_count" \
    || fail "Kotlin MarkupVisitor is not exhaustive over all $markup_kind_count Markup kinds"

# A generic CHILD LIST, which is what this forbids — every kind names its own
# edges. `DiffParts.children` is a boolean saying that a node's child list
# differs, and an unanchored `children` pattern reported it as a violation.
if grep -R -E -n 'readonly children[[:space:]]*:[[:space:]]*readonly' packages/es-markdown-core/src/model; then
    fail "ES exposes a generic child list"
fi
# Every node value carries its own extent, declared once on the base every
# kind extends. A document-mediated lookup is the shape this replaced.
grep -q 'readonly scope: Scope;' packages/es-markdown-core/src/model/base.ts \
    || fail "ES node values do not carry their own scope"
if grep -R -n 'scope: (node' packages/es-markdown-core/src; then
    fail "ES resolves scopes through the document instead of off the node"
fi
grep -q 'static dump(document: Document, node: Markup = document)' packages/es-markdown-core/src/markup-dumper.ts \
    && grep -q 'readonly dump: () => string' packages/es-markdown-core/src/model/document.ts \
    || fail "ES does not expose the reviewed Document diagnostic dump API"
# The document entry points, pinned. There is no session type any more: a
# document is created from text and edited into its successor, and the
# footnote/reference answers are gone entirely.
es_document_surface=$(
    {
        grep -h -E '^export (function|const|type|interface) [A-Za-z]+' \
            packages/es-markdown-core/src/document.ts \
            packages/es-markdown-core/src/model/document.ts \
            packages/es-markdown-core/src/model/commit.ts \
            packages/es-markdown-core/src/model/diagnostic.ts
        grep -h -E '^    readonly [a-zA-Z]+' \
            packages/es-markdown-core/src/model/document.ts \
            packages/es-markdown-core/src/model/commit.ts \
            packages/es-markdown-core/src/model/diagnostic.ts
    } | sed -E 's/;$//; s/^    //' | sort -u
)
expected_es_document_surface='export const DiagnosticCode = {
export function Document(markdown: string, options: ParseOptions = {}): Document {
export interface Commit {
export interface Delta {
export interface Diagnostic {
export interface Diff {
export interface DiffParts {
export interface Document extends MarkupBase<"document"> {
export type DiagnosticCode = (typeof DiagnosticCode)[keyof typeof DiagnosticCode]
export type Document = DocumentValue
readonly afterRevision: number
readonly beforeRevision: number
readonly children: boolean
readonly close: () => void
readonly code: DiagnosticCode
readonly content: readonly Markup[]
readonly delta: Delta
readonly descendant: boolean
readonly diagnostics: readonly Diagnostic[]
readonly diffs: readonly Diff[]
readonly document: Document
readonly dump: () => string
readonly edit: (markdown: string) => Commit
readonly markup: MarkupID
readonly node: (id: Markup["id"]) => Markup | null
readonly options: Readonly<Required<ParseOptions>>
readonly parts: DiffParts
readonly retired: boolean
readonly scope: Scope
readonly text: boolean
readonly value: boolean'
if [ "$es_document_surface" != "$expected_es_document_surface" ]; then
    printf '%s\n' "$es_document_surface" >&2
    fail "ES document surface drifted from the reviewed pin"
fi
grep -q 'TableRow extends MarkupBase<"tableRow">' packages/es-markdown-core/src/model/table.ts \
    && grep -q 'TableCell extends MarkupBase<"tableCell">' packages/es-markdown-core/src/model/table.ts \
    && grep -q 'visitTableRow(this:' packages/es-markdown-core/src/markup-visitor.ts \
    && grep -q 'visitTableCell(this:' packages/es-markdown-core/src/markup-visitor.ts \
    || fail "ES table rows and cells are not first-class Markup visitor nodes"
if grep -R -E -n 'defaultVisit|visit[A-Z][A-Za-z]+\?' packages/es-markdown-core/src; then
    fail "ES MarkupVisitor exposes a catch-all or optional typed handlers"
fi
test "$(grep -c '^    visit[A-Z].*(this:' packages/es-markdown-core/src/markup-visitor.ts)" \
    -eq "$markup_kind_count" \
    || fail "ES MarkupVisitor is not exhaustive over all $markup_kind_count Markup kinds"
grep -q 'walk(document: Document, visitor: MarkupVisitor<void>): void;' \
    packages/es-markdown-core/src/markup-walker.ts \
    || fail "ES MarkupWalker lacks the scope-free typed visitor overload"

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
    ...[...runtime.matchAll(/^export (?:class|const|function) ([A-Za-z0-9_]+)/gm)].map(
        (match) => match[1]
    ),
    ...[...runtime.matchAll(/^export \{ ([^}]+) \} from /gm)].flatMap((match) =>
        match[1].split(",").map((name) => name.trim())
    )
].sort();
const expectedRuntime = ["DiagnosticCode", "Document", "MarkupDumper", "MarkupWalker", "ParseError", "WalkEvent", "visit"].sort();
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
