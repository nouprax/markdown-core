/* libFuzzer target for document appends: every input is an append script
 * (format in tests/support/append_replay.h) replayed with full per-append
 * verification — dump equality against a one-shot parse of the shadow text,
 * identity-ledger integrity, and footnote-query equivalence when the script
 * enables footnotes.  Any verification failure aborts, so the fuzzer keeps
 * the input as a crash reproducer; replay one with
 * `fuzz_smoke_runner --script FILE`. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "append_replay.h"

static void fuzz_report(void *user, const char *context, const char *message) {
    (void)user;
    (void)context;
    fprintf(stderr, "document append script failed verification: %s\n", message);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)er_script_replay(data, size, "fuzz_document_appends", fuzz_report, NULL);
    return 0;
}
