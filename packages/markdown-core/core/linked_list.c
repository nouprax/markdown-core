#include <stdlib.h>

#include "markdown-core.h"

void markdown_core_llist_free(markdown_core_mem *mem, markdown_core_llist *head) {
    markdown_core_llist *tmp, *prev;

    for (tmp = head; tmp;) {
        prev = tmp;
        tmp = tmp->next;
        mem->free(mem, prev);
    }
}
