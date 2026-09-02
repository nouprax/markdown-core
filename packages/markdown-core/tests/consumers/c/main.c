#include <markdown_core.h>

#include <string.h>

int main(void) {
    const char *source = "# installed consumer\n";
    markdown_core_error *error = NULL;
    markdown_core_document *document =
        markdown_core_document_parse((const uint8_t *)source, strlen(source), NULL, &error);
    const markdown_core_node *root;

    if (document == NULL || error != NULL) {
        markdown_core_error_free(error);
        return 1;
    }
    root = markdown_core_document_root(document);
    if (root == NULL || markdown_core_node_get_kind(root) != MARKDOWN_CORE_KIND_DOCUMENT ||
        markdown_core_node_child_count(root) != 1) {
        markdown_core_document_free(document);
        return 2;
    }
    markdown_core_document_free(document);
    return 0;
}
