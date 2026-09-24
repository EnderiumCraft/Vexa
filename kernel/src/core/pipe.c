#include <vexa/abi.h>
#include <vexa/mm.h>
#include <vexa/pipe.h>
#include <vexa/process.h>
#include <vexa/sched.h>
#include <vexa/signal.h>
#include <vexa/spinlock.h>

#define PIPE_SIZE (64 * 1024)

struct pipe {
    struct spinlock lock;
    uint8_t *buffer;
    size_t head, tail, used;
    int readers, writers; /* Open ends. */
    struct wait_queue can_read, can_write;
};

struct pipe_end {
    struct object object;
    struct pipe *pipe;
};

static void pipe_free_if_unused(struct pipe *pipe) {
    uint64_t flags = spin_lock_irqsave(&pipe->lock);
    bool unused = pipe->readers == 0 && pipe->writers == 0;
    spin_unlock_irqrestore(&pipe->lock, flags);
    if (unused) {
        pmm_free(virt_to_phys(pipe->buffer), 4); /* 16 pages = 64 KiB */
        kfree(pipe);
    }
}

static void read_end_destroy(struct object *object) {
    struct pipe *pipe = ((struct pipe_end *)object)->pipe;
    uint64_t flags = spin_lock_irqsave(&pipe->lock);
    pipe->readers--;
    spin_unlock_irqrestore(&pipe->lock, flags);
    wait_queue_wake_all(&pipe->can_write); /* Writers now get EPIPE. */
    kfree(object);
    pipe_free_if_unused(pipe);
}

static void write_end_destroy(struct object *object) {
    struct pipe *pipe = ((struct pipe_end *)object)->pipe;
    uint64_t flags = spin_lock_irqsave(&pipe->lock);
    pipe->writers--;
    spin_unlock_irqrestore(&pipe->lock, flags);
    wait_queue_wake_all(&pipe->can_read); /* Readers now see the end. */
    kfree(object);
    pipe_free_if_unused(pipe);
}

static bool readable(void *arg) {
    struct pipe *pipe = arg;
    return pipe->used > 0 || pipe->writers == 0;
}

static bool writable(void *arg) {
    struct pipe *pipe = arg;
    return pipe->used < PIPE_SIZE || pipe->readers == 0;
}

static int64_t pipe_read(struct object *object, void *buffer, size_t size) {
    struct pipe *pipe = ((struct pipe_end *)object)->pipe;
    if (size == 0) {
        return 0;
    }
    int error = wait_queue_wait_interruptible(&pipe->can_read, readable, pipe);
    if (error) {
        return error;
    }
    uint64_t flags = spin_lock_irqsave(&pipe->lock);
    size_t n = 0;
    while (n < size && pipe->used > 0) {
        ((uint8_t *)buffer)[n++] = pipe->buffer[pipe->tail];
        pipe->tail = (pipe->tail + 1) % PIPE_SIZE;
        pipe->used--;
    }
    spin_unlock_irqrestore(&pipe->lock, flags);
    wait_queue_wake_all(&pipe->can_write);
    return (int64_t)n;
}

static int64_t pipe_write(struct object *object, const void *buffer, size_t size) {
    struct pipe *pipe = ((struct pipe_end *)object)->pipe;
    size_t done = 0;
    while (done < size) {
        int error = wait_queue_wait_interruptible(&pipe->can_write, writable, pipe);
        if (error) {
            return done ? (int64_t)done : error;
        }
        uint64_t flags = spin_lock_irqsave(&pipe->lock);
        if (pipe->readers == 0) {
            spin_unlock_irqrestore(&pipe->lock, flags);
            struct process *process = process_current();
            if (process) {
                signal_send(process, VX_SIGPIPE);
            }
            return done ? (int64_t)done : -VX_EPIPE;
        }
        while (done < size && pipe->used < PIPE_SIZE) {
            pipe->buffer[pipe->head] = ((const uint8_t *)buffer)[done++];
            pipe->head = (pipe->head + 1) % PIPE_SIZE;
            pipe->used++;
        }
        spin_unlock_irqrestore(&pipe->lock, flags);
        wait_queue_wake_all(&pipe->can_read);
    }
    return (int64_t)done;
}

const struct object_type pipe_read_type = {
    .name = "pipe (read end)",
    .destroy = read_end_destroy,
    .read = pipe_read,
};

const struct object_type pipe_write_type = {
    .name = "pipe (write end)",
    .destroy = write_end_destroy,
    .write = pipe_write,
};

int pipe_create(struct object **read_end, struct object **write_end) {
    struct pipe *pipe = kzalloc(sizeof(*pipe));
    struct pipe_end *reader = kzalloc(sizeof(*reader));
    struct pipe_end *writer = kzalloc(sizeof(*writer));
    uint64_t buffer = pipe && reader && writer ? pmm_alloc(4) : 0;
    if (!buffer) {
        kfree(pipe);
        kfree(reader);
        kfree(writer);
        return -VX_ENOMEM;
    }
    pipe->buffer = phys_to_virt(buffer);
    pipe->readers = pipe->writers = 1;
    object_init(&reader->object, &pipe_read_type);
    object_init(&writer->object, &pipe_write_type);
    reader->pipe = writer->pipe = pipe;
    *read_end = &reader->object;
    *write_end = &writer->object;
    return 0;
}
