import assert from "node:assert/strict";
import { digest } from "./pandoc-oracle.mjs";

/** Reviewed witnesses, rather than enabled reader flags, name the feature being
 * exercised. Dependency extensions stay enabled when two witnesses are joined. */
export function compositionCases(source, corpus, seeds) {
    assert.equal(seeds.schemaVersion, 1, "unsupported composition seed schema");
    const selected = Object.keys(source.profileBoundary.publicToPandocExtensions).sort();
    const features = seeds.features.map((seed) => seed.feature);
    assert.deepEqual([...features].sort(), selected, "composition seeds must cover each selected feature exactly once");
    const cases = new Map(corpus.cases.map((testCase) => [testCase.id, testCase]));
    assert.equal(cases.size, corpus.cases.length, "duplicate corpus id");
    assert.equal(new Set(seeds.features.map((seed) => seed.case)).size, selected.length, "reuse of a seed case");
    const knownExtensions = new Set(Object.values(source.profileBoundary.publicToPandocExtensions).flat());
    const witnesses = new Map();
    for (const seed of seeds.features) {
        const testCase = cases.get(seed.case);
        assert.ok(testCase, `unknown composition seed: ${seed.case}`);
        assert.ok(typeof testCase.input === "string" && testCase.input.trim(), `empty seed: ${seed.case}`);
        assert.match(testCase.from, /^markdown_strict(?:\+[a-z_]+)+$/, `uncontrolled reader: ${seed.case}`);
        const extensions = testCase.from.split("+").slice(1);
        assert.equal(new Set(extensions).size, extensions.length, `duplicate reader extension: ${seed.case}`);
        assert.ok(
            extensions.every((extension) => knownExtensions.has(extension)),
            `unknown reader extension: ${seed.case}`
        );
        assert.ok(
            source.profileBoundary.publicToPandocExtensions[seed.feature].some((extension) =>
                extensions.includes(extension)
            ),
            `seed does not enable its feature: ${seed.feature}`
        );
        witnesses.set(seed.feature, { ...testCase, extensions });
    }
    const result = [];
    for (const first of selected) {
        for (const second of selected) {
            if (first === second) continue;
            const left = witnesses.get(first);
            const right = witnesses.get(second);
            // Exactly one blank line separates the two complete snippets. No
            // identifier renaming, AST concatenation, or content normalization
            // hides document-wide resolution or block adjacency interactions.
            const input = `${left.input.replace(/\n*$/, "")}\n\n${right.input.replace(/\n*$/, "")}\n`;
            const from = `markdown_strict+${[...new Set([...left.extensions, ...right.extensions])].sort().join("+")}`;
            result.push({
                id: `${first}--${second}`,
                features: [first, second],
                seeds: [left.id, right.id],
                from,
                input,
                inputDigest: digest([from, input])
            });
        }
    }
    return result;
}

/** This is diagnostic evidence, not a difference waiver. In particular a
 * seed's registered difference is never inherited by its composed input. */
export function compareCompositions(cases, oracle, product) {
    assert.equal(new Set(cases.map((testCase) => testCase.id)).size, cases.length, "duplicate composition id");
    const results = cases.map((testCase) => {
        const expected = oracle(testCase.input, testCase.from);
        const actual = product(testCase.input);
        const oracleDigest = digest(expected);
        const coreDigest = digest(actual);
        return {
            ...testCase,
            status: oracleDigest === coreDigest ? "agreement" : "difference",
            oracleDigest,
            coreDigest,
            oracle: expected,
            core: actual
        };
    });
    const agreements = results.filter((result) => result.status === "agreement").length;
    return {
        schemaVersion: 1,
        scope: "deterministic semantic comparisons; context labels distinguish adjacency, nesting, opacity and mutation cases; source-position and complexity coverage are separate gates",
        caseCount: results.length,
        agreements,
        differences: results.length - agreements,
        cases: results
    };
}

/** Wrap every selected witness in shared block inputs and opaque blocks. The
 * wrapper changes only container prefixes; the witness bytes remain authored
 * content, including identifiers and reference definitions. */
export function compositionEvidenceCases(source, corpus, seeds) {
    const adjacent = compositionCases(source, corpus, seeds).map((value) => ({ ...value, context: "adjacent" }));
    const byId = new Map(corpus.cases.map((value) => [value.id, value]));
    const result = [...adjacent];
    for (const { feature, case: id } of [...seeds.features].sort((a, b) => a.feature.localeCompare(b.feature))) {
        const witness = byId.get(id);
        const input = witness.input.replace(/\n*$/, "");
        for (const [context, wrap, extensions] of [
            [
                "blockquote",
                (value) =>
                    value
                        .split("\n")
                        .map((line) => `> ${line}`)
                        .join("\n"),
                []
            ],
            ["grid-cell", gridCell, ["grid_tables"]],
            [
                "code-block",
                (value) =>
                    value
                        .split("\n")
                        .map((line) => `    ${line}`)
                        .join("\n"),
                []
            ]
        ]) {
            const from = `markdown_strict+${[...new Set([...witness.from.split("+").slice(1), ...extensions])].sort().join("+")}`;
            const authored = `${wrap(input)}\n`;
            result.push({
                id: `${context}--${feature}`,
                context,
                features: [feature],
                seeds: [id],
                from,
                input: authored,
                inputDigest: digest([from, authored])
            });
        }
    }
    return result;
}

function gridCell(input) {
    const lines = input.split("\n");
    const widths = lines.map((line) => {
        let column = 2; // the left border and the cell's leading space
        for (const scalar of line) column += scalar === "\t" ? 4 - (column % 4) : 1;
        return column;
    });
    const width = Math.max(...widths);
    const border = `+${"-".repeat(width)}+`;
    return [border, ...lines.map((line, index) => `| ${line}${" ".repeat(width - widths[index])} |`), border].join(
        "\n"
    );
}

/** A fixed integer PRNG, explicit alphabet, and stable reader make mutations
 * reproducible on every host. Differences remain visible evidence; fuzz cases
 * never inherit a seed's single-case waiver. */
export function compositionFuzzCases(source, corpus, seeds, count = 128) {
    const witnesses = [...seeds.features].sort((a, b) => a.feature.localeCompare(b.feature));
    compositionCases(source, corpus, seeds); // validate the reviewed inventory
    const byId = new Map(corpus.cases.map((value) => [value.id, value]));
    const atoms = ["[", "]", "{", "}", "@", "^", "~", ":", "1.", "(iv)", "+---+", "|", "\\", "\t", "é", " "];
    const from = `markdown_strict+${[...new Set(Object.values(source.profileBoundary.publicToPandocExtensions).flat())].sort().join("+")}`;
    let state = 0x50313112;
    const next = () => {
        state ^= state << 13;
        state ^= state >>> 17;
        state ^= state << 5;
        return state >>> 0;
    };
    return Array.from({ length: count }, (_, index) => {
        const item = witnesses[next() % witnesses.length];
        const witness = byId.get(item.case);
        const before = Array.from({ length: 1 + (next() % 32) }, () => atoms[next() % atoms.length]).join("");
        const after = Array.from({ length: 1 + (next() % 32) }, () => atoms[next() % atoms.length]).join("");
        const input = `${before}\n\n${witness.input.replace(/\n*$/, "")}\n\n${after}\n`;
        return {
            id: `fuzz-${index}`,
            context: "fuzz",
            features: [item.feature],
            seeds: [item.case],
            from,
            input,
            inputDigest: digest([from, input])
        };
    });
}
