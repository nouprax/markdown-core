import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { parse } from "yaml";
import { capture, decide, fingerprint, isDocumentation, readEvidence } from "../ci-changes.mjs";

const before = "a".repeat(40);
const head = "b".repeat(40);
const base = "c".repeat(40);
const context = {
    eventName: "pull_request",
    ref: "refs/pull/240/merge",
    sha: "d".repeat(40),
    repo: { owner: "nouprax", repo: "markdown-core" },
    payload: {
        action: "synchronize",
        before,
        pull_request: { number: 240, head: { sha: head }, base: { sha: base } }
    }
};
const current = {
    version: 1,
    repository: "nouprax/markdown-core",
    event: "pull_request",
    ref: context.ref,
    pullRequest: 240,
    head,
    base,
    fingerprint: "e".repeat(64)
};
const workflows = ["ci.yml", "codeql.yml", "release-dry-run.yml"];

function fixture() {
    const runs = workflows.map((workflow, index) => ({
        id: index + 1,
        path: `.github/workflows/${workflow}`,
        event: "pull_request",
        head_sha: before,
        status: "completed",
        conclusion: "success",
        html_url: `https://github.com/nouprax/markdown-core/actions/runs/${index + 1}`
    }));
    const records = runs.map(() => ({ ...current, head: before }));
    const options = {
        context: globalThis.structuredClone(context),
        current: { ...current },
        baseFingerprint: "f".repeat(64),
        github: {
            rest: {
                actions: {
                    listWorkflowRunsForRepo: async (query) => {
                        assert.equal(query.head_sha, before);
                        return { data: { total_count: runs.length, workflow_runs: runs } };
                    }
                }
            }
        },
        evidence: async (_github, _repo, run) => records[run.id - 1]
    };
    return { runs, records, options };
}

test("only explicit prose paths are documentation", () => {
    for (const file of [
        "README.md",
        "CHANGELOG.md",
        "docs/reviews/report.md",
        "docs/deprecated/specs/old.md",
        "packages/swift-markdown-core/README.md",
        "specs/oracles/pandoc/differences.md"
    ])
        assert.equal(isDocumentation(file), true, file);
    for (const file of [
        "packages/markdown-core/core/node.c",
        "specs/canonical-ast/example.md",
        "specs/oracles/remark/corpus.md",
        "packages/markdown-core/tests/fixtures/input.md",
        "docs/specs/canonical-ast.md",
        "docs/specs/canonical-ast.json",
        "docs/diagram.js",
        ".github/workflows/ci.yml",
        "pnpm-lock.yaml",
        "VERSION",
        "LICENSE",
        "scripts/example.md"
    ])
        assert.equal(isDocumentation(file), false, file);
});

function repository(t) {
    const cwd = fs.mkdtempSync(path.join(os.tmpdir(), "ci-changes-test-"));
    t.after(() => fs.rmSync(cwd, { recursive: true, force: true }));
    const git = (...args) =>
        execFileSync("git", ["-c", "core.fsmonitor=false", ...args], {
            cwd,
            encoding: "utf8",
            env: { ...process.env, GIT_CONFIG_NOSYSTEM: "1" }
        }).trim();
    git("init", "-q");
    git("config", "user.name", "CI test");
    git("config", "user.email", "ci@example.invalid");
    git("config", "commit.gpgsign", "false");
    const write = (file, contents) => {
        fs.mkdirSync(path.dirname(path.join(cwd, file)), { recursive: true });
        fs.writeFileSync(path.join(cwd, file), contents);
    };
    const commit = () => {
        git("add", "--all");
        git("commit", "-qm", "snapshot");
        return git("rev-parse", "HEAD");
    };
    return { cwd, git, write, commit };
}

test("fingerprints preserve code, configuration, fixture, rename, and mode boundaries", (t) => {
    const { cwd, git, write, commit } = repository(t);
    write("engine.c", "int value;\n");
    write("README.md", "English\n");
    const original = fingerprint(cwd, commit());
    write("README.md", "Updated documentation\n");
    write("docs/reviews/new\nreport.md", "Prose\n");
    assert.equal(fingerprint(cwd, commit()), original);
    fs.unlinkSync(path.join(cwd, "docs/reviews/new\nreport.md"));
    assert.equal(fingerprint(cwd, commit()), original);
    git("mv", "engine.c", "docs/engine.md");
    const renamed = fingerprint(cwd, commit());
    assert.notEqual(renamed, original, "renaming code into docs must not hide deletion");
    fs.chmodSync(path.join(cwd, "README.md"), 0o755);
    const executable = fingerprint(cwd, commit());
    assert.notEqual(executable, renamed, "executable docs are inputs");
    fs.unlinkSync(path.join(cwd, "README.md"));
    fs.symlinkSync("docs/engine.md", path.join(cwd, "README.md"));
    const symlink = fingerprint(cwd, commit());
    assert.notEqual(symlink, executable, "symlinks are inputs");
    for (const file of ["specs/input.md", "docs/specs/schema.md", ".github/workflows/ci.yml", "lockfile"]) {
        const prior = fingerprint(cwd, git("rev-parse", "HEAD"));
        write(file, "changed\n");
        assert.notEqual(fingerprint(cwd, commit()), prior, file);
    }
});

test("capture records the actual tested merge base and rejects a head-only checkout", (t) => {
    const { cwd, git, write, commit } = repository(t);
    write("engine.c", "base\n");
    const baseSha = commit();
    git("checkout", "-qb", "topic");
    write("engine.c", "head\n");
    const headSha = commit();
    git("checkout", "--detach", baseSha);
    git("merge", "--no-ff", "-m", "test merge", headSha);
    const mergeSha = git("rev-parse", "HEAD");
    const event = globalThis.structuredClone(context);
    event.sha = mergeSha;
    event.payload.pull_request.head.sha = headSha;
    event.payload.pull_request.base.sha = "f".repeat(40);
    assert.equal(capture(event, cwd).base, baseSha, "mutable metadata is not the tested base");
    event.sha = headSha;
    assert.throws(() => capture(event, cwd), /expected test merge/);
});

test("a complete docs-only PR skips without querying old runs", async () => {
    const { options } = fixture();
    options.baseFingerprint = current.fingerprint;
    options.github = null;
    assert.equal((await decide(options)).required, false);
});

test("docs-only follow-ups reuse all three successful workflows", async () => {
    const { options } = fixture();
    const result = await decide(options);
    assert.equal(result.required, false);
    assert.match(result.reason, /actions\/runs\/3/);
});

test("failures, cancellation, in-progress, skipped, and missing runs require full CI", async () => {
    for (const conclusion of ["failure", "cancelled", "skipped", "neutral", "timed_out", null]) {
        const { runs, options } = fixture();
        runs[1].conclusion = conclusion;
        assert.equal((await decide(options)).required, true, String(conclusion));
    }
    const { runs, options } = fixture();
    runs[0].status = "in_progress";
    assert.equal((await decide(options)).required, true);
    runs.shift();
    assert.equal((await decide(options)).required, true);
});

test("a newer failed run cannot be bypassed using an older green run", async () => {
    const { runs, options } = fixture();
    runs.push({ ...runs[0], id: 99, conclusion: "failure" });
    assert.equal((await decide(options)).required, true);
});

test("changed inputs or base and foreign, stale, or absent evidence prevent reuse", async () => {
    for (const [key, value] of Object.entries({
        version: 2,
        repository: "elsewhere/repo",
        event: "push",
        ref: "refs/pull/241/merge",
        pullRequest: 241,
        head,
        base: "f".repeat(40),
        fingerprint: "f".repeat(64)
    })) {
        const { records, options } = fixture();
        records[0][key] = value;
        assert.equal((await decide(options)).required, true, key);
    }
    const { records, options } = fixture();
    records[0] = null;
    assert.equal((await decide(options)).required, true);
});

test("updated live PR metadata cannot overwrite the captured base", async () => {
    const { options, records, runs } = fixture();
    options.current.base = "f".repeat(40);
    options.context.payload.pull_request.base.sha = options.current.base;
    for (const run of runs) run.pull_requests = [options.context.payload.pull_request];
    assert.equal(records[0].base, base);
    assert.equal((await decide(options)).required, true);
});

test("manual, scheduled, tag release, and code-changing merge groups always run", async () => {
    for (const [eventName, ref] of [
        ["workflow_dispatch", context.ref],
        ["schedule", "refs/heads/main"],
        ["push", "refs/tags/v1.0.2"],
        ["merge_group", "refs/heads/gh-readonly-queue/main/test"]
    ]) {
        const { options } = fixture();
        options.context.eventName = eventName;
        options.context.ref = ref;
        options.github = null;
        assert.equal((await decide(options)).required, true, eventName);
    }
});

test("all-docs merge groups skip, but base updates and missing before snapshots do not reuse", async () => {
    const { options } = fixture();
    options.context.eventName = "merge_group";
    options.baseFingerprint = current.fingerprint;
    assert.equal((await decide(options)).required, false);
    options.context.eventName = "pull_request";
    options.current.base = "f".repeat(40);
    assert.equal((await decide(options)).required, true);
    options.current.base = base;
    options.baseFingerprint = null;
    for (const value of [undefined, "0".repeat(40), "bad ref"]) {
        options.context.payload.before = value;
        assert.equal((await decide(options)).required, true);
    }
});

test("main docs follow-ups require both successful push workflows", async () => {
    const { options, runs, records } = fixture();
    options.context.eventName = "push";
    options.context.ref = "refs/heads/main";
    Object.assign(options.current, { event: "push", ref: options.context.ref, pullRequest: null, base: null });
    for (const run of runs) run.event = "push";
    for (const record of records) Object.assign(record, { ...options.current, head: before });
    runs.pop();
    assert.equal((await decide(options)).required, false);
    runs[1].conclusion = "failure";
    assert.equal((await decide(options)).required, true);
});

test("API failures, incomplete history, and corrupt artifacts fall back to full CI", async () => {
    const { options } = fixture();
    options.evidence = async () => {
        throw new Error("Invalid archive");
    };
    assert.equal((await decide(options)).required, true);
    options.github.rest.actions.listWorkflowRunsForRepo = async () => ({
        data: { total_count: 101, workflow_runs: [] }
    });
    assert.equal((await decide(options)).required, true);
    options.github.rest.actions.listWorkflowRunsForRepo = async () => {
        throw new Error("Unavailable");
    };
    assert.equal((await decide(options)).required, true);
});

test("evidence archives are bounded, unique, unexpired, and parsed without extraction", async (t) => {
    const { cwd, write } = repository(t);
    write("inputs.json", JSON.stringify(current));
    execFileSync("zip", ["-q", "inputs.zip", "inputs.json"], { cwd });
    const bytes = fs.readFileSync(path.join(cwd, "inputs.zip"));
    const artifacts = [{ id: 1, name: "ci-inputs", expired: false, size_in_bytes: bytes.length }];
    const github = {
        rest: {
            actions: {
                listWorkflowRunArtifacts: async () => ({ data: { total_count: artifacts.length, artifacts } }),
                downloadArtifact: async () => ({ data: bytes })
            }
        }
    };
    assert.deepEqual(await readEvidence(github, context.repo, { id: 1 }), current);
    artifacts[0].expired = true;
    assert.equal(await readEvidence(github, context.repo, { id: 1 }), null);
    artifacts[0].expired = false;
    artifacts[0].size_in_bytes = 65537;
    assert.equal(await readEvidence(github, context.repo, { id: 1 }), null);
    artifacts[0].size_in_bytes = bytes.length;
    artifacts.push({ ...artifacts[0], id: 2 });
    assert.equal(await readEvidence(github, context.repo, { id: 1 }), null);
});

const workflow = (name) =>
    parse(fs.readFileSync(new URL(`../../.github/workflows/${name}.yml`, import.meta.url), "utf8"));

test("required contexts skip only after successful preflight explicitly permits it", () => {
    for (const [file, gate, roots] of [
        [
            "ci",
            "required-gates",
            [
                "health-check-repository",
                "health-check-c",
                "health-check-es",
                "health-check-kotlin",
                "health-check-swift"
            ]
        ],
        ["codeql", "codeql-gate", ["analyze"]],
        ["release-dry-run", "dry-run-gate", ["validate"]]
    ]) {
        const config = workflow(file);
        assert.equal(config.jobs.changes.uses, "./.github/workflows/changes.yml");
        for (const event of ["pull_request", "merge_group"]) {
            assert.ok(event in config.on);
            assert.equal(config.on[event]?.paths, undefined);
            assert.equal(config.on[event]?.["paths-ignore"], undefined);
        }
        const job = config.jobs[gate];
        assert.ok(job.needs.includes("changes"));
        const evaluate = new Function("needs", "always", `return (${job.if.slice(3, -2)});`);
        for (const result of ["success", "failure", "cancelled", "skipped"]) {
            for (const required of ["true", "false", "", "invalid"]) {
                assert.equal(
                    evaluate({ changes: { result, outputs: { required } } }, () => true),
                    !(result === "success" && required === "false"),
                    `${file}: ${result}/${required}`
                );
            }
        }
        for (const root of roots) {
            assert.equal(config.jobs[root].needs, "changes");
            assert.equal(config.jobs[root].if, "${{ needs.changes.outputs.required == 'true' }}");
        }
    }
});

test("full-validation gates reject every failed, missing, or unexpectedly skipped dependency", () => {
    for (const [file, gate] of [
        ["ci", "required-gates"],
        ["ci", "tests-ready"],
        ["codeql", "codeql-gate"],
        ["release-dry-run", "dry-run-gate"]
    ]) {
        const step = workflow(file).jobs[gate].steps[0];
        const success = Object.fromEntries(Object.keys(step.env).map((key) => [key, "success"]));
        const execute = (env) =>
            spawnSync("bash", ["-e", "-c", step.run], {
                env: { ...process.env, ...env },
                encoding: "utf8"
            }).status;
        assert.equal(execute(success), 0, `${file}/${gate}: all dependencies succeeded`);
        for (const key of Object.keys(success)) {
            for (const result of ["failure", "cancelled", "skipped", ""]) {
                assert.notEqual(execute({ ...success, [key]: result }), 0, `${file}/${gate}: ${key}/${result}`);
            }
        }
    }
});

test("documentation decisions cover the complete job graph including benchmarks", () => {
    for (const file of ["ci", "codeql", "release-dry-run", "pr-benchmark"]) {
        const { jobs } = workflow(file);
        const skipped = new Set();
        const visit = (id, ancestors = new Set()) => {
            if (id === "changes" || skipped.has(id)) return;
            assert.ok(!ancestors.has(id), `${file}/${id}: dependency cycle`);
            const job = jobs[id];
            const dependencies = [job.needs ?? []].flat();
            assert.ok(dependencies.length, `${file}/${id}: bypasses preflight`);
            for (const dependency of dependencies) visit(dependency, new Set([...ancestors, id]));
            if (job.if) {
                // Explicit conditions are also checked when they override the
                // implicit success() dependency check, as always() does.
                const needs = Object.fromEntries(
                    dependencies.map((dependency) => [
                        dependency,
                        dependency === "changes"
                            ? { result: "success", outputs: { required: "false" } }
                            : { result: "skipped" }
                    ])
                );
                const evaluate = new Function("needs", "always", "github", `return (${job.if.slice(3, -2)});`);
                for (const event_name of ["pull_request", "push", "merge_group"]) {
                    assert.equal(
                        evaluate(needs, () => true, { event_name }),
                        false,
                        `${file}/${id}`
                    );
                }
            } else {
                assert.ok(
                    dependencies.some((dependency) => skipped.has(dependency)),
                    `${file}/${id}: would run`
                );
            }
            skipped.add(id);
        };
        for (const id of Object.keys(jobs)) visit(id);
    }
});
