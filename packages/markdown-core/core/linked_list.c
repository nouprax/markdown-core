#include <stdlib.h>

#include "alloc.h"
#include "markdown-core.h"

void markdown_core_llist_free_full(markdown_core_llist *head, markdown_core_free_func free_func) {
    markdown_core_llist *tmp, *prev;

    for (tmp = head; tmp;) {
        if (free_func) {
            free_func(tmp->data);
        }

        prev = tmp;
        tmp = tmp->next;
        markdown_core_free(prev);
    }
}

void markdown_core_llist_free(markdown_core_llist *head) { markdown_core_llist_free_full(head, NULL); }
