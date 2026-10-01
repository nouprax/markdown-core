/* 4.11 of the incremental gates: the input is a document and an edit script
 * (tests/runners/incremental_fuzz.c), and the session subject must pass the
 * oracles after every step, in both units. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "incremental_oracles.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    run state;
    memset(&state, 0, sizeof(state));
    state.subject = &eh_session;
    check_script_bytes(&state, data, size);
    if (state.failures) {
        abort();
    }
    return 0;
}
