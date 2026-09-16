/**
 * WHAT A CALLGRIND MEASUREMENT IS ISOLATED FROM, in one place.
 *
 * Two drivers measure under callgrind -- `benchmark-stages.mjs` for the
 * document parse and `benchmark-attributes.mjs` for the attribute grammar --
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
