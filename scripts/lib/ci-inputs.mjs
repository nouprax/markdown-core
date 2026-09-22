export const inputVersion = 2;
export const requiredWorkflows = [
    ".github/workflows/ci.yml",
    ".github/workflows/codeql.yml",
    ".github/workflows/release-dry-run.yml"
];

// Heads may differ only after preflight proves equal execution inputs. Keep
// the PR, integration base and fingerprint in the same reuse identity.
export function sameInputs(left, right) {
    return ["version", "repository", "event", "ref", "pullRequest", "base", "fingerprint"].every(
        (key) => left[key] === right[key]
    );
}

// Each reused workflow points directly to its original full validation, not
// to another skip. Provenance stays bounded across arbitrarily many doc pushes.
export function validationSource(record, workflow, run) {
    if (
        !Number.isSafeInteger(run.id) ||
        run.id <= 0 ||
        !Number.isSafeInteger(run.run_attempt) ||
        run.run_attempt <= 0
    ) {
        throw new Error("Invalid validation run");
    }
    const validation = record.validation;
    if (
        record.version !== inputVersion ||
        typeof validation?.required !== "boolean" ||
        !validation.sources ||
        typeof validation.sources !== "object" ||
        Array.isArray(validation.sources)
    )
        throw new Error("Invalid validation provenance");
    const keys = Object.keys(validation.sources);
    const expected = record.event === "push" ? requiredWorkflows.slice(0, 2) : requiredWorkflows;
    if (
        validation.required
            ? keys.length !== 0
            : keys.length !== 0 && (keys.length !== expected.length || keys.some((key) => !expected.includes(key)))
    ) {
        throw new Error("Incomplete validation provenance");
    }
    for (const source of Object.values(validation.sources)) {
        if (
            !Number.isSafeInteger(source?.runId) ||
            source.runId <= 0 ||
            source.runId >= run.id ||
            !Number.isSafeInteger(source.runAttempt) ||
            source.runAttempt <= 0
        ) {
            throw new Error("Invalid validation source run");
        }
    }
    if (!validation.required) return validation.sources[workflow] ?? null;
    return { runId: run.id, runAttempt: run.run_attempt };
}
