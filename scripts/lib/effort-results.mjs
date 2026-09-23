// Numeric projection shared by the full boundary report and PR comments.
import { boundaryModel, boundaryOperations } from "./effort-boundaries.mjs";

const positive = (value) => {
    if (!Number.isSafeInteger(value) || value <= 0) throw new Error("Invalid boundary measurement count");
    return value;
};

export function boundaryRows(report) {
    if (
        report?.schemaVersion !== 1 ||
        report.model !== boundaryModel ||
        report.scope !== "local-operation-including-native-adapters" ||
        typeof report.measured !== "boolean" ||
        !Array.isArray(report.cases) ||
        !Array.isArray(report.certificates) ||
        report.certificates.length !== boundaryOperations.length ||
        new Set(report.certificates).size !== boundaryOperations.length ||
        report.certificates.some((operation) => !boundaryOperations.includes(operation)) ||
        report.fullParserCertificates !== 0
    )
        throw new Error("Invalid local boundary report");
    if (!report.measured) return [];
    positive(report.iterations);
    const seen = new Set();
    const rows = report.cases.map((row) => {
        if (
            typeof row.id !== "string" ||
            !/^[a-z0-9][a-z0-9-]{0,127}$/.test(row.id) ||
            seen.has(row.id) ||
            !boundaryOperations.includes(row.operation)
        ) {
            throw new Error("Invalid or duplicate boundary input");
        }
        seen.add(row.id);
        const read = (engine) =>
            Object.fromEntries(
                ["operation", "prepare", "release"].map((stage) => {
                    const edge = row.engines[engine]?.[stage];
                    if (edge?.calls !== (stage === "operation" ? report.iterations : 1)) {
                        throw new Error("Mismatched boundary invocation counts");
                    }
                    return [stage, positive(edge.cost.Ir)];
                })
            );
        const core = read("markdown-core"),
            reference = read("cmark");
        return { id: row.id, operation: row.operation, core, reference, ratio: core.operation / reference.operation };
    });
    if (boundaryOperations.some((operation) => !rows.some((row) => row.operation === operation))) {
        throw new Error("Incomplete local boundary measurements");
    }
    return rows;
}
