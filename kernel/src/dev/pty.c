#include <vexa/abi.h>
#include <vexa/fs.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/object.h>
#include <vexa/process.h>
#include <vexa/pty.h>
#include <vexa/sched.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>
#include <vexa/tty.h>
#include <vexa/vfs.h>

/*
 * Pseudo-terminals. Opening /dev/ptmx makes a new one: that handle is the
 * master (a terminal window reads the programs' output from it and writes
 * what the user types into it), and /dev/pts/N is the terminal the programs
 * run on, with the same line editing as the console.
 *
 * A pty lives as long as its /dev/pts/N vnode: the master holds one
 * reference until it closes (which hangs the terminal up), and every open
 * /dev/pts/N file holds one.
 */

#define MAX_PTYS 64
#define OUTPUT_SIZE (64 * 1024) /* Program output not yet read by the master. */
#define WRITE_CHUNK 1024

struct pty {
    struct vnode vnode; /* /dev/pts/N */
    int number;
    struct tty *tty;
    struct spinlock lock;
    char output[OUTPUT_SIZE];
    size_t head, tail, used;
    bool master_open;
    int slaves_open, slaves_ever;
    struct wait_queue master_readers; /* Waiting for output. */
    struct wait_queue writers;        /* Waiting for room in `output`. */
};

static struct spinlock table_lock = SPINLOCK_INIT;
static struct pty *ptys[MAX_PTYS];
static const struct vnode_ops slave_ops;

/* ---- The terminal's output, for the master ---- */

static void to_master(struct tty *tty, const char *text, size_t length) {
    struct pty *pty = tty_owner(tty);
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    for (size_t i = 0; i < length && pty->used < OUTPUT_SIZE; i++) {
        pty->output[pty->head] = text[i];
        pty->head = (pty->head + 1) % OUTPUT_SIZE;
        pty->used++;
    }
    spin_unlock_irqrestore(&pty->lock, flags);
    wait_queue_wake_all(&pty->master_readers);
}

static bool output_waiting(void *arg) {
    struct pty *pty = arg;
    return pty->used > 0 || (pty->slaves_ever && pty->slaves_open == 0);
}

/* ---- /dev/ptmx: the master ---- */

static int master_open(struct file *file) {
    struct pty *pty = kzalloc(sizeof(*pty));
    if (!pty) {
        return -VX_ENOMEM;
    }
    pty->tty = tty_create(to_master, pty, 24, 80);
    uint64_t flags = spin_lock_irqsave(&table_lock);
    int number = -1;
    for (int i = 0; i < MAX_PTYS && number < 0; i++) {
        if (!ptys[i]) {
            number = i;
            ptys[i] = pty;
        }
    }
    spin_unlock_irqrestore(&table_lock, flags);
    if (!pty->tty || number < 0) {
        int error = pty->tty ? -VX_EBUSY : -VX_ENOMEM;
        if (number >= 0) {
            ptys[number] = NULL;
        }
        if (pty->tty) {
            tty_destroy(pty->tty);
        }
        kfree(pty);
        return error;
    }
    pty->number = number;
    pty->master_open = true;
    vnode_init(&pty->vnode, file->vnode->mount, VX_TYPE_CHAR_DEVICE, &slave_ops);
    pty->vnode.inode = 0x1000 + (uint64_t)number;
    pty->vnode.mode = 0620;
    pty->vnode.modified = time_now();
    file->private = pty;
    return 0;
}

static void master_close(struct file *file) {
    struct pty *pty = file->private;
    uint64_t flags = spin_lock_irqsave(&table_lock);
    ptys[pty->number] = NULL; /* /dev/pts/N is gone for new opens. */
    spin_unlock_irqrestore(&table_lock, flags);
    pty->master_open = false;
    tty_hang_up(pty->tty);
    wait_queue_wake_all(&pty->writers);
    vfs_lock();
    vnode_put(&pty->vnode);
    vfs_unlock();
}

static int64_t master_read(struct file *file, void *buffer, size_t size) {
    struct pty *pty = file->private;
    if (size == 0) {
        return 0;
    }
    if (!output_waiting(pty)) {
        if (file->object.flags & OBJECT_NONBLOCK) {
            return -VX_EAGAIN;
        }
        int error = wait_queue_wait_interruptible(&pty->master_readers, output_waiting, pty);
        if (error) {
            return error;
        }
    }
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    size_t n = 0;
    while (n < size && pty->used) {
        ((char *)buffer)[n++] = pty->output[pty->tail];
        pty->tail = (pty->tail + 1) % OUTPUT_SIZE;
        pty->used--;
    }
    spin_unlock_irqrestore(&pty->lock, flags);
    wait_queue_wake_all(&pty->writers);
    return (int64_t)n; /* 0: every program on the terminal has closed it. */
}

static int64_t master_write(struct file *file, const void *buffer, size_t size) {
    struct pty *pty = file->private;
    for (size_t i = 0; i < size; i++) {
        tty_receive(pty->tty, ((const char *)buffer)[i]);
    }
    return (int64_t)size;
}

static uint32_t master_poll(struct file *file) {
    struct pty *pty = file->private;
    uint32_t ready = OBJECT_WRITABLE;
    if (pty->used) {
        ready |= OBJECT_READABLE;
    }
    if (pty->slaves_ever && pty->slaves_open == 0) {
        ready |= OBJECT_READABLE | OBJECT_HANGUP;
    }
    return ready;
}

static int size_control(struct tty *tty, uint32_t request, void *arg, size_t size) {
    struct vx_tty_size *s = arg;
    if (size < sizeof(*s)) {
        return -VX_EINVAL;
    }
    if (request == VX_TTY_SET_SIZE) {
        tty_set_window_size(tty, s->rows, s->columns);
    } else {
        tty_window_size(tty, &s->rows, &s->columns);
    }
    return 0;
}

static int master_control(struct file *file, uint32_t request, void *arg, size_t size) {
    struct pty *pty = file->private;
    switch (request) {
    case VX_TTY_PTY_NUMBER:
        if (size < sizeof(int)) {
            return -VX_EINVAL;
        }
        *(int *)arg = pty->number;
        return 0;
    case VX_TTY_SET_SIZE:
    case VX_TTY_GET_SIZE:
        return size_control(pty->tty, request, arg, size);
    default:
        return -VX_ENOTTY;
    }
}

static const struct vnode_ops master_ops = {
    .open = master_open,
    .close = master_close,
    .file_read = master_read,
    .file_write = master_write,
    .file_poll = master_poll,
    .control = master_control,
};

/* ---- /dev/pts/N: the terminal ---- */

static struct pty *pty_of(struct vnode *vnode) {
    return (struct pty *)vnode;
}

static int slave_open(struct file *file) {
    struct pty *pty = pty_of(file->vnode);
    if (!pty->master_open) {
        return -VX_EIO;
    }
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    pty->slaves_open++;
    pty->slaves_ever++;
    spin_unlock_irqrestore(&pty->lock, flags);
    /* As on Linux, the leader of a new session (here: a group leader) that
     * opens the terminal gets it: it becomes the process's /dev/tty, and its
     * group the foreground. That's what a terminal emulator's child shell
     * expects. */
    struct process *process = process_current();
    if (process && process->id == process->group) {
        pty_make_controlling(file);
    } else if (process && !tty_foreground(pty->tty)) {
        tty_set_foreground(pty->tty, process->group);
    }
    return 0;
}

static void slave_close(struct file *file) {
    struct pty *pty = pty_of(file->vnode);
    uint64_t flags = spin_lock_irqsave(&pty->lock);
    pty->slaves_open--;
    spin_unlock_irqrestore(&pty->lock, flags);
    wait_queue_wake_all(&pty->master_readers); /* Maybe the end. */
}

static int64_t slave_read(struct file *file, void *buffer, size_t size) {
    return tty_read(pty_of(file->vnode)->tty, buffer, size,
                    file->object.flags & OBJECT_NONBLOCK);
}

static bool room_to_write(void *arg) {
    struct pty *pty = arg;
    /* Room for a chunk even if every newline doubles. */
    return OUTPUT_SIZE - pty->used >= 2 * WRITE_CHUNK || !pty->master_open;
}

static int64_t slave_write(struct file *file, const void *buffer, size_t size) {
    struct pty *pty = pty_of(file->vnode);
    size_t done = 0;
    while (done < size) {
        if (!room_to_write(pty)) {
            if (file->object.flags & OBJECT_NONBLOCK) {
                return done ? (int64_t)done : -VX_EAGAIN;
            }
            int error = wait_queue_wait_interruptible(&pty->writers, room_to_write, pty);
            if (error) {
                return done ? (int64_t)done : error;
            }
        }
        size_t n = size - done < WRITE_CHUNK ? size - done : WRITE_CHUNK;
        int64_t result = tty_write(pty->tty, (const char *)buffer + done, n);
        if (result < 0) {
            return done ? (int64_t)done : result;
        }
        done += n;
    }
    return (int64_t)done;
}

static uint32_t slave_poll(struct file *file) {
    return tty_poll(pty_of(file->vnode)->tty);
}

static int slave_control(struct file *file, uint32_t request, void *arg, size_t size) {
    struct pty *pty = pty_of(file->vnode);
    if (request == VX_TTY_SET_SIZE || request == VX_TTY_GET_SIZE) {
        return size_control(pty->tty, request, arg, size);
    }
    return -VX_ENOTTY;
}

static void slave_release(struct vnode *vnode) {
    struct pty *pty = pty_of(vnode);
    tty_destroy(pty->tty);
    kfree(pty);
}

static const struct vnode_ops slave_ops = {
    .open = slave_open,
    .close = slave_close,
    .file_read = slave_read,
    .file_write = slave_write,
    .file_poll = slave_poll,
    .control = slave_control,
    .release = slave_release,
};

/* ---- /dev/pts ---- */

static int pts_lookup(struct vnode *dir, const char *name, size_t length, struct vnode **out) {
    if (length == 2 && name[0] == '.' && name[1] == '.') {
        return devfs_parent(dir, out);
    }
    int number = 0;
    if (length == 0 || length > 3) {
        return -VX_ENOENT;
    }
    for (size_t i = 0; i < length; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return -VX_ENOENT;
        }
        number = number * 10 + (name[i] - '0');
    }
    uint64_t flags = spin_lock_irqsave(&table_lock);
    struct pty *pty = number < MAX_PTYS ? ptys[number] : NULL;
    if (pty) {
        vnode_ref(&pty->vnode);
    }
    spin_unlock_irqrestore(&table_lock, flags);
    if (!pty) {
        return -VX_ENOENT;
    }
    *out = &pty->vnode;
    return 0;
}

static int pts_read_dir(struct vnode *dir, uint64_t *cookie, struct vx_dir_entry *entry) {
    (void)dir;
    for (; *cookie < MAX_PTYS; (*cookie)++) {
        uint64_t flags = spin_lock_irqsave(&table_lock);
        struct pty *pty = ptys[*cookie];
        spin_unlock_irqrestore(&table_lock, flags);
        if (pty) {
            ksnprintf(entry->name, sizeof(entry->name), "%lu", *cookie);
            entry->name_length = (unsigned)strlen(entry->name);
            entry->type = VX_TYPE_CHAR_DEVICE;
            entry->inode = 0x1000 + *cookie;
            (*cookie)++;
            return 1;
        }
    }
    return 0;
}

static const struct vnode_ops pts_ops = {
    .lookup = pts_lookup,
    .read_dir = pts_read_dir,
};

struct tty *pty_terminal(struct file *file) {
    if (file->vnode->ops == &slave_ops) {
        return pty_of(file->vnode)->tty;
    }
    if (file->vnode->ops == &master_ops) {
        return ((struct pty *)file->private)->tty;
    }
    return NULL;
}

bool pty_make_controlling(struct file *file) {
    struct process *process = process_current();
    if (file->vnode->ops != &slave_ops || !process) {
        return false;
    }
    vnode_ref(file->vnode);
    struct vnode *old = __atomic_exchange_n(&process->terminal, file->vnode, __ATOMIC_ACQ_REL);
    if (old) {
        vnode_put(old);
    }
    tty_set_foreground(pty_of(file->vnode)->tty, process->group);
    return true;
}

bool pty_is_master(struct file *file) {
    return file->vnode->ops == &master_ops;
}

void pty_init(void) {
    devfs_add("ptmx", &master_ops, NULL);
    devfs_add_directory("pts", &pts_ops);
}
