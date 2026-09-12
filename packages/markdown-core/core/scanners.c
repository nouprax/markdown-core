#include "scanners.h"

bufsize_t _ext_scan_at(bufsize_t (*scanner)(const unsigned char *, const unsigned char *), const unsigned char *input,
                       int length, bufsize_t offset) {
    if (!input || offset < 0 || offset >= length) {
        return 0;
    }
    return scanner(input + offset, input + length);
}
bufsize_t _scan_at(bufsize_t (*scanner)(const unsigned char *, const unsigned char *), const markdown_core_chunk *input,
                   bufsize_t offset) {
    return _ext_scan_at(scanner, input->data, input->len, offset);
}
