#include <stdlib.h>

#include "markdown-core.h"

/* On allocation failure the element is dropped and the original list is
 * returned unchanged; callers that must not lose elements verify membership
 * afterwards (see markdown_core_parser_attach_syntax_extension). */
markdown_core_llist *markdown_core_llist_append(markdown_core_mem *mem, markdown_core_llist *head, void *data) {
    markdown_core_llist *tmp;
    markdown_core_llist *new_node = (markdown_core_llist *)mem->calloc(1, sizeof(markdown_core_llist));

    if (!new_node)
        return head;

    new_node->data = data;
    new_node->next = NULL;

    if (!head)
        return new_node;

    for (tmp = head; tmp->next; tmp = tmp->next)
        ;

    tmp->next = new_node;

    return head;
}

void markdown_core_llist_free_full(markdown_core_mem *mem, markdown_core_llist *head,
                                   markdown_core_free_func free_func) {
    markdown_core_llist *tmp, *prev;

    for (tmp = head; tmp;) {
        if (free_func)
            free_func(mem, tmp->data);

        prev = tmp;
        tmp = tmp->next;
        mem->free(prev);
    }
}

void markdown_core_llist_free(markdown_core_mem *mem, markdown_core_llist *head) {
    markdown_core_llist_free_full(mem, head, NULL);
}
