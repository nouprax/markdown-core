import { Buffer } from "node:buffer";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const evidenceName = "ci-inputs";
const requiredWorkflows = [
    ".github/workflows/ci.yml",
    ".github/workflows/codeql.yml",
    ".github/workflows/release-dry-run.yml"
];

// Everything is an execution input unless explicitly identified as prose.
// In particular, .md fixtures and machine-readable contracts are not docs.
export function isDocumentation(file) {
    return (
        /^(?:README|CHANGELOG|CONTRIBUTING|UPSTREAM|AGENTS)\.md$/.test(file) ||
        (/^docs\/.+\.md$/s.test(file) && !file.startsWith("docs/specs/")) ||
        /^packages\/[^/]+\/README\.md$/.test(file) ||
        /^specs\/oracles\/[^/]+\/(?:README|IMPORTS|differences)\.md$/.test(file)
    );
}

function git(cwd, ...args) {
    return execFileSync("git", args, { cwd, maxBuffer: 64 * 1024 * 1024 });
}

export function fingerprint(cwd, revision) {
    if (!/^[0-9a-f]{40}$/.test(revision)) throw new Error("Invalid commit SHA");
    const tree = git(cwd, "ls-tree", "-rz", "--full-tree", revision);
    const hash = createHash("sha256");
    let start = 0;
    while (start < tree.length) {
        const end = tree.indexOf(0, start);
        if (end < 0) throw new Error("Unterminated Git tree entry");
        const entry = tree.subarray(start, end);
        const separator = entry.indexOf(9);
        if (separator < 0) throw new Error("Invalid Git tree entry");
        const header = entry.subarray(0, separator).toString("ascii");
        const filename = entry.subarray(separator + 1);
        const file = filename.toString("utf8");
        // Hash raw Git bytes, including mode and path. Invalid UTF-8 names,
        // executable Markdown, symlinks, and submodules are always inputs.
        if (!header.startsWith("100644 blob ") || !Buffer.from(file).equals(filename) || !isDocumentation(file)) {
            hash.update(entry);
            hash.update("\0");
        }
        start = end + 1;
    }
    return hash.digest("hex");
}

export function capture(context, cwd) {
    const { payload, eventName, sha, ref } = context;
    const head = payload.pull_request?.head.sha ?? sha;
    let base = null;
    if (eventName === "pull_request") {
        // Record the actual tested merge, not mutable PR metadata from the API.
        const parents = git(cwd, "show", "-s", "--format=%P", sha).toString().trim().split(" ");
        if (parents.length !== 2 || parents[1] !== head) {
            throw new Error("PR checkout is not the expected test merge");
        }
        base = parents[0];
    } else if (eventName === "merge_group") {
        base = payload.merge_group.base_sha;
    }
    return {
        version: 1,
        repository: `${context.repo.owner}/${context.repo.repo}`,
        event: eventName,
        ref,
        pullRequest: payload.pull_request?.number ?? null,
        head,
        base,
        fingerprint: fingerprint(cwd, sha)
    };
}

export async function readEvidence(github, repo, run) {
    const { data } = await github.rest.actions.listWorkflowRunArtifacts({
        ...repo,
        run_id: run.id,
        per_page: 100
    });
    if (data.total_count > data.artifacts.length) return null;
    const artifacts = data.artifacts.filter((artifact) => artifact.name === evidenceName);
    if (artifacts.length !== 1) return null;
    const artifact = artifacts[0];
    if (artifact.expired || artifact.size_in_bytes <= 0 || artifact.size_in_bytes > 65536) return null;
    const archive = await github.rest.actions.downloadArtifact({
        ...repo,
        artifact_id: artifact.id,
        archive_format: "zip"
    });
    const bytes = Buffer.from(archive.data);
    if (bytes.length > 65536) return null;
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "ci-inputs-"));
    try {
        const zip = path.join(temporary, "inputs.zip");
        fs.writeFileSync(zip, bytes);
        // Read one bounded member without extracting paths from the archive.
        return JSON.parse(
            execFileSync("unzip", ["-p", zip, "inputs.json"], {
                encoding: "utf8",
                maxBuffer: 16384,
                timeout: 5000
            })
        );
    } finally {
        fs.rmSync(temporary, { recursive: true, force: true });
    }
}

export async function decide({ context, current, baseFingerprint, github, evidence = readEvidence }) {
    const run = (reason) => ({ required: true, reason });
    const skip = (reason) => ({ required: false, reason });
    const { eventName, payload, ref, repo } = context;
    if (
        !["pull_request", "push", "merge_group"].includes(eventName) ||
        (eventName === "push" && !ref.startsWith("refs/heads/"))
    ) {
        return run("Manual, scheduled, and release runs always execute fully.");
    }
    if (eventName === "pull_request" && current.base !== payload.pull_request.base.sha) {
        return run("The checked-out merge base differs from the event base.");
    }
    if (["pull_request", "merge_group"].includes(eventName) && current.fingerprint === baseFingerprint) {
        return skip("The complete change against the integration base contains documentation only.");
    }
    const before = payload.before;
    if (
        !/^[0-9a-f]{40}$/.test(before ?? "") ||
        /^0+$/.test(before) ||
        (eventName === "pull_request" && payload.action !== "synchronize") ||
        eventName === "merge_group"
    ) {
        return run("No previous push snapshot is available for reuse.");
    }
    try {
        // Only the immediately preceding push is eligible. Do not search past
        // failed/in-progress runs to find an older green result.
        const { data } = await github.rest.actions.listWorkflowRunsForRepo({
            ...repo,
            event: eventName,
            head_sha: before,
            per_page: 100
        });
        if (data.total_count > data.workflow_runs.length) return run("Run history is incomplete.");
        const workflows = eventName === "push" ? requiredWorkflows.slice(0, 2) : requiredWorkflows;
        const sources = [];
        for (const workflow of workflows) {
            const previous = data.workflow_runs
                .filter(
                    (candidate) =>
                        candidate.path === workflow && candidate.event === eventName && candidate.head_sha === before
                )
                .sort((left, right) => right.id - left.id)[0];
            if (!previous || previous.status !== "completed" || previous.conclusion !== "success") {
                return run(`The preceding ${workflow} run has not completed successfully.`);
            }
            const record = await evidence(github, repo, previous);
            if (
                !record ||
                record.version !== 1 ||
                record.repository !== current.repository ||
                record.event !== current.event ||
                record.ref !== current.ref ||
                record.pullRequest !== current.pullRequest ||
                record.head !== before ||
                record.base !== current.base ||
                record.fingerprint !== current.fingerprint
            ) {
                return run(`The preceding ${workflow} run does not prove identical execution inputs and base.`);
            }
            sources.push(previous.html_url);
        }
        return skip(`Reusing successful validation of identical inputs and integration base: ${sources.join(", ")}`);
    } catch (error) {
        return run(`Validation evidence is unavailable; running fully (${error.message}).`);
    }
}

export async function check({ github, context, core, cwd = process.cwd() }) {
    const current = capture(context, cwd);
    const baseFingerprint = current.base ? fingerprint(cwd, current.base) : null;
    const decision = await decide({ context, current, baseFingerprint, github });
    const directory = path.join(cwd, "build/ci");
    fs.mkdirSync(directory, { recursive: true });
    fs.writeFileSync(path.join(directory, "inputs.json"), `${JSON.stringify(current)}\n`);
    core.info(decision.reason);
    core.setOutput("required", String(decision.required));
    await core.summary.addHeading("CI execution").addRaw(decision.reason).write();
}
