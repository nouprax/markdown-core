/**
 * What the benchmark corpus's pairs and splits mean, as functions of
 * `corpus.json` alone.
 *
 * Two scripts read the same declarations and each used to carry its own
 * reading of them: `benchmark-stages.mjs` decides which cases a `--case` run
 * must build, and `audit-corpus-reach.mjs` decides whether a split's two
 * halves are what the manifest claims. Two readings drift, and a drift here is
 * a run measuring one thing while the audit holds another. So the rules that
 * need nothing but the manifest live here once, take it as data, and are
 * tested by mutating a manifest in memory -- one mutation per rule, each
 * showing the rule fail when what it holds is broken.
 *
 * The rules that need the parser -- a dump, a census of node kinds -- stay in
 * the audit, which is the one place a parser is built for them.
 */

/** Every case that is one half of a pair, of either kind. */
export function pairHalves(manifest) {
    const paired = new Set();
    for (const declaration of [...(manifest.isomorphs ?? []), ...(manifest.logicalIsomorphs ?? [])]) {
        paired.add(declaration.case);
        paired.add(declaration.isomorph);
    }
    return paired;
}

/**
 * Whether the driver publishes a same-job ratio for a case: either half of a
 * pair, or a case whose syntax a reference reads the same way and whose tree
 * carries no referenceless field. The same test the driver applies, written
 * once so the state census, the split checks and the report cannot disagree
 * about which cases are comparisons.
 */
export function publishesRatio(entry, paired) {
    return (
        paired.has(entry.name) ||
        ((entry.dialect === "commonmark" || entry.gfm === true) && !(entry.carries ?? []).length)
    );
}

/** The cases that exist only to be the `with` half of a split's host. */
export function splitWithCases(manifest) {
    return new Set((manifest.splits ?? []).flatMap((split) => (split.hosts ?? []).map((host) => host.with)));
}

/**
 * The cases a run must build when it is asked for `names`.
 *
 * A named case drags in what it is DEFINED AGAINST, and keeps dragging until
 * nothing new arrives. A paired case drags its other half: naming one alone
 * would measure a case whose comparison lives on a document the run never
 * built. The pair is not optional context; it IS the comparison. A split's
 * `with` half drags its `without`, for the same reason, and only in that
 * direction: a pair half is a comparison on its own and a `with` document is
 * not, so naming the `without` measures the pair it belongs to and no split.
 * A counted case drags the generated case it is sized by, because a count
 * taken from a partner that was never generated is no count at all. The
 * closure is iterated rather than applied once because each step can name a
 * case the next rule has to see -- a `with` names a `without` that names a
 * pair that names a generated match.
 */
export function caseClosure(manifest, names) {
    const wanted = new Set(names);
    const byName = new Map((manifest.cases ?? []).map((entry) => [entry.name, entry]));
    for (let before = -1; before !== wanted.size;) {
        before = wanted.size;
        for (const pair of [...(manifest.isomorphs ?? []), ...(manifest.logicalIsomorphs ?? [])]) {
            if (wanted.has(pair.case)) wanted.add(pair.isomorph);
            if (wanted.has(pair.isomorph)) wanted.add(pair.case);
        }
        for (const split of manifest.splits ?? []) {
            for (const host of split.hosts ?? []) {
                if (wanted.has(host.with)) wanted.add(host.without);
            }
        }
        for (const name of [...wanted]) {
            const match = byName.get(name)?.counted?.match;
            if (match) wanted.add(match);
        }
    }
    return wanted;
}

const occurrences = (text, needle) => text.split(needle).length - 1;

/**
 * What a split claims that the manifest alone can be held to.
 *
 * A split measures a remainder INSIDE its hosts, as the difference between two
 * whole documents that differ by the remainder's bytes and nothing else. That
 * is the remainder's cost only while everything else about the two documents
 * is the same, and these are the parts of "the same" that are properties of
 * the declarations rather than of the parser:
 *
 *   The remainder names an `unpairable` proof. A split exists because a pair
 *   cannot, and a remainder nobody has proved unpairable is a pair somebody
 *   has not written. Its bytes are declared once and are the same in every
 *   host, which is what makes the rows one job.
 *
 *   The two units are the same text but for the remainder, inserted at the
 *   host's site: the `with` unit holds the bytes exactly `each` times, the
 *   `without` unit never, and deleting every copy -- together with the
 *   whitespace the host declares as its `separator`, where the grammar admits
 *   or requires some before the list -- gives the `without` unit byte for
 *   byte. The separator belongs to the site and is spaces or tabs only; the
 *   remainder itself is never allowed to differ between hosts.
 *
 *   Both halves are generated to the same count of units, which is why a
 *   `with` case is `counted` against the case its `without` is sized by; the
 *   host names the kind of node the remainder decorates, and both halves
 *   declare they build it; and `each` is how many of those one unit holds.
 *
 *   The `with` half publishes no ratio of its own: it is `extended` by
 *   construction, it is not a pair half, and its states count as bounds. The
 *   `without` half is a document in its own right -- a pair half, or a case
 *   with its own comparison -- and never another host's `with`, or the
 *   difference would be one remainder over another.
 *
 * Failures are returned rather than thrown, each naming the split by its
 * index in the manifest and the `with` case it belongs to -- or no case, when
 * a split-level claim failed and every host of that split is broken by it.
 */
export function splitManifestFailures(manifest, units, stateNames) {
    const failures = [];
    const proofs = new Set((manifest.unpairable ?? []).map((entry) => entry.production));
    const cases = new Map((manifest.cases ?? []).map((entry) => [entry.name, entry]));
    const paired = pairHalves(manifest);
    const withs = splitWithCases(manifest);
    const carriers = new Map();
    const unitOf = (entry) => (entry.generated ?? entry.counted)?.unit;
    (manifest.splits ?? []).forEach((split, index) => {
        const label = `the split for "${split.remainder}"`;
        const fail = (message) => failures.push({ split: index, with: null, message });
        if (!proofs.has(split.remainder)) {
            fail(
                `${label} names no unpairable entry of that name. A split exists because a pair cannot, ` +
                    `and a remainder nobody has proved unpairable is a pair nobody has written`
            );
        }
        if (typeof split.bytes !== "string" || !split.bytes.length) fail(`${label} declares no remainder bytes`);
        if (!Array.isArray(split.varies) || !split.varies.length) {
            fail(`${label} names no field the remainder populates, so no tree can be compared modulo it`);
        }
        for (const state of split.states ?? []) {
            if (!stateNames.has(state)) fail(`${label} names ${state}, which is not a declared grammar state`);
        }
        for (const host of split.hosts ?? []) {
            const failHost = (message) => failures.push({ split: index, with: host.with, message });
            const carrier = cases.get(host.with);
            const without = cases.get(host.without);
            if (!carrier || !without) {
                failHost(
                    `the ${host.host} host names ${host.with} and ${host.without}, and the corpus holds no such case`
                );
                continue;
            }
            if (carriers.has(host.with)) {
                failHost(
                    `${host.with} is the with half of both the ${carriers.get(host.with)} and the ${host.host} host`
                );
            }
            carriers.set(host.with, host.host);
            if (carrier.dialect !== "extended") {
                failHost(
                    `${host.with} is declared dialect ${JSON.stringify(carrier.dialect)}, and a with half holds a ` +
                        `dialect production by construction`
                );
            }
            if (paired.has(host.with) || publishesRatio(carrier, paired)) {
                failHost(
                    `${host.with} would be published with a ratio of its own, so the states only the remainder ` +
                        `reaches would count as measured while the split says they are bounds`
                );
            }
            if (withs.has(host.without)) {
                failHost(
                    `${host.without} is itself the with half of a split, and the difference against it would be ` +
                        `one remainder over another`
                );
            }
            if (typeof host.kind !== "string" || !host.kind.length) {
                failHost(`the ${host.host} host names no kind of node the remainder decorates`);
            } else {
                for (const [name, entry] of [
                    [host.with, carrier],
                    [host.without, without]
                ]) {
                    if (!(entry.builds ?? []).includes(host.kind)) {
                        failHost(
                            `the ${host.host} host decorates ${host.kind} nodes, which ${name} does not declare it builds`
                        );
                    }
                }
            }
            const carrierUnit = unitOf(carrier);
            const withoutUnit = unitOf(without);
            if (carrierUnit === undefined || withoutUnit === undefined) {
                failHost(
                    `${host.with} and ${host.without} must both be generated from a unit for their units to compare`
                );
                continue;
            }
            if (!Number.isInteger(host.each) || host.each < 1) {
                failHost(`the ${host.host} host declares no count of host nodes per unit`);
                continue;
            }
            const separator = host.separator ?? "";
            if (typeof separator !== "string" || !/^[ \t]*$/u.test(separator)) {
                failHost(
                    `the ${host.host} host declares ${JSON.stringify(host.separator)} as its separator, and a ` +
                        `separator is the whitespace the site admits before the list: spaces or tabs, or nothing`
                );
                continue;
            }
            if (typeof split.bytes !== "string" || !split.bytes.length) continue;
            const bare = occurrences(carrierUnit, split.bytes);
            if (bare !== host.each) {
                failHost(
                    `${host.with} holds the remainder ${bare} times per unit and declares ${host.each} host nodes. ` +
                        `Every host node carries the remainder, or the difference is not the remainder's cost per list`
                );
            }
            const inserted = separator + split.bytes;
            if (separator && occurrences(carrierUnit, inserted) !== bare) {
                failHost(
                    `${host.with} holds ${bare - occurrences(carrierUnit, inserted)} copies of the remainder without ` +
                        `the separator the ${host.host} host declares before it`
                );
            }
            if (occurrences(withoutUnit, split.bytes) !== 0) {
                failHost(`${host.without} holds the remainder, and the difference against it would then be nothing`);
            }
            if (carrierUnit.replaceAll(inserted, "") !== withoutUnit) {
                failHost(
                    `${host.with} is not ${host.without} with the remainder inserted` +
                        `${separator ? " after its separator" : ""}: with every copy deleted, the two units still differ`
                );
            }
            if (units[host.with] === undefined || units[host.with] !== units[host.without]) {
                failHost(
                    `${host.with} and ${host.without} were generated to ${units[host.with]} and ` +
                        `${units[host.without]} units, and the difference between them is the remainder only ` +
                        `while both hold the same number of everything else`
                );
            }
        }
    });
    return failures;
}
