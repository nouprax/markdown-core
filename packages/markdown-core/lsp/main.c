#include "adapter.h"
#include "frame.h"

#include <stdio.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/*
 * markdown-core-server: the engine on stdio, for editors that speak the
 * protocol on the wire (plan, section 4.2). It reads one message, answers
 * it, and reads the next, until `exit` or the end of the input.
 */
int main(void) {
#ifdef _WIN32
    /* The framing counts bytes, so neither stream may translate line ends. */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    adapter self;
    if (!adapter_open(&self)) {
        fputs("markdown-core-server: the server cannot be allocated\n", stderr);
        return 1;
    }
    frame message = {NULL, 0, 0};
    int code = 1;
    for (;;) {
        frame_status status = frame_read(stdin, &message);
        if (status == FRAME_END) {
            code = adapter_code(&self);
            break;
        }
        if (status == FRAME_BROKEN) {
            fputs("markdown-core-server: the input is not framed as the protocol frames it\n", stderr);
            break;
        }
        adapter_handle(&self, message.body, message.size);
        if (self.out.failed) {
            fputs("markdown-core-server: an answer cannot be allocated\n", stderr);
            break;
        }
        if (self.out.size && !frame_write(stdout, self.out.data, self.out.size)) {
            fputs("markdown-core-server: an answer cannot be written\n", stderr);
            break;
        }
        if (self.exited) {
            code = adapter_code(&self);
            break;
        }
    }
    frame_release(&message);
    adapter_close(&self);
    return code;
}
