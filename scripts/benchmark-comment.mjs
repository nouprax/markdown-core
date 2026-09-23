import { Buffer } from "node:buffer";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { inputVersion, sameInputs, validationSource } from "./lib/ci-inputs.mjs";
import { sourceBudget, SOURCE_IR_LIMIT } from "./lib/source-budget.mjs";
import { boundaryOperations } from "./lib/effort-boundaries.mjs";
import { boundaryRows } from "./lib/effort-results.mjs";
import { median, sameInputGroups, stageComparisons } from "./lib/stage-comparisons.mjs";

const marker = "<!-- markdown-core-benchmark -->";
const archiveLimit = 8 * 1024 * 1024;
const reportLimit = 16 * 1024 * 1024;
const measurements = [
    {
        kind: "effort",
        title: "Local equal-effort operations",
        job: "Benchmark / Measure - parse stages against cmark",
        members: ["effort.json"],
        render: effortSection
    },
    {
        kind: "stages",
        title: "Parse stages",
        job: "Benchmark / Measure - parse stages against cmark",
        members: ["stages.json", "baseline/stages.json"],
        render: stageSection
    },
    {
        kind: "attributes",
        title: "Attribute grammar",
        job: "Benchmark / Measure - the attribute grammar against lexbor",
        members: ["attributes.json"],
        render: attributeSection
    }
];
const count = (value) => {
    if (!Number.isSafeInteger(value) || value < 0) throw new Error("Invalid benchmark count");
    return value;
};
const digest = (value, length = 64) => {
    if (typeof value !== "string" || !new RegExp(`^[0-9a-f]{${length}}$`).test(value)) {
        throw new Error("Invalid benchmark identity");
    }
    return value;
};
const number = (value) => count(value).toLocaleString("en-US");
const ratio = (after, before) => (before > 0 ? `${(after / before).toFixed(4)}×` : "n/a");

// These are projections of the existing report schemas, not Markdown supplied
// by a PR. Only validated IDs, digests and numeric counts reach the comment.
function stageCounts(report) {
    if (report?.schemaVersion !== 4 || !Array.isArray(report.cases) || !report.cases.length) {
        throw new Error("Invalid stage report");
    }
    digest(report.corpus.digest);
    digest(report.pairingDigest);
    if (report.corpus.cases !== report.cases.length) throw new Error("Incomplete stage report");
    const totals = [0, 0, 0, 0];
    for (const row of report.cases) {
        if (typeof row.case !== "string" || !/^[a-z0-9][a-z0-9-]{0,127}$/.test(row.case)) {
            throw new Error("Invalid benchmark case ID");
        }
        if (!count(row.scale) || !count(row.bytes)) throw new Error("Empty benchmark workload");
        digest(row.sha256);
        const engine = row.engines["markdown-core"];
        const values = [
            engine.stages.source_to_buffer.cost.Ir,
            engine.stages.buffer_to_ast.cost.Ir,
            engine.outsideStagesIr,
            engine.parsePathIr
        ].map(count);
        if (values[0] + values[1] + values[2] !== values[3]) throw new Error("Inconsistent parse counts");
        values.forEach((value, i) => (totals[i] = count(totals[i] + value)));
    }
    return totals;
}

export function stageSection(current, baseline) {
    const after = stageCounts(current);
    const before = stageCounts(baseline);
    if (current.corpus.digest !== baseline.corpus.digest || current.pairingDigest !== baseline.pairingDigest) {
        throw new Error("Benchmark identities differ");
    }
    const rows = sourceBudget(current.cases, baseline.cases);
    const failures = rows.filter((row) => !row.passed);
    const lines = [
        referenceSection(current),
        "",
        "### PR/base regression",
        "",
        `Baseline: \`${digest(baseline.revision, 40)}\`. ${number(rows.length)} document/scale workloads, measured in the same job.`,
        "",
        "| Core instructions (Ir) | Base | PR | PR / base |",
        "| --- | ---: | ---: | ---: |"
    ];
    ["Source → buffer", "Buffer → AST", "Outside the two stages", "Complete parse path"].forEach((name, i) =>
        lines.push(`| ${name} | ${number(before[i])} | ${number(after[i])} | ${ratio(after[i], before[i])} |`)
    );
    lines.push(
        "",
        "Totals sum this finite workload; they are not elapsed time or a general speedup claim.",
        "",
        `Source budget (+${((SOURCE_IR_LIMIT - 1) * 100).toFixed(0)}% per document): **${number(rows.length - failures.length)}/${number(rows.length)} passed**, ${number(failures.length)} exceeded. Required when CI inputs require execution.`,
        "",
        "<details><summary>Largest source-stage ratios (up to 10 workloads)</summary>",
        "",
        "| Case | Scale | Base Ir | PR Ir | PR / base |",
        "| --- | ---: | ---: | ---: | ---: |",
        ...[...rows]
            .sort((a, b) => b.ratio - a.ratio)
            .slice(0, 10)
            .map(
                (row) =>
                    `| ${row.case} | ${row.scale} | ${number(row.before)} | ${number(row.after)} | ${ratio(row.after, row.before)} |`
            ),
        "",
        "</details>",
        "",
        `Corpus: \`${current.corpus.digest}\` · Pairing: \`${current.pairingDigest}\`.`,
        "Full reference comparisons, all workloads, toolchain identities and raw profiles are in the run artifacts."
    );
    return lines.join("\n");
}

export function referenceSection(report) {
    // These derived stage values also drive stages.md. Validate their original
    // counts before interpreting any grouping metadata from an artifact.
    if (!Array.isArray(report.pairs)) throw new Error("Missing stage pairing contracts");
    for (const row of report.cases) {
        if (
            !Array.isArray(row.carries) ||
            !["commonmark", "extended"].includes(row.dialect) ||
            (row.gfm !== undefined && typeof row.gfm !== "boolean")
        )
            throw new Error("Invalid reference cohort");
        for (const engine of Object.values(row.engines)) {
            for (const name of ["source_to_buffer", "buffer_to_ast"]) {
                const stage = engine.stages[name];
                if (!count(stage.cost.Ir) || stage.ir !== stage.cost.Ir)
                    throw new Error("Invalid reference stage count");
            }
        }
        const reference = row.gfm ? "cmark-gfm" : "cmark";
        if ((row.gfm || row.dialect === "commonmark") && !row.engines[reference]) {
            throw new Error("Missing reference measurement");
        }
    }
    const groups = sameInputGroups(stageComparisons(report));
    const lines = [
        "### Same-input parser comparisons",
        "",
        "Core/reference instruction ratios at scale 1, summing Source → buffer + Buffer → AST. Lower is less measured work; these are not elapsed-time speedups.",
        "",
        "| Workload | Reference | Cases | Median Core/reference | Worst Core/reference |",
        "| --- | --- | ---: | ---: | ---: |"
    ];
    for (const [label, reference, rows] of groups) {
        if (!rows.length) continue;
        lines.push(
            `| ${label} | ${reference} | ${number(rows.length)} | ${ratio(median(rows.map((row) => row.comparisonRatio)), 1)} | ${ratio(rows[0].comparisonRatio, 1)} |`
        );
    }
    if (groups.every(([, , rows]) => !rows.length)) lines.push("", "No same-input reference workloads measured.");
    lines.push(
        "",
        "Structural pairs, candidate substitutions, unmatched fields and feature-absent diagnostics are excluded from these medians. Cross-syntax A/B, B/R and A/R remain in the full report as descriptive controls."
    );
    return lines.join("\n");
}

export function effortSection(report) {
    const rows = boundaryRows(report);
    if (!rows.length) throw new Error("Local boundary report has no measured instruction counts");
    digest(report.identity);
    digest(report.revision, 40);
    const lines = [
        "### Local equal-effort operations",
        "",
        `${number(rows.length)} measured inputs across ${number(boundaryOperations.length)} identical local operation contracts. Core/cmark compares production code plus native adapters; lower is less measured instruction work. This does not certify equal whole-parser effort or attainment of the optimum.`,
        "",
        "| Operation | Inputs | Median Core/cmark | Worst Core/cmark |",
        "| --- | ---: | ---: | ---: |"
    ];
    for (const operation of boundaryOperations) {
        const values = rows.filter((row) => row.operation === operation).map((row) => row.ratio);
        lines.push(
            `| ${operation} | ${number(values.length)} | ${ratio(median(values), 1)} | ${ratio(
                values.reduce((a, b) => Math.max(a, b)),
                1
            )} |`
        );
    }
    lines.push(
        "",
        "<details><summary>Largest local operation ratios (up to 10 inputs)</summary>",
        "",
        "| Input | Core operation Ir | cmark operation Ir | Core/cmark | Core prepare/release Ir | cmark prepare/release Ir |",
        "| --- | ---: | ---: | ---: | ---: | ---: |"
    );
    for (const row of [...rows].sort((a, b) => b.ratio - a.ratio).slice(0, 10)) {
        lines.push(
            `| ${row.id} | ${number(row.core.operation)} | ${number(row.reference.operation)} | ${ratio(row.core.operation, row.reference.operation)} | ${number(row.core.prepare)} / ${number(row.core.release)} | ${number(row.reference.prepare)} / ${number(row.reference.release)} |`
        );
    }
    lines.push(
        "",
        "</details>",
        "",
        `Operation Ir includes ${number(report.iterations)} fresh invocations per input. Preparation and release are separate harness costs; local ratios are not combined into a whole-parse ratio.`,
        `Contract: \`${report.identity}\`. All input-level ratios, receipts and raw profiles are in the effort report artifacts.`
    );
    return lines.join("\n");
}

export function attributeSection(report) {
    if (report?.schemaVersion !== 1) throw new Error("Invalid attribute report");
    const lines = [
        "### Attribute grammar",
        "",
        "| Engine | Lists | Values | Instructions (Ir) | Data reads | Data writes |",
        "| --- | ---: | ---: | ---: | ---: | ---: |"
    ];
    for (const name of ["markdown-core", "lexbor"]) {
        const row = report.baselines[name];
        const values = [row.lists, row.values, row.ir, row.dataReads, row.dataWrites];
        if (values.some((value) => !count(value))) throw new Error("Empty attribute measurement");
        lines.push(`| ${name} | ${values.map(number).join(" | ")} |`);
    }
    const ours = report.baselines["markdown-core"];
    const theirs = report.baselines.lexbor;
    if (ours.lists !== theirs.lists || ours.values !== theirs.values) throw new Error("Attribute census differs");
    lines.push("", `Core / lexbor instructions: **${ratio(ours.ir, theirs.ir)}** on the same recovered attributes.`);
    return lines.join("\n");
}

// Never extract archive paths into the checkout or interpret report content as
// commands. Read only fixed, bounded JSON members from a private temporary zip.
// A decoder may handle SIGTERM without exiting while its output pipe is full.
// Force termination at either resource limit so the synchronous wait is bounded.
export function readArchive(bytes, members, { archiveBytes = archiveLimit, memberBytes = reportLimit } = {}) {
    if (bytes.length > archiveBytes) throw new Error("Benchmark archive exceeds limit");
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "benchmark-comment-"));
    try {
        const zip = path.join(temporary, "report.zip");
        fs.writeFileSync(zip, bytes);
        const names = execFileSync("unzip", ["-Z1", zip], {
            encoding: "utf8",
            maxBuffer: 65536,
            timeout: 5000,
            killSignal: "SIGKILL"
        })
            .trim()
            .split("\n");
        return members.map((member) => {
            if (names.filter((name) => name === member).length !== 1)
                throw new Error("Missing or duplicate report member");
            return JSON.parse(
                execFileSync("unzip", ["-p", zip, member], {
                    encoding: "utf8",
                    maxBuffer: memberBytes,
                    timeout: 5000,
                    killSignal: "SIGKILL"
                })
            );
        });
    } finally {
        fs.rmSync(temporary, { recursive: true, force: true });
    }
}

export async function publish({ github, context, core, read = readArchive }) {
    const run = context.payload.workflow_run;
    const repo = context.repo;
    if (
        context.eventName !== "workflow_run" ||
        run.event !== "pull_request" ||
        run.path !== ".github/workflows/ci.yml" ||
        run.status !== "completed"
    )
        return;
    digest(run.head_sha, 40);
    count(run.id);
    count(run.run_attempt);
    const associated = await github.paginate(github.rest.repos.listPullRequestsAssociatedWithCommit, {
        ...repo,
        commit_sha: run.head_sha,
        per_page: 100
    });
    // Run membership binds same-head PRs to the triggering PR. Creation time
    // also prevents a later replacement PR from inheriting a fork run whose
    // pull_requests array is empty. Artifact metadata may only narrow these
    // API-authorized candidates, never nominate an unrelated PR.
    const triggering = new Set((run.pull_requests ?? []).map((pr) => pr.number));
    const matchesRun = (pr) =>
        pr.state === "open" &&
        (triggering.size === 0 || triggering.has(pr.number)) &&
        Date.parse(pr.created_at) <= Date.parse(run.created_at) &&
        pr.base.repo.full_name === `${repo.owner}/${repo.repo}` &&
        pr.head.sha === run.head_sha &&
        pr.head.repo?.id === run.head_repository.id &&
        pr.head.ref === run.head_branch;
    const candidates = associated.filter(matchesRun);
    if (!candidates.length) return;

    // Both the current preflight and its original validation use the same
    // bounded artifact reader and snapshot checks. A comment is never evidence.
    const load = async (measurementRun) => {
        const artifacts = await github.paginate(github.rest.actions.listWorkflowRunArtifacts, {
            ...repo,
            run_id: measurementRun.id,
            per_page: 100
        });
        const readArtifact = async (name, members, limits = {}) => {
            const matches = artifacts.filter((item) => item.name === name);
            if (
                matches.length !== 1 ||
                matches[0].expired ||
                !count(matches[0].size_in_bytes) ||
                matches[0].size_in_bytes > (limits.archiveBytes ?? archiveLimit)
            )
                throw new Error("Missing, expired or oversized benchmark artifact");
            const archive = await github.rest.actions.downloadArtifact({
                ...repo,
                artifact_id: matches[0].id,
                archive_format: "zip"
            });
            return read(Buffer.from(archive.data), members, limits);
        };
        const [inputs] = await readArtifact("ci-inputs", ["inputs.json"], { archiveBytes: 65536, memberBytes: 16384 });
        if (
            inputs?.version !== inputVersion ||
            inputs.repository !== `${repo.owner}/${repo.repo}` ||
            inputs.event !== "pull_request" ||
            inputs.head !== measurementRun.head_sha ||
            !count(inputs.pullRequest) ||
            inputs.ref !== `refs/pull/${inputs.pullRequest}/merge`
        )
            throw new Error("CI inputs do not identify the triggering PR snapshot");
        digest(inputs.head, 40);
        digest(inputs.base, 40);
        digest(inputs.fingerprint);
        const source = validationSource(inputs, run.path, measurementRun);
        return { run: measurementRun, inputs, source, artifacts, readArtifact };
    };
    let target;
    try {
        target = await load(run);
    } catch (error) {
        core.warning(`Cannot bind benchmark results to a PR: ${error.message}`);
        return;
    }
    const { inputs, source } = target;
    // Historical workflow API responses can contain the PR's current base.
    // The existing input evidence records the actual tested merge's parent.
    const matches = (pr) => matchesRun(pr) && pr.number === inputs.pullRequest && pr.base.sha === inputs.base;
    const pulls = candidates.filter(matches);
    if (!pulls.length) return;

    // A wholly documentation-only PR has no measurement. A follow-up that
    // reused validation points directly to the full run, even across many skips.
    if (!source) return;
    const reused = source.runId !== run.id;
    let measured = target;
    try {
        if (reused) {
            const { data: original } = await github.rest.actions.getWorkflowRun({ ...repo, run_id: source.runId });
            if (
                original.id !== source.runId ||
                original.run_attempt !== source.runAttempt ||
                original.status !== "completed" ||
                original.conclusion !== "success" ||
                original.path !== run.path ||
                original.event !== run.event ||
                original.head_repository?.id !== run.head_repository.id ||
                original.head_branch !== run.head_branch ||
                !(Date.parse(original.created_at) <= Date.parse(run.created_at)) ||
                !(Date.parse(original.created_at) >= Date.parse(pulls[0].created_at)) ||
                (original.pull_requests?.length &&
                    !original.pull_requests.some((pr) => pr.number === inputs.pullRequest))
            )
                throw new Error("Original validation run no longer matches the recorded source");
            measured = await load(original);
            if (!measured.inputs.validation.required || !sameInputs(measured.inputs, inputs)) {
                throw new Error("Original validation does not prove identical execution inputs and base");
            }
        }
        // Failed-job reruns retain successful jobs from earlier attempts. Use
        // each job's own attempt, never an earlier artifact of a failed retry.
        measured.jobs = await github.paginate(github.rest.actions.listJobsForWorkflowRun, {
            ...repo,
            run_id: measured.run.id,
            filter: "latest",
            per_page: 100
        });
    } catch (error) {
        core.warning(`Cannot recover benchmark measurement: ${error.message}`);
        measured = null;
    }
    const sections = [];
    for (const { kind, title, job: jobName, members, render } of measurements) {
        try {
            if (!measured) throw new Error("Original measurement is unavailable");
            const job = measured.jobs.find((item) => item.name === jobName);
            const artifactName = `benchmark-report-${kind}-${job?.run_attempt}`;
            if (job?.conclusion === "skipped" && !measured.artifacts.some((item) => item.name === artifactName)) {
                sections.push(`### ${title}\n\nMeasurement skipped. See the run logs for the preflight decision.`);
                continue;
            }
            if (job?.status !== "completed" || !count(job.run_attempt) || job.run_attempt > measured.run.run_attempt) {
                throw new Error("Measurement job is missing or belongs to another attempt");
            }
            const reports = await measured.readArtifact(artifactName, members);
            if (kind === "stages" && reports[1]?.revision !== inputs.base) {
                throw new Error("Stage baseline differs from the tested PR base");
            }
            sections.push(render(...reports));
        } catch (error) {
            core.warning(`Could not read ${kind} benchmark: ${error.message}`);
            sections.push(`### ${title}\n\nResult unavailable. See the run logs and artifacts.`);
        }
    }
    const status = ["success", "failure", "cancelled", "timed_out", "skipped"].includes(run.conclusion)
        ? run.conclusion
        : "unknown";
    const runUrl = `https://github.com/${repo.owner}/${repo.repo}/actions/runs/${run.id}/attempts/${run.run_attempt}`;
    const measurementUrl = `https://github.com/${repo.owner}/${repo.repo}/actions/runs/${source.runId}/attempts/${source.runAttempt}`;
    const provenance = reused
        ? measured
            ? `Reused validation of identical execution inputs and integration base. Measured commit: \`${measured.run.head_sha}\` · [Original run and full reports](${measurementUrl}).`
            : `Reused validation's [original measurement](${measurementUrl}) is unavailable.`
        : `[Run and full reports](${measurementUrl}).`;
    const body = `${marker}\n<!-- run:${run.id}:${run.run_attempt} -->\n## Benchmark\n\nCommit: \`${run.head_sha}\` · [CI run](${runUrl}) · CI status: **${status}**\n\n${provenance}\n\nBenchmark is required when CI inputs require execution. This ordinary PR comment does not create a review thread to resolve.\n\n${sections.join("\n\n")}`;

    for (const pr of pulls) {
        const comments = await github.paginate(github.rest.issues.listComments, {
            ...repo,
            issue_number: pr.number,
            per_page: 100
        });
        const existing = comments.find(
            (comment) =>
                comment.user?.login === "github-actions[bot]" &&
                comment.user.type === "Bot" &&
                comment.body?.startsWith(`${marker}\n`)
        );
        const previous = /<!-- run:(\d+):(\d+) -->/.exec(existing?.body ?? "");
        if (
            previous &&
            (Number(previous[1]) > run.id || (Number(previous[1]) === run.id && Number(previous[2]) > run.run_attempt))
        )
            continue;
        // Refresh after downloads so an old completion cannot overwrite a
        // newer PR snapshot (including base) or a newer attempt of the same run.
        const { data: latest } = await github.rest.actions.getWorkflowRun({ ...repo, run_id: run.id });
        const { data: current } = await github.rest.pulls.get({ ...repo, pull_number: pr.number });
        if (!matches(current) || latest.run_attempt !== run.run_attempt || latest.status !== "completed") continue;
        if (reused && measured) {
            const { data: original } = await github.rest.actions.getWorkflowRun({ ...repo, run_id: source.runId });
            if (
                original.run_attempt !== source.runAttempt ||
                original.status !== "completed" ||
                original.conclusion !== "success"
            )
                continue;
        }
        if (existing) {
            await github.rest.issues.updateComment({ ...repo, comment_id: existing.id, body });
        } else {
            await github.rest.issues.createComment({ ...repo, issue_number: pr.number, body });
        }
    }
}
