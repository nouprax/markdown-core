/* THIS FILE DEFINES THREE FUNCTIONS AND NOTHING ELSE. See `alloc.h`: the test
 * suite substitutes these by defining them itself, which works only while this
 * archive member has no other symbol the library needs. A fourth definition
 * here -- a helper, a counter, a string -- would make the member get pulled
 * and turn the substitution into a duplicate-definition error. */
#include "alloc.h"

#include <stdlib.h>

void *markdown_core_alloc(size_t count, size_t size) { return calloc(count, size); }

void *markdown_core_realloc(void *pointer, size_t size) { return realloc(pointer, size); }

void markdown_core_free(void *pointer) { free(pointer); }
