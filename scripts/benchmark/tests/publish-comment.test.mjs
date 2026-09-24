import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";
import { parse } from "yaml";
import { attributeSection, publish, readArchive, stageSection } from "../publish-comment.mjs";
import { markdownReport } from "../run.mjs";

const head = "a".repeat(40);
const base = "b".repeat(40);
function stageReport(ir, { rejection = false } = {}) {
    const certificate = {
        id: "inline-links",
        certificate: "inline-links-grammar-v2",
        scope: "paired-document-grammar",
        reference: "cmark"
    };
    const rejected = {
        id: "inline-marks",
        certificate: "inline-marks-grammar-v2",
        scope: "paired-document-grammar",
        reference: null,
        rejects: "mark"
    };
    const engine = (source, ast = 50) => ({
        stages: { source_to_buffer: { cost: { Ir: source } }, buffer_to_ast: { cost: { Ir: ast } } },
        parsePathIr: source + ast + 10,
        outsideStagesIr: 10
    });
    const names = (owner, sides) =>
        Object.fromEntries(sides.map((side) => [`paired-${side}`, `${owner.id}-paired-${side}`]));
    const document = (owner, side, engines) => ({
        case: `${owner.id}-paired-${side}`,
        side,
        part: "paired",
        certificate: owner.certificate,
        units: 1,
        bytes: 32,
        sha256: "e".repeat(64),
        engines
    });
    const cases = [
        document(certificate, "dialect", { "markdown-core": engine(ir) }),
        document(certificate, "common", { "markdown-core": engine(ir), cmark: engine(50, 25) }),
        ...(rejection
            ? [
                  document(rejected, "dialect", { "markdown-core": engine(ir) }),
                  document(rejected, "common", { "markdown-core": engine(ir) }),
                  document(rejected, "control", { "markdown-core": engine(ir - 50) })
              ]
            : [])
    ];
    return {
        schemaVersion: 5,
        revision: base,
        corpus: { digest: "c".repeat(64), cases: cases.length },
        grammarCorpus: {
            identity: "d".repeat(64),
            certificates: [certificate, ...(rejection ? [rejected] : [])],
            proofs: [
                { ...certificate, units: 1, names: names(certificate, ["dialect", "common"]) },
                ...(rejection
                    ? [{ ...rejected, units: 1, names: names(rejected, ["dialect", "common", "control"]) }]
                    : [])
            ]
        },
        cases
    };
}
const attributes = () => ({
    schemaVersion: 1,
    baselines: {
        "markdown-core": { lists: 10, values: 30, ir: 200, dataReads: 100, dataWrites: 40 },
        lexbor: { lists: 10, values: 30, ir: 100, dataReads: 50, dataWrites: 20 }
    }
});

test("PR tables report numeric results, source regressions, and complete parse accounting", () => {
    const body = stageSection(stageReport(103), stageReport(100));
    assert.match(body, /0\/2 passed/);
    assert.match(body, /\| Source → buffer \| 200 \| 206 \| 1.0300× \|/);
    assert.match(body, /\| Complete parse path \| 320 \| 326 \|/);
    assert.match(body, /inline-links-paired-common/);
    assert.match(body, /Required when CI inputs require execution/);
    assert.match(body, /inline-links-grammar-v2 \| whole \| 1 \| 32\/32 \| 1.0000× \| 2.0400× \| 2.0400×/);
    assert.match(body, /Grammar:/);
    assert.match(attributeSection(attributes()), /2.0000×/);
});

test("Core-only rejections are reported against their control, apart from reference comparisons", () => {
    const body = stageSection(stageReport(103, { rejection: true }), stageReport(100, { rejection: true }));
    const start = body.indexOf("<summary>Grammar-equivalent reference comparisons");
    const equivalences = body.slice(start, body.indexOf("</details>", start));
    assert.match(equivalences, /inline-links-grammar-v2/);
    assert.doesNotMatch(equivalences, /inline-marks/);
    assert.match(body, /<summary>Rejection of constructs only Core implements/);
    assert.match(body, /\| inline-marks-grammar-v2 \| mark \| 1 \| 1\.4854× \| 50 \|/);
    assert.doesNotMatch(stageSection(stageReport(103), stageReport(100)), /Rejection of constructs/);
    for (const mutate of [
        (r) => {
            r.grammarCorpus.certificates[1].rejects = "@everyone";
        },
        (r) => {
            r.cases[3].engines.cmark = r.cases[1].engines.cmark;
        },
        (r) => {
            r.cases.pop();
            r.corpus.cases--;
        }
    ]) {
        const report = stageReport(103, { rejection: true });
        mutate(report);
        assert.throws(() => stageSection(report, stageReport(100, { rejection: true })));
    }
});

test("the measured report schema renders both the Markdown artifact and PR comparison", () => {
    const report = stageReport(100);
    const digest = "a".repeat(64);
    report.cmark = report.cmarkGfm = { version: "test", commit: head };
    report.toolchain = {
        compiler: "test compiler",
        compilerDigest: digest,
        compilerBinaries: digest,
        libc: "test libc",
        libraries: { summary: "test libraries", digest },
        valgrind: "test profiler",
        valgrindDigest: digest,
        architecture: "test architecture",
        target: "test target",
        targetDigest: digest,
        flags: "test flags",
        compiled: { shared: "test options", objects: {} },
        dispatch: digest
    };
    const artifact = markdownReport(report);
    const comment = stageSection(report, stageReport(100));
    assert.match(
        artifact,
        /inline-links-grammar-v2 \| paired-document-grammar \| 1 \| 32\/32 \| 150 \| 150 \| 75 \| 1.000x \| 2.000x \| 2.000x/
    );
    assert.match(comment, /inline-links-grammar-v2 \| whole \| 1 \| 32\/32 \| 1.0000× \| 2.0000× \| 2.0000×/);
    for (const output of [artifact, comment]) {
        assert.doesNotMatch(output, /\| Scale \|/);
    }
    assert.throws(() => markdownReport({ ...report, schemaVersion: 4 }), /report schema 5/);
});

test("report projections reject corrupt counts, mismatched workloads and injected text", () => {
    for (const mutate of [
        (r) => {
            r.schemaVersion = 4;
        },
        (r) => {
            delete r.grammarCorpus;
        },
        (r) => {
            r.grammarCorpus.identity = "[untrusted](https://example.com)";
        },
        (r) => {
            r.grammarCorpus.proofs[0].certificate = "@everyone";
        },
        (r) => {
            r.grammarCorpus.proofs[0].scope = "boundary-grammar";
        },
        (r) => {
            r.cases[1].engines.cmark.stages.buffer_to_ast.cost.Ir = -1;
        },
        (r) => {
            r.cases[0].case = "@everyone <script>";
        },
        (r) => {
            r.corpus.digest = "[click](https://example.com)";
        },
        (r) => {
            r.corpus.cases = 3;
        },
        (r) => {
            r.cases[0].units = -1;
        },
        (r) => {
            r.cases[0].bytes = 33;
        },
        (r) => {
            r.cases[0].engines["markdown-core"].parsePathIr = NaN;
        },
        (r) => {
            r.cases[0].engines["markdown-core"].parsePathIr = 0;
        },
        (r) => {
            r.cases[0].engines["markdown-core"].stages.source_to_buffer.cost.Ir = "100";
        },
        (r) => {
            r.cases[0].sha256 = "f".repeat(64);
        }
    ]) {
        const report = stageReport(100);
        mutate(report);
        assert.throws(() => stageSection(report, stageReport(100)));
    }
    for (const value of ["200", -1, null, Number.MAX_SAFE_INTEGER + 1]) {
        const report = attributes();
        report.baselines.lexbor.ir = value;
        assert.throws(() => attributeSection(report));
    }
    const different = attributes();
    different.baselines.lexbor.values++;
    assert.throws(() => attributeSection(different));
    const untrusted = stageReport(100);
    untrusted.toolchain = { compiler: "@everyone" };
    assert.doesNotMatch(stageSection(untrusted, stageReport(100)), /@everyone/);
});

test("archives read only fixed JSON members without extracting or executing files", (t) => {
    const cwd = fs.mkdtempSync(path.join(os.tmpdir(), "benchmark-archive-test-"));
    t.after(() => fs.rmSync(cwd, { recursive: true, force: true }));
    fs.writeFileSync(path.join(cwd, "attributes.json"), JSON.stringify(attributes()));
    fs.writeFileSync(path.join(cwd, "untrusted.sh"), "exit 1\n");
    execFileSync("zip", ["-q", "report.zip", "attributes.json", "untrusted.sh"], { cwd });
    const bytes = fs.readFileSync(path.join(cwd, "report.zip"));
    assert.deepEqual(readArchive(bytes, ["attributes.json"]), [attributes()]);
    assert.throws(() => readArchive(bytes, ["attributes.json"], { archiveBytes: bytes.length - 1 }));
    assert.throws(() => readArchive(bytes, ["attributes.json"], { memberBytes: 8 }));
    assert.throws(() => readArchive(bytes, ["stages.json"]));
    assert.throws(() => readArchive(Buffer.alloc(8 * 1024 * 1024 + 1), ["attributes.json"]));
    fs.writeFileSync(path.join(cwd, "attributes.json"), "x".repeat(16 * 1024 * 1024 + 1));
    execFileSync("zip", ["-q", "report.zip", "attributes.json"], { cwd });
    assert.throws(() => readArchive(fs.readFileSync(path.join(cwd, "report.zip")), ["attributes.json"]));
});

function fixture() {
    const run = {
        id: 42,
        run_attempt: 1,
        head_sha: head,
        head_branch: "contribution",
        head_repository: { id: 100 },
        path: ".github/workflows/ci.yml",
        event: "pull_request",
        status: "completed",
        conclusion: "success",
        created_at: "2026-09-22T12:00:00Z",
        pull_requests: [{ number: 9 }]
    };
    const pr = {
        number: 9,
        state: "open",
        created_at: "2026-09-22T11:00:00Z",
        base: { sha: base, repo: { full_name: "nouprax/markdown-core" } },
        head: { sha: head, ref: "contribution", repo: { id: 100 } }
    };
    const state = {
        run,
        pr,
        current: JSON.parse(JSON.stringify(pr)),
        latest: JSON.parse(JSON.stringify(run)),
        comments: [],
        writes: [],
        warnings: [],
        reads: [],
        associated: [pr],
        inputs: {
            version: 2,
            repository: "nouprax/markdown-core",
            event: "pull_request",
            ref: "refs/pull/9/merge",
            pullRequest: 9,
            head,
            base,
            fingerprint: "f".repeat(64),
            validation: { required: true, sources: {} }
        },
        artifacts: ["benchmark-report-stages-1", "benchmark-report-attributes-1", "ci-inputs"].map((name, id) => ({
            id,
            name,
            expired: false,
            size_in_bytes: 100
        })),
        jobs: [
            "Benchmark / Measure - parse stages against cmark",
            "Benchmark / Measure - the attribute grammar against lexbor"
        ].map((name) => ({ name, status: "completed", conclusion: "success", run_attempt: 1 }))
    };
    const snapshot = (run_id) => {
        state.reads.push(run_id);
        if (run_id === state.run.id) return state;
        assert.equal(run_id, state.original?.run.id, "only the recorded source may be read");
        return state.original;
    };
    const api = {
        repos: { listPullRequestsAssociatedWithCommit: () => state.associated },
        pulls: { get: async () => ({ data: state.current }) },
        actions: {
            listWorkflowRunArtifacts: ({ run_id }) => snapshot(run_id).artifacts,
            listJobsForWorkflowRun: ({ run_id }) => snapshot(run_id).jobs,
            getWorkflowRun: async ({ run_id }) => ({ data: snapshot(run_id).latest }),
            downloadArtifact: async ({ artifact_id }) => ({ data: Buffer.from(String(artifact_id)) })
        },
        issues: {
            listComments: () => state.comments,
            createComment: async (args) => state.writes.push({ method: "create", ...args }),
            updateComment: async (args) => state.writes.push({ method: "update", ...args })
        }
    };
    state.api = api;
    state.publish = () =>
        publish({
            github: { rest: api, paginate: async (method, args) => method(args) },
            context: {
                eventName: "workflow_run",
                repo: { owner: "nouprax", repo: "markdown-core" },
                payload: { workflow_run: run }
            },
            core: { warning: (message) => state.warnings.push(message) },
            read: (bytes) => {
                if (bytes.toString() === "102") return [state.inputs];
                if (bytes.toString() === "2") return [state.original?.inputs ?? state.inputs];
                return bytes.toString() === "0" ? [stageReport(103), stageReport(100)] : [attributes()];
            }
        });
    return state;
}

test("fork runs with empty PR metadata find the current PR through commit association", async () => {
    const state = fixture();
    state.run.pull_requests = [];
    state.run.conclusion = "failure"; // A failed source budget still has results.
    await state.publish();
    assert.equal(state.writes.length, 1);
    assert.equal(state.writes[0].issue_number, 9);
    assert.match(state.writes[0].body, /0\/2 passed/);
    assert.match(state.writes[0].body, /2.0000×/);
    assert.match(state.writes[0].body, /CI status: \*\*failure\*\*/);
    assert.match(state.writes[0].body, /runs\/42\/attempts\/1/);
});

test("run PR identities exclude another PR with the same repository, branch and SHA", async () => {
    const state = fixture();
    state.run.pull_requests = [{ number: 8 }];
    await state.publish();
    assert.equal(state.writes.length, 0);
});

test("closing a PR and opening another never transfers the old benchmark", async () => {
    for (const fork of [false, true]) {
        const state = fixture();
        if (fork) state.run.pull_requests = [];
        state.pr.state = "closed";
        state.current.number = 10;
        state.current.created_at = "2026-09-22T12:01:00Z";
        state.associated.push(state.current);
        await state.publish();
        assert.equal(state.writes.length, 0);
        // Even an artifact claiming the new PR cannot authorize this write.
        state.inputs.pullRequest = 10;
        state.inputs.ref = "refs/pull/10/merge";
        await state.publish();
        assert.equal(state.writes.length, 0);
    }
});

test("recorded inputs select only the triggering fork PR and its tested base", async () => {
    const state = fixture();
    state.run.pull_requests = [];
    state.associated.push({ ...state.pr, number: 10, base: { ...state.pr.base, sha: head } });
    await state.publish();
    assert.equal(state.writes.length, 1);
    assert.equal(state.writes[0].issue_number, 9);
    state.writes.length = 0;
    state.pr.base.sha = head;
    await state.publish();
    assert.equal(state.writes.length, 0);
});

test("base changes during publishing cannot receive the previous baseline's result", async () => {
    for (const fork of [false, true]) {
        const state = fixture();
        if (fork) state.run.pull_requests = [];
        state.current.base.sha = head;
        await state.publish();
        assert.equal(state.writes.length, 0);
    }
});

test("mutable workflow PR metadata cannot replace the recorded tested base", async () => {
    const state = fixture();
    state.pr.base.sha = state.current.base.sha = head;
    state.run.pull_requests[0].base = { sha: head };
    await state.publish();
    assert.equal(state.writes.length, 0);
});

test("unavailable or inconsistent input evidence fails closed without publishing", async () => {
    for (const mutate of [
        (s) => {
            s.artifacts.pop();
        },
        (s) => {
            s.artifacts[2].expired = true;
        },
        (s) => {
            s.artifacts[2].size_in_bytes = 65537;
        },
        (s) => {
            s.artifacts.push(s.artifacts[2]);
        },
        (s) => {
            s.inputs.version = 1;
        },
        (s) => {
            s.inputs.repository = "someone/else";
        },
        (s) => {
            s.inputs.event = "push";
        },
        (s) => {
            s.inputs.head = base;
        },
        (s) => {
            s.inputs.base = "bad";
        },
        (s) => {
            s.inputs.pullRequest = "9";
        },
        (s) => {
            s.inputs.ref = "refs/pull/10/merge";
        }
    ]) {
        const state = fixture();
        mutate(state);
        await state.publish();
        assert.equal(state.writes.length, 0);
        assert.equal(state.warnings.length, 1);
    }
});

test("a report for another baseline is unavailable even if the PR inputs match", async () => {
    const state = fixture();
    state.inputs.base = state.pr.base.sha = state.current.base.sha = head;
    await state.publish();
    assert.match(state.writes[0].body, /Result unavailable/);
    assert.doesNotMatch(state.writes[0].body, /0\/2 passed/);
    assert.match(state.writes[0].body, /2.0000×/);
});

test("one bot-owned comment is updated, without editing a user's marker imitation", async () => {
    const state = fixture();
    await state.publish();
    const body = state.writes[0].body;
    state.writes.length = 0;
    state.comments = [
        { id: 8, body, user: { login: "contributor", type: "User" } },
        { id: 10, body, user: { login: "github-actions[bot]", type: "Bot" } }
    ];
    await state.publish();
    assert.equal(state.writes.length, 1);
    assert.equal(state.writes[0].method, "update");
    assert.equal(state.writes[0].comment_id, 10);
});

test("obsolete, foreign, closed and superseded results never replace the current comment", async () => {
    for (const mutate of [
        (s) => {
            s.current.head.sha = base;
        },
        (s) => {
            s.current.state = "closed";
        },
        (s) => {
            s.pr.head.repo.id = 200;
        },
        (s) => {
            s.pr.base.repo.full_name = "someone/else";
        },
        (s) => {
            s.pr.head.ref = "another-branch";
        },
        (s) => {
            s.run.event = "push";
        },
        (s) => {
            s.run.path = ".github/workflows/other.yml";
        },
        (s) => {
            s.latest.run_attempt = 2;
        },
        (s) => {
            s.latest.status = "in_progress";
        },
        (s) => {
            s.comments = [
                {
                    id: 1,
                    user: { login: "github-actions[bot]", type: "Bot" },
                    body: "<!-- markdown-core-benchmark -->\n<!-- run:43:1 -->"
                }
            ];
        },
        (s) => {
            s.comments = [
                {
                    id: 1,
                    user: { login: "github-actions[bot]", type: "Bot" },
                    body: "<!-- markdown-core-benchmark -->\n<!-- run:42:2 -->"
                }
            ];
        }
    ]) {
        const state = fixture();
        mutate(state);
        await state.publish();
        assert.equal(state.writes.length, 0);
    }
});

test("missing, corrupt or expired artifacts produce an explicit partial result", async () => {
    for (const mutate of [
        (s) => {
            s.artifacts.shift();
        },
        (s) => {
            s.artifacts[0].expired = true;
        },
        (s) => {
            s.artifacts[0].size_in_bytes = 8 * 1024 * 1024 + 1;
        },
        (s) => {
            s.artifacts.push(s.artifacts[0]);
        },
        (s) => {
            s.artifacts[0].name = "benchmark-report-stages-0";
        },
        (s) => {
            s.artifacts[0].id = 2;
        }
    ]) {
        const state = fixture();
        mutate(state);
        await state.publish();
        assert.equal(state.writes.length, 1);
        assert.match(state.writes[0].body, /Result unavailable/);
        assert.match(state.writes[0].body, /2.0000×/);
        assert.equal(state.warnings.length, 1);
    }
});

function reusedFixture() {
    const state = fixture();
    state.original = Object.fromEntries(
        ["run", "latest", "inputs", "artifacts", "jobs"].map((key) => [key, globalThis.structuredClone(state[key])])
    );
    Object.assign(state.run, { id: 60, head_sha: "c".repeat(40), created_at: "2026-09-22T12:01:00Z" });
    state.latest = globalThis.structuredClone(state.run);
    state.pr.head.sha = state.current.head.sha = state.inputs.head = state.run.head_sha;
    state.inputs.validation = {
        required: false,
        sources: Object.fromEntries(
            ["ci.yml", "codeql.yml", "release-dry-run.yml"].map((name, index) => [
                `.github/workflows/${name}`,
                { runId: 42 + index, runAttempt: 1 }
            ])
        )
    };
    state.artifacts = [{ id: 102, name: "ci-inputs", expired: false, size_in_bytes: 100 }];
    state.jobs = [{ name: "Benchmark", status: "completed", conclusion: "skipped", run_attempt: 1 }];
    return state;
}

test("a documentation push recovers a measurement whose old publisher lost the PR-head race", async () => {
    const old = fixture();
    old.current.head.sha = "c".repeat(40);
    await old.publish();
    assert.equal(old.writes.length, 0, "obsolete publisher cannot write to the new head");
    for (const fork of [false, true]) {
        const state = reusedFixture();
        if (fork) {
            state.run.pull_requests = [];
            state.original.latest.pull_requests = [];
        }
        await state.publish();
        assert.equal(state.writes.length, 1, "no existing comment is needed");
        assert.equal(state.writes[0].method, "create");
        const { body } = state.writes[0];
        assert.match(body, /<!-- run:60:1 -->/);
        assert.ok(body.includes(`Commit: \`${state.run.head_sha}\``));
        assert.ok(body.includes(`Measured commit: \`${head}\``));
        assert.match(body, /Reused validation of identical execution inputs and integration base/);
        assert.match(body, /runs\/42\/attempts\/1/);
        assert.match(body, /0\/2 passed/);
        assert.match(body, /2.0000×/);
        assert.equal(state.warnings.length, 0);
    }
});

test("successive skips publish the direct original measurement and update the same comment", async () => {
    const state = reusedFixture();
    await state.publish();
    state.comments = [{ id: 10, user: { login: "github-actions[bot]", type: "Bot" }, body: state.writes[0].body }];
    state.writes.length = 0;
    state.run.id = state.latest.id = 80;
    state.run.head_sha =
        state.latest.head_sha =
        state.inputs.head =
        state.pr.head.sha =
        state.current.head.sha =
            "d".repeat(40);
    await state.publish();
    assert.equal(state.writes.length, 1);
    assert.equal(state.writes[0].method, "update");
    assert.equal(state.writes[0].comment_id, 10);
    assert.match(state.writes[0].body, /<!-- run:80:1 -->/);
    assert.match(state.writes[0].body, /0\/2 passed/);
    assert.ok(state.reads.every((id) => [42, 60, 80].includes(id)));
});

test("a wholly documentation-only PR has no original measurement and posts no empty report", async () => {
    const state = reusedFixture();
    state.inputs.validation.sources = {};
    await state.publish();
    assert.equal(state.writes.length, 0);
    assert.deepEqual(state.reads, [60]);
});

test("invalid reuse provenance cannot nominate a measurement or overwrite a comment", async () => {
    for (const validation of [
        undefined,
        { required: true, sources: { ".github/workflows/ci.yml": { runId: 42, runAttempt: 1 } } },
        { required: false, sources: { ".github/workflows/ci.yml": { runId: 42, runAttempt: 1 } } },
        ...[0, -1, "42", 60, 70].map((runId) => {
            const { sources } = reusedFixture().inputs.validation;
            sources[".github/workflows/ci.yml"].runId = runId;
            return { required: false, sources };
        })
    ]) {
        const state = reusedFixture();
        state.inputs.validation = validation;
        await state.publish();
        assert.equal(state.writes.length, 0);
        assert.deepEqual(state.reads, [60]);
    }
});

test("unavailable, superseded or mismatched original validation is explicit and never falls back", async () => {
    for (const mutate of [
        (s) => {
            s.original.latest.conclusion = "failure";
        },
        (s) => {
            s.original.latest.status = "in_progress";
        },
        (s) => {
            s.original.latest.run_attempt = 2;
        },
        (s) => {
            s.original.latest.path = ".github/workflows/other.yml";
        },
        (s) => {
            s.original.latest.event = "push";
        },
        (s) => {
            s.original.latest.head_repository.id = 200;
        },
        (s) => {
            s.original.latest.head_branch = "other";
        },
        (s) => {
            s.original.latest.created_at = "2026-09-22T12:02:00Z";
        },
        (s) => {
            s.original.latest.created_at = "2026-09-22T10:00:00Z";
        },
        (s) => {
            s.original.latest.pull_requests = [{ number: 10 }];
        },
        (s) => {
            s.original.inputs.pullRequest = 10;
            s.original.inputs.ref = "refs/pull/10/merge";
        },
        (s) => {
            s.original.inputs.base = head;
        },
        (s) => {
            s.original.inputs.fingerprint = "a".repeat(64);
        },
        (s) => {
            s.original.inputs.head = "c".repeat(40);
        },
        (s) => {
            s.original.inputs.validation.required = false;
        },
        (s) => {
            s.original.artifacts.pop();
        },
        (s) => {
            s.original.artifacts[2].expired = true;
        }
    ]) {
        const state = reusedFixture();
        mutate(state);
        await state.publish();
        assert.equal(state.writes.length, 1);
        assert.match(state.writes[0].body, /original measurement.*is unavailable/);
        assert.match(state.writes[0].body, /Result unavailable/);
        assert.doesNotMatch(state.writes[0].body, /0\/2 passed|2.0000×/);
    }
});

test("reused measurements still check the current PR and original attempt immediately before writing", async () => {
    for (const mutate of [
        (s) => {
            s.current.head.sha = head;
        },
        (s) => {
            s.current.base.sha = head;
        },
        (s) => {
            s.latest.run_attempt = 2;
        },
        (s) => {
            s.original.latest.run_attempt = 2;
        },
        (s) => {
            s.original.latest.status = "in_progress";
        },
        (s) => {
            s.original.latest.conclusion = "failure";
        }
    ]) {
        const state = reusedFixture();
        state.api.issues.listComments = () => {
            mutate(state);
            return [];
        };
        await state.publish();
        assert.equal(state.writes.length, 0);
    }
});

test("reuse reads each retained job's attempt and exposes missing original reports", async () => {
    const state = reusedFixture();
    state.inputs.validation.sources[".github/workflows/ci.yml"].runAttempt = 2;
    state.original.latest.run_attempt = 2;
    state.original.jobs[0].run_attempt = 2;
    state.original.artifacts[0].name = "benchmark-report-stages-2";
    await state.publish();
    assert.match(state.writes[0].body, /0\/2 passed/);
    assert.match(state.writes[0].body, /2.0000×/);
    assert.match(state.writes[0].body, /runs\/42\/attempts\/2/);
    state.writes.length = 0;
    state.original.artifacts[0].expired = true;
    await state.publish();
    assert.match(state.writes[0].body, /Result unavailable/);
    assert.doesNotMatch(state.writes[0].body, /0\/2 passed/);
    assert.match(state.writes[0].body, /2.0000×/);
});

test("failed-job retries retain successful measurements but cannot reuse a failed retry's old artifact", async () => {
    const state = fixture();
    state.run.run_attempt = state.latest.run_attempt = 2;
    state.jobs[0].run_attempt = 2;
    state.artifacts[0].name = "benchmark-report-stages-2";
    // Attribute job and artifact still belong to attempt 1.
    await state.publish();
    assert.match(state.writes[0].body, /0\/2 passed/);
    assert.match(state.writes[0].body, /2.0000×/);
    assert.match(state.writes[0].body, /runs\/42\/attempts\/2/);
    assert.equal(state.warnings.length, 0);
    state.writes.length = 0;
    state.artifacts[0].name = "benchmark-report-stages-1";
    await state.publish();
    assert.match(state.writes[0].body, /Result unavailable/);
    assert.doesNotMatch(state.writes[0].body, /0\/2 passed/);
    assert.match(state.writes[0].body, /2.0000×/);
});

test("skipped job names cannot substitute for the preflight's reuse evidence", async () => {
    const state = fixture();
    state.jobs = [{ name: "Benchmark", conclusion: "skipped" }];
    await state.publish();
    assert.equal(state.writes.length, 1);
    assert.match(state.writes[0].body, /Result unavailable/);
    assert.doesNotMatch(state.writes[0].body, /0\/2 passed|2.0000×/);
});

test("producer and publisher keep PR execution separate from write permissions", (t) => {
    const workflow = (file) =>
        parse(fs.readFileSync(new URL(`../../../.github/workflows/${file}.yml`, import.meta.url), "utf8"));
    const producer = workflow("benchmark");
    assert.ok(Object.values(producer.permissions).every((value) => value === "read"));
    for (const job of Object.values(producer.jobs)) {
        assert.ok(Object.values(job.permissions ?? {}).every((value) => value === "read"));
        for (const step of job.steps ?? []) {
            assert.doesNotMatch(step.run ?? "", /GITHUB_STEP_SUMMARY/);
            if (step.uses?.startsWith("actions/checkout@")) assert.equal(step.with["persist-credentials"], false);
        }
    }
    const publisher = workflow("benchmark-comment");
    assert.deepEqual(publisher.on, { workflow_run: { workflows: ["CI"], types: ["completed"] } });
    assert.equal(publisher.permissions["pull-requests"], "write");
    assert.equal(publisher.concurrency["cancel-in-progress"], false);
    const steps = publisher.jobs.comment.steps;
    const checkout = steps.find((step) => step.uses?.startsWith("actions/checkout@"));
    assert.equal(checkout.with.ref, "${{ github.sha }}");
    assert.equal(checkout.with["persist-credentials"], false);
    assert.equal(steps.length, 2);
    assert.ok(steps.every((step) => !step.run));
    assert.match(steps[1].with.script, /scripts\/benchmark\/publish-comment\.mjs/);
    // The trusted sparse checkout must include the publisher's complete module
    // graph. Import exactly those shipped files without the rest of this repo.
    const isolated = fs.mkdtempSync(path.join(os.tmpdir(), "benchmark-publisher-checkout-"));
    t.after(() => fs.rmSync(isolated, { recursive: true, force: true }));
    const root = fileURLToPath(new URL("../../../", import.meta.url));
    for (const file of checkout.with["sparse-checkout"].trim().split(/\s+/u)) {
        const target = path.join(isolated, file);
        fs.mkdirSync(path.dirname(target), { recursive: true });
        fs.copyFileSync(path.join(root, file), target);
    }
    execFileSync(process.execPath, [
        "--input-type=module",
        "--eval",
        "await import(process.argv[1])",
        pathToFileURL(path.join(isolated, "scripts/benchmark/publish-comment.mjs")).href
    ]);

    for (const [kind, members] of [
        ["stages", ["stages.json", "baseline/stages.json"]],
        ["attributes", ["attributes.json"]]
    ]) {
        const upload = producer.jobs[kind].steps.find(
            (step) => step.with?.name === `benchmark-report-${kind}-` + "${{ github.run_attempt }}"
        );
        assert.ok(upload);
        assert.equal(upload.if, "${{ always() }}");
        for (const member of members) assert.ok(upload.with.path.includes(member));
    }
});
