import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { compiledFlags, sameCompileOptions } from "../lib/compile-identity.mjs";

function compilerDatabase(t) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "compile-identity-"));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    return (units) => {
        const entries = Object.entries(units).map(([file, flags]) => {
            const output = `CMakeFiles/engine.dir/${file}.o`;
            return {
                directory: root,
                file: path.join(root, file),
                output,
                command: `/usr/bin/cc -I${root}/include ${flags} -o ${output} -c ${path.join(root, file)}`
            };
        });
        fs.writeFileSync(path.join(root, "compile_commands.json"), JSON.stringify(entries));
        return compiledFlags(root, root, "engine", (message) => {
            throw new Error(message);
        });
    };
}

test("source inventory changes preserve compile compatibility and remain visible as provenance", (t) => {
    const read = compilerDatabase(t);
    const base = read({ "core/a.c": "-O3 -DNDEBUG", "core/b.c": "-O3 -DNDEBUG" });
    for (const units of [
        { "core/a.c": "-O3 -DNDEBUG" },
        { "core/a.c": "-O3 -DNDEBUG", "core/b.c": "-O3 -DNDEBUG", "core/new.c": "-O3 -DNDEBUG" },
        { "core/a.c": "-O3 -DNDEBUG", "moved/b.c": "-O3 -DNDEBUG" }
    ]) {
        const changed = read(units);
        assert.equal(sameCompileOptions(base, changed), true);
        assert.equal(sameCompileOptions(changed, base), true);
        assert.notEqual(base.digest, changed.digest);
        assert.equal(changed.units, Object.keys(units).length);
        assert.deepEqual(Object.keys(changed.bySource), Object.keys(units).sort());
    }
});

test("one unit's changed options cannot hide behind an unchanged option union", (t) => {
    const read = compilerDatabase(t);
    const base = read({ "a.c": "-O3", "b.c": "-O3 -fPIC", "c.c": "-O3" });
    const changed = read({ "a.c": "-O3 -fPIC", "b.c": "-O3 -fPIC", "c.c": "-O3" });
    assert.deepEqual(base.distinct, changed.distinct);
    assert.equal(sameCompileOptions(base, changed), false);
    assert.equal(sameCompileOptions(changed, base), false);
});

test("new option sets, option arguments and option order remain comparison boundaries", (t) => {
    const read = compilerDatabase(t);
    for (const [before, after] of [
        [{ "a.c": "-O3" }, { "a.c": "-O3", "new.c": "-O0" }],
        [{ "a.c": "-O3 -imacros first.c" }, { "a.c": "-O3 -imacros second.c" }],
        [{ "a.c": "-O0 -O3" }, { "a.c": "-O3 -O0" }]
    ]) {
        assert.equal(sameCompileOptions(read(before), read(after)), false);
    }
});
