#ifndef VEXA_PIPE_H
#define VEXA_PIPE_H

#include <vexa/object.h>

/* A pipe: bytes written to one end come out of the other, in order. Reading an
 * empty pipe waits for a writer; once every write end is closed, reads return
 * 0 (end of input). Writing when every read end is closed fails with -VX_EPIPE
 * (and SIGPIPE). */

extern const struct object_type pipe_read_type;
extern const struct object_type pipe_write_type;

/* Returns 0 and the two ends, each with one reference, or -VX_ENOMEM. */
int pipe_create(struct object **read_end, struct object **write_end);

#endif
