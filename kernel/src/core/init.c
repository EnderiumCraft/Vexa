#include <vexa/cmdline.h>
#include <vexa/fs.h>
#include <vexa/keyboard.h>
#include <vexa/object.h>
#include <vexa/process.h>
#include <vexa/tty.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/modules.h>
#include <vexa/monitor.h>
#include <vexa/sched.h>
#include <vexa/storage.h>
#include <vexa/string.h>
#include <vexa/vfs.h>

/* The first kernel thread: sets up file systems and devices, which can mean
 * waiting for a disk and so can't happen on a CPU's idle thread, then starts
 * the monitor. */

static void must(int error, const char *what) {
    if (error) {
        panic("init: %s failed: %s", what, vfs_error_name(error));
    }
}

static void print_file(const char *path) {
    struct file *file;
    if (vfs_open(path, strlen(path), VX_OPEN_READ, &file)) {
        return;
    }
    char buffer[512];
    int64_t n;
    while ((n = vfs_read(file, buffer, sizeof(buffer))) > 0) {
        kwrite(buffer, n);
    }
    vfs_close(file);
}

/* Starts /bin/vinit, the first user program, with the terminal as its input
 * and output. Returns false if it couldn't. */
static bool start_vinit(void) {
    struct file *console;
    if (vfs_open("/dev/console", 12, VX_OPEN_READ | VX_OPEN_WRITE, &console)) {
        return false;
    }
    char *argv[] = {"/bin/vinit"};
    char *envp[] = {"PATH=/bin", "HOME=/", "TERM=vt100"};
    struct spawn_request request = {
        .path = "/bin/vinit", .argv = argv, .argc = 1, .envp = envp, .envc = 3,
    };
    for (int i = 0; i < 3; i++) {
        request.handles[i] = &console->object;
        request.rights[i] = HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE;
    }
    int error;
    const char *reason;
    struct process *vinit = process_spawn(&request, &error, &reason);
    vfs_close(console);
    if (!vinit) {
        kprintf("[init] cannot start /bin/vinit: %s\n", reason);
        return false;
    }
    tty_set_foreground(vinit->group);
    object_put(&vinit->object);
    return true;
}

void init_thread(void *unused) {
    (void)unused;
    fs_init();
    must(vfs_mount("tmpfs", NULL, "tmpfs", "/"), "mounting the root file system");

    const struct boot_module *initramfs = module_find("initramfs.tar");
    if (initramfs) {
        initramfs_unpack(initramfs->data, initramfs->size);
    } else {
        kprintf("[init] no initramfs.tar from the bootloader; the root file system is empty\n");
    }
    vfs_mkdir("/dev", 4);
    must(vfs_mount("devfs", NULL, "devfs", "/dev"), "mounting /dev");
    vfs_mkdir("/mnt", 4);
    vfs_mkdir("/tmp", 4);

    storage_init();

    kprintf("\nVexa kernel initialized.\n");
    /* Normally the rest of the system is user programs, starting with vinit.
     * The `monitor` boot option (or a missing vinit) gives the kernel monitor. */
    if (!cmdline_has("monitor")) {
        tty_init();
        tty_attach_keyboard();
        if (start_vinit()) {
            return;
        }
        keyboard_set_consumer(NULL);
    }
    print_file("/etc/motd");
    if (!thread_create("monitor", monitor_thread, NULL)) {
        panic("could not start the monitor");
    }
}
