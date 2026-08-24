import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const packageDirectory = path.resolve(fileURLToPath(new URL("..", import.meta.url)));

function run(command, args) {
    const result = spawnSync(command, args, { cwd: packageDirectory, stdio: "inherit" });
    if (result.status !== 0) process.exit(result.status ?? 1);
}

// `--skip-build`, for the same reason `run-tests.mjs` has it: the build job
// produces `dist/` and the test job consumes it, and the test job has no emsdk
// on PATH -- so rebuilding here does not merely waste work, it fails outright
// with `emcc ENOENT`. `run-es-test-artifact.sh` has been passing this flag all
// along and this runner was the one that never read it.
if (!process.argv.includes("--skip-build")) run("node", ["scripts/build.mjs"]);
run("node", ["--test", "tests/conformance.test.mjs"]);
