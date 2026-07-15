import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const packageDirectory = path.resolve(fileURLToPath(new URL("..", import.meta.url)));

function run(command, args) {
    const result = spawnSync(command, args, { cwd: packageDirectory, stdio: "inherit" });
    if (result.status !== 0) process.exit(result.status ?? 1);
}

run("node", ["scripts/build.mjs"]);
run("node", ["--test", "tests/conformance.test.mjs"]);
