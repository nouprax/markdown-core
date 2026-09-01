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
    packages/markdown-core/core/exports/markdown_core.map \
    packages/markdown-core/core/exports/markdown_core.exports "$temp_dir" <<'NODE'
import fs from "node:fs";
import path from "node:path";

const [, , headerPath, mapPath, machOPath, outputDirectory] = process.argv;
const header = fs.readFileSync(headerPath, "utf8");
const map = fs.readFileSync(mapPath, "utf8");
// THE MACH-O LIST IS THE ONE THAT LINKS ON macOS, and until Step 13 nothing
// compared it with anything: a symbol added to the header and the ELF version
// script and forgotten here left this audit green and the macOS shared library
// short one symbol, which surfaces as a link error in a binding build.
const machO = fs.readFileSync(machOPath, "utf8");
const declared = [
    ...header.matchAll(/MARKDOWN_CORE_API[\s\S]*?\b(markdown_core_[a-z0-9_]+)\s*\(/g)
].map((match) => match[1]).sort();
const exported = [...map.matchAll(/^\s+(markdown_core_[a-z0-9_]+);$/gm)]
    .map((match) => match[1])
    .sort();
const machOExported = [...machO.matchAll(/^_(markdown_core_[a-z0-9_]+)$/gm)]
    .map((match) => match[1])
    .sort();
fs.writeFileSync(path.join(outputDirectory, "declared.txt"), `${declared.join("\n")}\n`);
fs.writeFileSync(path.join(outputDirectory, "exported.txt"), `${exported.join("\n")}\n`);
if (declared.join("\n") !== exported.join("\n")) {
    throw new Error("C header declarations and export allowlist differ");
}
if (declared.join("\n") !== machOExported.join("\n")) {
    throw new Error("C header declarations and the Mach-O export list differ");
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

if grep -R -n -E \
    'public (func|var|let|static func).*\b(render|set[A-Z]|insert|append|prepend|replace|unlink|nativeHandle|pointer|memory|wasm)' \
    packages/swift-markdown-core/Sources/MarkdownCore; then
    fail "Swift exports mutation, renderer, or native implementation details"
fi
grep -q 'public enum TreeDumper' packages/swift-markdown-core/Sources/MarkdownCore/Walker/TreeDumper.swift \
    && grep -q 'public static func dump' packages/swift-markdown-core/Sources/MarkdownCore/Walker/TreeDumper.swift \
    && grep -q 'func dump() -> String' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Markup.swift \
    || fail "Swift does not expose the reviewed Markup diagnostic dump API"
grep -q 'public struct TableRow: Markup' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Table.swift \
    && grep -q 'public struct TableCell: Markup' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Table.swift \
    && grep -q 'visit(_ node: TableRow)' packages/swift-markdown-core/Sources/MarkdownCore/Walker/Visitor.swift \
    && grep -q 'visit(_ node: TableCell)' packages/swift-markdown-core/Sources/MarkdownCore/Walker/Visitor.swift \
    || fail "Swift table rows and cells are not first-class Markup visitor nodes"
# The kind count is the CONTRACT's, not a number written here. It was 28 in
# three places until Step 7 added a 29th kind and all three said the same wrong
# thing at once.
kind_count=$(node -e 'process.stdout.write(String(JSON.parse(require("node:fs").readFileSync("docs/specs/canonical-ast.json", "utf8")).kinds.length))')

if grep -R -n 'defaultVisit' packages/swift-markdown-core/Sources/MarkdownCore; then
    fail "Swift Visitor exposes a catch-all fallback"
fi
test "$(grep -c 'mutating func visit' packages/swift-markdown-core/Sources/MarkdownCore/Walker/Visitor.swift)" -eq "$kind_count" \
    || fail "Swift Visitor is not exhaustive over all $kind_count Markup kinds"

grep -q 'explicitApi()' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin explicit API mode is disabled"
if grep -R -n -E \
    'public (fun|val|var).*\b(render|set[A-Z]|insert|append|prepend|replace|unlink|nativeHandle|pointer|memory|wasm)' \
    packages/kotlin-markdown-core/src/commonMain; then
    fail "Kotlin exports mutation, renderer, or native implementation details"
fi
grep -q 'public object TreeDumper' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/TreeDumper.kt \
    && grep -q 'public fun dump(root: Markup): String' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/TreeDumper.kt \
    && grep -q 'public fun dump(): String' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Markup.kt \
    || fail "Kotlin does not expose the reviewed Markup diagnostic dump API"
grep -q 'visitor.visitTableRow(this)' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Table.kt \
    && grep -q 'visitor.visitTableCell(this)' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Table.kt \
    && grep -q 'visitTableRow' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/Visitor.kt \
    && grep -q 'visitTableCell' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/Visitor.kt \
    || fail "Kotlin table rows and cells are not first-class Markup visitor nodes"
if grep -R -n 'defaultVisit' packages/kotlin-markdown-core/src/commonMain; then
    fail "Kotlin Visitor exposes a catch-all fallback"
fi
test "$(grep -c 'public fun visit' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/Visitor.kt)" -eq "$kind_count" \
    || fail "Kotlin Visitor is not exhaustive over all $kind_count Markup kinds"

if grep -R -E -n 'readonly children' packages/es-markdown-core/src/model; then
    fail "ES exposes generic children"
fi
grep -q 'TableRow extends MarkupBase<"tableRow">' packages/es-markdown-core/src/model/table.ts \
    && grep -q 'TableCell extends MarkupBase<"tableCell">' packages/es-markdown-core/src/model/table.ts \
    && grep -q 'visitTableRow(this:' packages/es-markdown-core/src/visitor.ts \
    && grep -q 'visitTableCell(this:' packages/es-markdown-core/src/visitor.ts \
    || fail "ES table rows and cells are not first-class Markup visitor nodes"
if grep -R -E -n 'defaultVisit|visit[A-Z][A-Za-z]+\?' packages/es-markdown-core/src; then
    fail "ES Visitor exposes a catch-all or optional typed handlers"
fi
test "$(grep -c '^    visit[A-Z].*(this:' packages/es-markdown-core/src/visitor.ts)" -eq "$kind_count" \
    || fail "ES Visitor is not exhaustive over all $kind_count Markup kinds"

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
// `RegionRole` left the runtime list with the regions when 11a-11c were
// retired; `Concrete` left it with the concrete view (D8). `Session` joined
// at T14 (docs/STREAMING.md D5) and became the living `Document` when the
// 3.0 names were formalized: the stream's one handle, beside the `Read`
// values it returns (`Read` and `Semantic` are types, not runtime exports).
const expectedRuntime = [
    "Document",
    "ParseError",
    "TreeDumper",
    "WalkEvent",
    "Walker",
    "visit"
].sort();
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
