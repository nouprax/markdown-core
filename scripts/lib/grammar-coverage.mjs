/** Specification-owned feature inventory. A feature is admitted through its
 * source grammars and mathematical certificates, not through an AST-kind census. The
 * descriptor inventory independently guards against a newly attached module.
 */
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { readElementInventory } from "./element-inventory.mjs";

const digest = (value) => createHash("sha256").update(value).digest("hex");
// The specification is the feature boundary. These links retain the earlier
// proof families; newer families declare their owning specification directly.
export const featureOwners = {
    anchors: { elements: [], previous: [] },
    attributes: { elements: ["ATTRIBUTES"], previous: ["record-span", "class-span"] },
    base: { elements: ["DOCUMENT", "PARAGRAPH", "TEXT"], previous: [] },
    "block-identifiers": { elements: [], previous: ["anchor"] },
    "bracketed-spans": { elements: [], previous: ["record-span", "class-span"] },
    callouts: { elements: ["CALLOUT"], previous: [] },
    citations: { elements: ["CITATION"], previous: ["cite-author", "cite-suppress", "cite-normal"] },
    code: { elements: ["CODE", "CODE_BLOCK"], previous: [] },
    comments: { elements: ["COMMENT"], previous: ["opaque-comment", "leaf-comment"] },
    conflicts: { elements: [], previous: [] },
    "cross-links": {
        elements: ["CROSS_LINK"],
        previous: [
            "cross-link",
            "cross-embed",
            "cross-link-absent",
            "cross-embed-absent",
            "cross-link-empty",
            "cross-embed-empty",
            "cross-anchor",
            "cross-local",
            "embed-dimensions"
        ]
    },
    "definition-lists": { elements: ["DEFINITION_LIST"], previous: ["loose-definition"] },
    directives: {
        elements: ["DIRECTIVE"],
        previous: ["inline-directive", "empty-directive", "leaf-directive", "anonymous-container", "named-container"]
    },
    emphasis: { elements: ["EMPHASIS", "EMPHASIS_UNDERSCORE"], previous: [] },
    footnotes: { elements: ["FOOTNOTE"], previous: [] },
    formulas: {
        elements: ["FORMULA"],
        previous: ["opaque-formula", "opaque-display", "leaf-formula", "leaf-fence", "leaf-promotion"]
    },
    headings: { elements: ["HEADING"], previous: [] },
    html: { elements: ["HTML", "HTML_BLOCK"], previous: [] },
    insertion: { elements: ["INSERTION"], previous: ["insertion-strong", "run-insertion"] },
    "line-breaks": { elements: ["LINE_BREAK"], previous: [] },
    "links-and-images": { elements: ["AUTOLINK", "LINK", "EMBEDDED"], previous: ["embed-dimensions"] },
    lists: { elements: ["LIST"], previous: ["decimal-list"] },
    marks: { elements: ["MARK"], previous: ["run-mark"] },
    properties: { elements: [], previous: ["metadataempty", "metadata"] },
    specimens: { elements: ["SPECIMEN"], previous: ["specimen-reset"] },
    strikethrough: { elements: ["STRIKETHROUGH"], previous: ["run-strike"] },
    "superscript-and-subscript": { elements: ["SUPERSCRIPT", "SUBSCRIPT"], previous: ["run-super", "run-sub"] },
    tables: { elements: ["TABLE"], previous: ["grid-cell", "simple-matrix", "headless-matrix", "sparse-grid"] },
    "task-lists": { elements: [], previous: ["task-value"] },
    "thematic-breaks": { elements: ["THEMATIC_BREAK"], previous: [] }
};

/** Markdown headings outside fenced examples. Hash each complete section as
 * well as the file, so changing a rule inside an existing section is visible.
 * This is an inventory/disposition gate, not a claim that counting headings
 * proves equivalence of unrestricted feature languages.
 */
export function specificationSections(source) {
    const lines = source.split("\n");
    const sections = [];
    let fence = null;
    for (const [index, line] of lines.entries()) {
        const marker = /^ {0,3}(`{3,}|~{3,})/.exec(line)?.[1];
        if (marker) {
            if (!fence) fence = marker;
            else if (marker[0] === fence[0] && marker.length >= fence.length) fence = null;
            continue;
        }
        if (fence) continue;
        const match = /^(#{1,6}) (.+)$/u.exec(line);
        if (match) sections.push({ title: match[2], line: index + 1, level: match[1].length });
    }
    return sections.map((section, index) => ({
        ...section,
        sha256: digest(lines.slice(section.line - 1, sections[index + 1]?.line - 1 || lines.length).join("\n"))
    }));
}

export function featureCoverage(root, corpus) {
    const specDirectory = "docs/specs/dialect";
    const specifications = fs
        .readdirSync(path.join(root, specDirectory))
        .filter((name) => name.endsWith(".md"))
        .map((name) => name.slice(0, -3))
        .sort();
    assert.deepEqual(
        Object.keys(featureOwners).sort(),
        specifications,
        "every syntax specification needs an explicit feature disposition"
    );
    const inventory = readElementInventory(path.join(root, "packages/markdown-core/elements"));
    const elements = Object.values(featureOwners)
        .flatMap((entry) => entry.elements)
        .map((name) => `MARKDOWN_CORE_ELEMENT_${name}`)
        .sort();
    assert.deepEqual(
        elements,
        inventory.ordered.map((entry) => entry.symbol).sort(),
        "every registered element needs exactly one feature owner"
    );
    const certificates = new Map(corpus.certificates.map((entry) => [entry.id, entry]));
    const covered = new Set();
    const features = specifications.map((name) => {
        const owner = featureOwners[name];
        const ids = [
            ...new Set([
                ...owner.previous,
                ...corpus.certificates.filter((entry) => entry.feature === name).map((entry) => entry.id)
            ])
        ].sort();
        assert.ok(ids.length, `${name}: no grammar certificate`);
        const proofs = ids.map((id) => {
            const certificate = certificates.get(id);
            assert.ok(certificate, `${name}: missing certificate ${id}`);
            covered.add(id);
            assert.ok(
                corpus.proofs.some((proof) => proof.id === id),
                `${id}: missing generated proof`
            );
            assert.ok(
                corpus.cases.some((entry) => entry.id === id && entry.side === "dialect"),
                `${id}: missing executable input`
            );
            return {
                id,
                scope: certificate.scope,
                identity: certificate.identity === true,
                facets: certificate.facets ?? []
            };
        });
        const spec = `${specDirectory}/${name}.md`;
        const source = fs.readFileSync(path.join(root, spec), "utf8");
        return {
            feature: name,
            specification: spec,
            sha256: digest(source),
            sections: specificationSections(source),
            elements: owner.elements,
            certificates: proofs
        };
    });
    assert.deepEqual(covered, new Set(certificates.keys()), "every certificate must have a specification owner");
    return {
        version: 1,
        contract:
            "Every syntax feature and registered element has generated grammar-certified inputs; every specification section has a reviewed, content-bound disposition. Parser correctness is owned by parity and regression suites. Whole-language proofs apply only to the declared grammars; local boundaries do not certify their residual host grammar.",
        guide: {
            file: "docs/specs/dialect.md",
            sha256: digest(fs.readFileSync(path.join(root, "docs/specs/dialect.md")))
        },
        features
    };
}

export function validateFeatureCoverage(root, corpus) {
    const expected = JSON.parse(
        fs.readFileSync(path.join(root, "packages/markdown-core/benchmarks/grammar-coverage.json"), "utf8")
    );
    const actual = featureCoverage(root, corpus);
    assert.deepEqual(
        actual,
        expected,
        "grammar feature coverage changed: review the syntax/rule delta and update its certificates before regenerating grammar-coverage.json"
    );
    return {
        features: actual.features.length,
        elements: actual.features.reduce((sum, entry) => sum + entry.elements.length, 0),
        sections: actual.features.reduce((sum, entry) => sum + entry.sections.length, 0),
        identity: digest(JSON.stringify(actual))
    };
}
