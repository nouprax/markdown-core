#include <stdlib.h>

#include "markdown-core.h"

void markdown_core_llist_free_full(
    markdown_core_mem *mem,
    markdown_core_llist *head,
    markdown_core_free_func free_func
) {
    markdown_core_llist *tmp, *prev;

    for (tmp = head; tmp;) {
        if (free_func) {
            free_func(mem, tmp->data);
        }

        prev = tmp;
        tmp = tmp->next;
        mem->free(prev);
    }
}

void markdown_core_llist_free(markdown_core_mem *mem, markdown_core_llist *head) {
    markdown_core_llist_free_full(mem, head, NULL);
}
