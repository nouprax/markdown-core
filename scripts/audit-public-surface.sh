#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT_DIR"

fail() {
    echo "Public surface audit failed: $1" >&2
    exit 1
}

for retired_root in \
    .travis.yml appveyor.yml Makefile.nmake nmake.bat toolchain-mingw32.cmake \
    suppressions android spm swift wrappers man data; do
    if git ls-files "$retired_root" | while IFS= read -r path; do
        [ -e "$path" ] && printf '%s\n' "$path"
    done | grep -q .; then
        fail "retired root path is still tracked: $retired_root"
    fi
done

for source_path in \
    packages/markdown-core/include/markdown_core.h \
    packages/swift-markdown-core/Sources/MarkdownCore \
    packages/kotlin-markdown-core/src/commonMain \
    packages/es-markdown-core/src; do
    if git check-ignore -q "$source_path"; then
        fail "source path is hidden by .gitignore: $source_path"
    fi
done

legacy_paths=$(git ls-files android spm swift wrappers | while IFS= read -r path; do
    if [ -e "$path" ]; then
        printf '%s\n' "$path"
    fi
done)
if [ -n "$legacy_paths" ]; then
    printf '%s\n' "$legacy_paths" >&2
    fail "retired setup or wrapper files are still tracked"
fi

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
if (!manifest.targets.some((target) => target.name === "MarkdownCoreC")) {
    throw new Error("SwiftPM internal C target is missing");
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
grep -q 'public struct ListItem' packages/swift-markdown-core/Sources/MarkdownCore/Markup/List.swift \
    || fail "Swift List.swift does not own ListItem"
test ! -e packages/swift-markdown-core/Sources/MarkdownCore/Markup/ListItem.swift \
    || fail "Swift ListItem must not have a separate model file"
grep -q 'public struct FootnoteDefinition' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Footnote.swift \
    && grep -q 'public struct FootnoteReference' packages/swift-markdown-core/Sources/MarkdownCore/Markup/Footnote.swift \
    || fail "Swift Footnote.swift does not own both footnote node types"
test ! -e packages/swift-markdown-core/Sources/MarkdownCore/Markup/FootnoteDefinition.swift \
    && test ! -e packages/swift-markdown-core/Sources/MarkdownCore/Markup/FootnoteReference.swift \
    || fail "Swift footnote nodes must not have separate model files"
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
for concept in Code CodeBlock Formula FormulaBlock HTML HTMLBlock Directive DirectiveBlock; do
    test -f "packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/$concept.kt" \
        || fail "Kotlin concept-local model source is missing: $concept"
done
grep -q 'public class ListItem' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/List.kt \
    || fail "Kotlin List.kt does not own ListItem"
test ! -e packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/ListItem.kt \
    || fail "Kotlin ListItem must not have a separate model file"
grep -q 'public class FootnoteDefinition' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Footnote.kt \
    && grep -q 'public class FootnoteReference' \
        packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/Footnote.kt \
    || fail "Kotlin Footnote.kt does not own both footnote node types"
test ! -e packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/FootnoteDefinition.kt \
    && test ! -e packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model/FootnoteReference.kt \
    || fail "Kotlin footnote nodes must not have separate model files"
if grep -R -n -E '\bWireNode\b|fun node\(\): Any' \
    packages/kotlin-markdown-core/src/commonMain; then
    fail "Kotlin constructs a generic intermediate wire tree"
fi
if grep -R -n '\bWireReader\b' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/model; then
    fail "Kotlin model files interpret the private wire protocol"
fi
if grep -R -n -E '\.ordinal\b|entries\.firstOrNull' \
    packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/wire; then
    fail "Kotlin wire decoding relies on enum order or linear kind lookup"
fi
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
test "$(grep -c 'public fun visit' packages/kotlin-markdown-core/src/commonMain/kotlin/com/nouprax/markdown/core/walker/Visitor.kt)" -eq 28 \
    || fail "Kotlin Visitor is not exhaustive over all 28 Markup kinds"

for concept in code code-block formula formula-block html html-block directive directive-block; do
    test -f "packages/es-markdown-core/src/model/$concept.ts" \
        || fail "ES concept-local model source is missing: $concept"
done
grep -q 'export interface ListItem' packages/es-markdown-core/src/model/list.ts \
    || fail "ES list.ts does not own ListItem"
test ! -e packages/es-markdown-core/src/model/list-item.ts \
    || fail "ES ListItem must not have a separate model module"
grep -q 'export interface FootnoteDefinition' packages/es-markdown-core/src/model/footnote.ts \
    && grep -q 'export interface FootnoteReference' packages/es-markdown-core/src/model/footnote.ts \
    || fail "ES footnote.ts does not own both footnote node types"
test ! -e packages/es-markdown-core/src/model/footnote-definition.ts \
    && test ! -e packages/es-markdown-core/src/model/footnote-reference.ts \
    || fail "ES footnote nodes must not have separate model modules"
if find packages/es-markdown-core/src -type f \( -name '*.js' -o -name '*.d.ts' \) | grep -q .; then
    fail "ES maintained source must be TypeScript, not handwritten JavaScript/declarations"
fi
if grep -R -E -n '\bDecodeContext\b|export function decode[A-Z]|\bes_node_' \
    packages/es-markdown-core/src/model; then
    fail "ES public model files interpret the private WASM boundary"
fi
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
const expectedRuntime = ["Document", "ParseError", "TreeDumper", "WalkEvent", "Walker", "visit"].sort();
if (runtimeExports.join("\n") !== expectedRuntime.join("\n")) {
    throw new Error(`Unexpected ES runtime exports: ${runtimeExports.join(", ")}`);
}
NODE

if grep -q '"paths"' packages/es-markdown-core/tests/types/tsconfig.json \
    || grep -R -n -E '(\.\./)+dist/index\.d\.ts' packages/es-markdown-core/tests/types; then
    fail "ES type consumer bypasses installed-package exports.types resolution"
fi

if grep -q 'include(":android")' settings.gradle.kts; then
    fail "the retired root Android/Prefab project is still registered"
fi
grep -q '^group = "com.nouprax"$' packages/kotlin-markdown-core/build.gradle.kts \
    || fail "Kotlin group coordinate drifted"
grep -q 'artifactId = "kotlin-markdown-core-android-runtime"' \
    packages/kotlin-markdown-core/android-runtime/build.gradle.kts \
    || fail "internal Android runtime coordinate drifted"

cmp LICENSE packages/es-markdown-core/LICENSE >/dev/null \
    || fail "npm package license attribution differs from the repository license"

echo "Public surface audit passed."
