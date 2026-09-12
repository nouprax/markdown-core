import assert from "node:assert/strict";
import { test } from "node:test";
import { outsideSharedFuzzScope } from "../lib/fuzz-scope.mjs";

test("implicit-reference compositions have an explicit independent oracle boundary", () => {
    for (const input of [
        "# Heading\n\n[Heading]\n",
        "[Heading]\n\nHeading\n===\n",
        "> # Heading\n>\n> [Heading]\n",
        "- # Heading\n\n  [Heading]\n",
        "# Heading\n\n[go][Heading]\n",
        "# Heading\n\n[Heading][]\n",
        "# Heading\n\n![[Heading]](/image)\n",
        // The boundary is conservative; label matching belongs to P4, not to
        // a second implementation inside the CommonMark fuzz harness.
        "# Heading\n\n[Different]\n",
        "### foo\nthree---        with two lines.\n[foo]: <>\n****\n    <a\n*foo _bar* baz_seven-------\n"
    ]) {
        assert.equal(outsideSharedFuzzScope(input), "implicit-heading-references", input);
    }
});

test("headings, ordinary references and opaque brackets retain differential coverage", () => {
    for (const input of [
        "# Heading\n\nprose\n",
        '"title" ok\n-\n"title" ok\nafte\nno references here\n',
        "[Heading]\n",
        "# Heading\n\n[Heading](/direct)\n",
        "# Heading\n\n[Heading]\n\n[Heading]: /explicit\n",
        "# Heading\n\n[go][r] ![alt][r]\n\n[r]: /explicit\n",
        "# Heading\n\n`[Heading]`\n",
        "# Heading\n\n    [Heading]\n",
        "# Heading\n\n~~~\n[Heading]\n~~~\n",
        '# Heading\n\n<a title="[Heading]">\n',
        "# Heading\n\n<!-- [Heading] -->\n",
        "# Heading\n\n&#91;Heading]\n",
        "~~~\n# Heading\n~~~\n\n[Heading]\n"
    ]) {
        assert.equal(outsideSharedFuzzScope(input), null, input);
    }
});

test("custom task and definition envelopes belong to their extension oracles", () => {
    for (const input of ["- [🚀]   - [ ] b\n", "1. [!] task\n"]) {
        assert.equal(outsideSharedFuzzScope(input), "custom-task-markers");
    }
    for (const input of ["Use [a]\n:\n:name[label]\n", "term\n: body\n", "term\n~ body\n"]) {
        assert.equal(outsideSharedFuzzScope(input), "definition-lists");
    }
    for (const input of [
        "- [x] task\n",
        "- [ ] task\n",
        "[🚀] prose\n",
        "- [ab] text\n",
        "    term\n    : body\n",
        "~~~\n- [🚀] task\nterm\n: body\n~~~\n",
        "term\n\\: literal\n"
    ]) {
        assert.equal(outsideSharedFuzzScope(input), null, input);
    }
});
