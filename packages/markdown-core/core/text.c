#include <string.h>

#include "text.h"

void markdown_core_text_init(markdown_core_text *text, markdown_core_mem *mem) {
    text->mem = mem;
    text->data = NULL;
    text->length = 0;
    text->alloc = 0;
}

void markdown_core_text_release(markdown_core_text *text) {
    if (text->data) {
        text->mem->free(text->data);
    }
    text->data = NULL;
    text->length = 0;
    text->alloc = 0;
}

bool markdown_core_text_edit(markdown_core_text *text, size_t start, size_t end, const unsigned char *bytes,
                             size_t length) {
    if (start > end || end > text->length) {
        return false;
    }
    if (length > 0 && bytes == NULL) {
        return false;
    }

    size_t removed = end - start;
    size_t kept = text->length - removed;
    if (length > SIZE_MAX - kept) {
        return false; // total length would overflow size_t
    }
    size_t new_length = kept + length;

    if (new_length > text->alloc) {
        size_t new_alloc = text->alloc ? text->alloc : 256;
        while (new_alloc < new_length) {
            if (new_alloc > SIZE_MAX / 2) {
                new_alloc = new_length;
                break;
            }
            new_alloc *= 2;
        }
        unsigned char *grown = (unsigned char *)text->mem->realloc(text->data, new_alloc);
        if (!grown) {
            return false;
        }
        text->data = grown;
        text->alloc = new_alloc;
    }

    if (end < text->length && removed != length) {
        memmove(text->data + start + length, text->data + end, text->length - end);
    }
    if (length > 0) {
        memcpy(text->data + start, bytes, length);
    }
    text->length = new_length;
    return true;
}
