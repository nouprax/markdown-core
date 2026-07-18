import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const packageDirectory = path.resolve(fileURLToPath(new URL("..", import.meta.url)));

const correctnessSuites = [
    "api",
    "ast",
    "consumer",
    "errors",
    "ownership",
    "robustness",
    "sessions",
    "unicode",
    "types",
    "packaging"
];
const targetIndex = process.argv.indexOf("--target");
const suiteIndex = process.argv.indexOf("--suite");
const target = targetIndex >= 0 ? process.argv[targetIndex + 1] : "node";
const requested = suiteIndex >= 0 ? process.argv[suiteIndex + 1] : null;
const targets = { node: correctnessSuites, browser: ["api"] };
const suites = targets[target];
if (!suites) throw new Error(`unsupported correctness target: ${target}`);
if (process.argv.includes("--list")) {
    process.stdout.write(`${suites.join("\n")}\n`);
    process.exit(0);
}
if (requested && !suites.includes(requested)) throw new Error(`unknown suite: ${requested}`);

function run(command, args) {
    const result = spawnSync(command, args, { cwd: packageDirectory, stdio: "inherit" });
    if (result.status !== 0) process.exit(result.status ?? 1);
}

if (!process.argv.includes("--skip-build")) run("node", ["scripts/build.mjs"]);
const selected = requested ? [requested] : suites;
if (target === "browser") {
    run("node", ["tests/browser.mjs"]);
    process.exit(0);
}
const selectedNodeSuites = selected.filter((suite) =>
    ["api", "ast", "errors", "ownership", "robustness", "unicode"].includes(suite)
);
if (selectedNodeSuites.length) {
    run("node", ["--test", `--test-name-pattern=^(${selectedNodeSuites.join("|")}):`, "tests/node.test.mjs"]);
}
if (selected.includes("sessions")) {
    run("node", ["--test", "tests/session.test.mjs"]);
}
const packageSuites = selected.filter((suite) => ["consumer", "types", "packaging"].includes(suite));
if (packageSuites.length === 3) {
    run("node", ["tests/packaging.mjs", "all"]);
} else {
    for (const suite of packageSuites) run("node", ["tests/packaging.mjs", suite]);
}
