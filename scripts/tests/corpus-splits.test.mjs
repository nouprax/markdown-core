import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { stateValidators } from "../lib/canonical-states.mjs";
import { caseClosure, splitManifestFailures } from "../lib/corpus-splits.mjs";

const root = path.resolve(fileURLToPath(new URL("../..", import.meta.url)));
const manifestPath = path.join(root, "packages/markdown-core/benchmarks/corpus.json");

/**
 * The rules are held against the manifest as checked in, so a declaration
 * that stops satisfying them fails here before the audit builds a parser to
 * say so -- and each rule is shown to FAIL by breaking exactly what it holds,
 * in a copy of the manifest, because a check that has never been seen to
 * fail is a check nobody knows is there.
 */
function manifest() {
    return JSON.parse(fs.readFileSync(manifestPath, "utf8"));
}

const states = new Set(Object.keys(stateValidators));

/* Units as the generator would report them when both halves were built to
 * the same count, which is what the manifest asks for by matching the `with`
 * to the case its `without` is sized by. */
function equalUnits(declared) {
    const units = {};
    for (const split of declared.splits) {
        for (const host of split.hosts) {
            units[host.with] = 1000;
            units[host.without] = 1000;
        }
    }
    return units;
}

const byName = (declared, name) => declared.cases.find((entry) => entry.name === name);
const hostOf = (declared, name) => declared.splits[0].hosts.find((host) => host.with === name);

test("the manifest as checked in declares its splits within the rules", () => {
    const declared = manifest();
    assert.ok(declared.splits.length >= 1, "the corpus declares a split");
    assert.deepEqual(splitManifestFailures(declared, equalUnits(declared), states), []);
});

test("a case named alone drags its comparison in, and only in the direction it is defined", () => {
    const declared = manifest();
    const split = declared.splits[0];
    const heading = split.hosts.find((host) => host.host === "ATX heading");
    /* A `with` drags its `without`, which is a pair half and drags its twin. */
    assert.deepEqual(
        [...caseClosure(declared, [heading.with])].sort(),
        [
            heading.with,
            "pair-ldirective-common",
            "pair-ldirective-dialect",
            "proof-leaf-directive-common",
            "proof-leaf-directive-dialect"
        ].sort()
    );
    /* The `without` is a comparison on its own and drags no `with`. */
    assert.deepEqual(
        [...caseClosure(declared, [heading.without])].sort(),
        [
            "pair-ldirective-common",
            "pair-ldirective-dialect",
            "proof-leaf-directive-common",
            "proof-leaf-directive-dialect"
        ].sort()
    );
    /* A `without` that is a plain generated case drags nothing. */
    const autolink = split.hosts.find((host) => host.host === "angle autolink");
    assert.deepEqual([...caseClosure(declared, [autolink.without])], [autolink.without]);
    assert.deepEqual([...caseClosure(declared, [autolink.with])].sort(), [autolink.with, autolink.without].sort());
    /* Either half of a pair drags the other. */
    for (const pair of declared.pairs.filter((pair) => !pair.contract.review)) {
        assert.deepEqual([...caseClosure(declared, [pair.case])].sort(), [pair.case, pair.isomorph].sort());
        assert.deepEqual([...caseClosure(declared, [pair.isomorph])].sort(), [pair.case, pair.isomorph].sort());
    }
    /* A counted case drags the generated case it is sized by. */
    const counted = declared.cases.find((entry) => entry.counted && !hostOf(declared, entry.name));
    assert.ok(caseClosure(declared, [counted.name]).has(counted.counted.match));
});

/* One mutation per rule: what is broken, and the words the failure must say. */
const MUTATIONS = [
    {
        rule: "the remainder names an unpairable proof",
        mutate: (declared) => (declared.splits[0].remainder = "no such production"),
        expect: /names no unpairable entry/u,
        breaksEveryHost: true
    },
    {
        rule: "the remainder declares its bytes",
        mutate: (declared) => delete declared.splits[0].bytes,
        expect: /declares no remainder bytes/u,
        breaksEveryHost: true
    },
    {
        rule: "the remainder names the fields it populates",
        mutate: (declared) => (declared.splits[0].varies = []),
        expect: /names no field the remainder populates/u,
        breaksEveryHost: true
    },
    {
        rule: "every state is a declared grammar state",
        mutate: (declared) => declared.splits[0].states.push("markup.attributes.imaginary"),
        expect: /markup\.attributes\.imaginary, which is not a declared grammar state/u,
        breaksEveryHost: true
    },
    {
        rule: "both halves are cases",
        mutate: (declared) => (hostOf(declared, "split-attributes-link").without = "no-such-case"),
        expect: /no such case/u,
        host: "split-attributes-link"
    },
    {
        rule: "a with half belongs to one host",
        mutate: (declared) => (hostOf(declared, "split-attributes-image").with = "split-attributes-link"),
        expect: /is the with half of both/u,
        host: "split-attributes-link"
    },
    {
        rule: "a with half is extended",
        mutate: (declared) => (byName(declared, "split-attributes-code").dialect = "commonmark"),
        expect: /a with half holds a dialect production by construction/u,
        host: "split-attributes-code"
    },
    {
        rule: "a with half publishes no ratio of its own",
        mutate: (declared) =>
            declared.pairs.push({ case: "split-attributes-code", isomorph: "pair-formulablock-common" }),
        expect: /would be published with a ratio of its own/u,
        host: "split-attributes-code"
    },
    {
        rule: "a without half is not another host's with",
        mutate: (declared) => (hostOf(declared, "split-attributes-setext").without = "split-attributes-heading"),
        expect: /is itself the with half of a split/u,
        host: "split-attributes-setext"
    },
    {
        rule: "the host names the kind it decorates",
        mutate: (declared) => delete hostOf(declared, "split-attributes-autolink").kind,
        expect: /names no kind of node the remainder decorates/u,
        host: "split-attributes-autolink"
    },
    {
        rule: "both halves declare they build that kind",
        mutate: (declared) => (hostOf(declared, "split-attributes-autolink").kind = "Heading"),
        expect: /decorates Heading nodes, which split-attributes-autolink does not declare it builds/u,
        host: "split-attributes-autolink"
    },
    {
        rule: "each is a count",
        mutate: (declared) => (hostOf(declared, "split-attributes-image").each = 2.5),
        expect: /declares no count of host nodes per unit/u,
        host: "split-attributes-image"
    },
    {
        rule: "a separator is whitespace",
        mutate: (declared) => (hostOf(declared, "split-attributes-heading").separator = "x"),
        expect: /spaces or tabs, or nothing/u,
        host: "split-attributes-heading"
    },
    {
        rule: "every host node carries the remainder",
        mutate: (declared) => (hostOf(declared, "split-attributes-idirective").each = 2),
        expect: /holds the remainder 3 times per unit and declares 2 host nodes/u,
        host: "split-attributes-idirective"
    },
    {
        rule: "every copy follows the host's separator",
        mutate: (declared) =>
            (byName(declared, "split-attributes-heading").counted.unit = "## Label {n}{key=value .cls}\n\n"),
        expect: /1 copies of the remainder without the separator the ATX heading host declares/u,
        host: "split-attributes-heading"
    },
    {
        rule: "the without half holds no remainder",
        mutate: (declared) =>
            (byName(declared, "pair-formulablock-common").counted.unit =
                "```{key=value .cls}\nE = mc^2 + {n}\n```\n\n"),
        expect: /pair-formulablock-common holds the remainder/u,
        host: "split-attributes-code"
    },
    {
        rule: "the with unit is the without unit with the remainder inserted",
        mutate: (declared) =>
            (byName(declared, "split-attributes-ldirective").counted.unit = "::note[Label {n}]{key=value .cls} \n\n"),
        expect: /is not pair-ldirective-dialect with the remainder inserted/u,
        host: "split-attributes-ldirective"
    },
    {
        rule: "the separator is inserted with the bytes",
        mutate: (declared) =>
            (byName(declared, "split-attributes-refdef").counted.unit =
                "See [body {n}][def-{n}] here.\n\n[def-{n}]: /target-{n}  {key=value .cls}\n\n"),
        expect: /is not host-refdef with the remainder inserted after its separator/u,
        host: "split-attributes-refdef"
    },
    {
        rule: "both halves were generated to the same units",
        mutate: (declared, units) => (units["split-attributes-image"] = 999),
        expect: /were generated to 999 and 1000 units/u,
        host: "split-attributes-image"
    }
];

for (const mutation of MUTATIONS) {
    test(`fails when broken: ${mutation.rule}`, () => {
        const declared = manifest();
        const units = equalUnits(declared);
        mutation.mutate(declared, units);
        const failures = splitManifestFailures(declared, units, states);
        const matching = failures.filter((failure) => mutation.expect.test(failure.message));
        assert.ok(
            matching.length >= 1,
            `no failure matched ${mutation.expect}; got:\n${failures.map((f) => f.message).join("\n")}`
        );
        if (mutation.breaksEveryHost) {
            assert.equal(matching[0].with, null, "a split-level failure names no host");
        } else {
            assert.equal(matching[0].with, mutation.host, "the failure names the host it belongs to");
        }
    });
}
