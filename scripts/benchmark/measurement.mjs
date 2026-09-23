/**
 * WHAT A CALLGRIND MEASUREMENT IS ISOLATED FROM, in one place.
 *
 * Two drivers measure under callgrind -- `run.mjs` for the
 * document parse and `measure-attributes.mjs` for the attribute grammar --
 * and the isolation below is load-bearing for both. It lives here rather than
 * in either of them because a second copy is a copy that drifts, and the
 * failure mode of a drifted copy is a number that still looks plausible.
 *
 * THE ENVIRONMENT REACHES INSIDE A MEASUREMENT through several layers at once.
 * The loader reads `LD_PRELOAD` at exec time, glibc reads `GLIBC_TUNABLES` and
 * `MALLOC_PERTURB_` when it allocates, and libc reads the locale when it
 * classifies a byte -- and a parse allocates and calls libc constantly, so none
 * of this is a rounding difference. Against one case's 35,066,966 Ir,
 * `MALLOC_PERTURB_=42` alone takes the summary to 51,807,936.
 *
 * Hence an allowlist and not a list of variables to remove: a denylist has to
 * name every mechanism that can reach in, those are three of them in three
 * different layers, and the next one would be admitted silently. The locale is
 * SET rather than dropped, because there is no "no locale" -- libc falls back
 * to C either way, so naming it makes the measurement state its locale instead
 * of depending on the caller not having one.
 *
 * Valgrind sets its own loader variables for the client, so it is undisturbed.
 */

import fs from "node:fs";
import path from "node:path";

/* The cache model both drivers measure against. It is a model and not this
 * host's cache: a number that moved because the runner had a different L3 is
 * not a number about either parser. */
export const CACHE = ["--I1=32768,8,64", "--D1=32768,8,64", "--LL=8388608,16,64"];

/**
 * The profiler's own configuration is isolated too, not just the environment.
 *
 * Valgrind takes options from `~/.valgrindrc`, then `VALGRIND_OPTS`, then
 * `./.valgrindrc`, before its command line -- so every option a driver does not
 * pass explicitly is the caller's to set, and the ones that matter most are
 * exactly the ones not passed. A home directory rc file containing
 * `--collect-atstart=no` takes a measurement's summary to 0 with the report's
 * identity table unchanged.
 *
 * `VALGRIND_OPTS` is already gone with everything else unnamed. The two rc
 * files are reached by HOME and by the working directory instead, so both point
 * at an empty directory the driver owns and neither file exists.
 */
export function measurementRoot(out, fail) {
    const directory = path.join(out, "measurement-root");
    fs.mkdirSync(directory, { recursive: true });
    const rc = path.join(directory, ".valgrindrc");
    if (fs.existsSync(rc)) fail(`${rc} would configure the profiler out from under the measurement`);
    return directory;
}

export function measurementEnvironment(root) {
    /* PATH is the only thing carried across: it is how `valgrind` is found. */
    const environment = { LC_ALL: "C", LANG: "C", HOME: root, TMPDIR: root };
    if (process.env.PATH !== undefined) environment.PATH = process.env.PATH;
    return environment;
}

/**
 * The build's environment is built rather than inherited, for the reason the
 * measurement's is.
 *
 * A compiler reads more than its command line. `CPATH` and `C_INCLUDE_PATH` add
 * include directories that appear on no compile line at all, so a header can be
 * swapped underneath a build while `compile_commands.json` -- which is where the
 * identity reads the engines' real options -- shows character-for-character the
 * same command. Verified: a project compiled against an injected `injected.h`
 * through `CPATH`, and its compile command mentioned no such directory.
 *
 * Only what a build needs is carried across, plus the two variables the
 * identity deliberately honours and records. Everything else is absent by
 * construction, which is the half of this that was missing: the measurement got
 * an allowlist earlier and the build that produced it did not.
 */
/* The two the identity deliberately honours and records, so the two whose
 * contents have to be visible in it. */
export const BUILD_FLAG_VARIABLES = ["CFLAGS", "LDFLAGS"];
const BUILD_VARIABLES = ["PATH", "HOME", "TMPDIR", ...BUILD_FLAG_VARIABLES];

export function buildEnvironment() {
    const environment = { LC_ALL: "C", LANG: "C" };
    for (const name of BUILD_VARIABLES) {
        if (process.env[name] !== undefined) environment[name] = process.env[name];
    }
    return environment;
}
