import { mkdir, readFile, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import path from "node:path";

const packageDirectory = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const repositoryRoot = path.resolve(packageDirectory, "../..");
const specDirectory = path.join(repositoryRoot, "specs/canonical-ast");
const output = path.join(packageDirectory, "build/generated/conformance/canonical-ast-fixtures.json");

const manifest = JSON.parse(await readFile(path.join(specDirectory, "manifest.json"), "utf8"));
if (manifest.schemaVersion !== 1 || !Array.isArray(manifest.cases) || manifest.cases.length === 0) {
    throw new Error("shared canonical AST manifest v1 must contain at least one case");
}

async function readFixture(relativePath) {
    if (typeof relativePath !== "string" || path.isAbsolute(relativePath)) {
        throw new Error(`canonical AST fixture path must be relative: ${relativePath}`);
    }
    const resolved = path.resolve(specDirectory, relativePath);
    const relative = path.relative(specDirectory, resolved);
    if (relative.startsWith("..") || path.isAbsolute(relative)) {
        throw new Error(`canonical AST fixture path escapes the spec directory: ${relativePath}`);
    }
    return readFile(resolved, "utf8");
}

const cases = await Promise.all(
    manifest.cases.map(async (testCase) => ({
        name: testCase.name,
        source: await readFixture(testCase.input),
        expected: await readFixture(testCase.expected),
        parseOptions: testCase.parseOptions
    }))
);
const generated = `${JSON.stringify({ schemaVersion: 1, cases }, null, 2)}\n`;

let current;
try {
    current = await readFile(output, "utf8");
} catch (error) {
    if (error?.code !== "ENOENT") throw error;
}
if (current !== generated) {
    await mkdir(path.dirname(output), { recursive: true });
    await writeFile(output, generated, "utf8");
}
