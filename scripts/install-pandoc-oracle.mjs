#!/usr/bin/env node
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(fileURLToPath(new URL("..", import.meta.url)));
const source = JSON.parse(fs.readFileSync(path.join(root, "specs/oracles/pandoc/source.json"), "utf8"));
const pin = source.executableOracle;
const directory = path.join(root, ".tools/pandoc", pin.version);
const binary = path.join(directory, process.platform === "win32" ? "pandoc.exe" : "pandoc");
const mode = process.argv[2];
if (!["--check", "--install"].includes(mode)) throw new Error("expected --check or --install");
const version = () => execFileSync(binary, ["--version"], { encoding: "utf8" });
if (mode === "--install" && !fs.existsSync(binary)) {
    const arch = process.arch === "x64" ? (process.platform === "linux" ? "amd64" : "x86_64") : process.arch;
    const platform = `${process.platform === "win32" ? "windows" : process.platform}-${arch}`;
    const artifact = pin.portableArtifacts.find((value) => value.platform === platform);
    if (!artifact) throw new Error(`no pinned Pandoc archive for ${platform}`);
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "markdown-core-pandoc-install-"));
    try {
        const archive = path.join(temporary, artifact.name);
        execFileSync("curl", ["--fail", "--location", "--retry", "2", "--output", archive, artifact.url], {
            stdio: "inherit"
        });
        const digest = createHash("sha256").update(fs.readFileSync(archive)).digest("hex");
        if (digest !== artifact.sha256) throw new Error("Pandoc archive SHA-256 mismatch");
        if (artifact.name.endsWith(".zip")) execFileSync("unzip", ["-q", archive, "-d", temporary]);
        else execFileSync("tar", ["-xzf", archive, "-C", temporary]);
        const candidates = fs
            .readdirSync(temporary, { recursive: true })
            .filter((name) => path.basename(name) === path.basename(binary));
        if (candidates.length !== 1) throw new Error("Pandoc archive has no unique executable");
        const extracted = path.join(temporary, candidates[0]);
        if (!execFileSync(extracted, ["--version"], { encoding: "utf8" }).startsWith(pin.versionPrefix))
            throw new Error("Pandoc archive version mismatch");
        fs.mkdirSync(directory, { recursive: true });
        fs.copyFileSync(extracted, binary);
        fs.chmodSync(binary, 0o755);
    } finally {
        fs.rmSync(temporary, { recursive: true, force: true });
    }
}
if (!fs.existsSync(binary) || !version().startsWith(pin.versionPrefix))
    throw new Error(
        `exact Pandoc ${pin.version} runner required; run scripts/init-environment.sh --install oracle-pandoc`
    );
process.stdout.write(`ok: Pandoc ${pin.version}\n`);
