/* libFuzzer target for incremental sessions: every input is an edit script
 * (format in tests/support/session_replay.h) replayed with full per-commit
 * verification — dump equality against a one-shot parse of the shadow text,
 * delta-mirror integrity, and footnote-query equivalence when the script
 * enables footnotes.  Any verification failure aborts, so the fuzzer keeps
 * the input as a crash reproducer; replay one with
 * `fuzz_smoke_runner --script FILE`. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "session_replay.h"

static void fuzz_report(void *user, const char *context, const char *message) {
    (void)user;
    (void)context;
    fprintf(stderr, "session edit script failed verification: %s\n", message);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)sr_script_replay(data, size, "fuzz_session_edits", fuzz_report, NULL);
    return 0;
}
