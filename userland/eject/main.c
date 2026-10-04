/* eject: takes a disk's file systems out of /mnt, so it can be unplugged or
 * written over: eject usb0 (or /dev/usb0, or /mnt/usb0). */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vexa/syscall.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: eject DISK (usb0, sda1...)\n");
        return 2;
    }
    const char *name = argv[1];
    if (!strncmp(name, "/dev/", 5) || !strncmp(name, "/mnt/", 5)) {
        name += 5;
    }
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "eject: there's no disk %s\n", name);
        return 1;
    }
    long error = vx_control(fd, VX_BLOCK_EJECT, NULL, 0);
    close(fd);
    if (error) {
        fprintf(stderr, "eject: %s: %s\n", name,
                error == -VX_EBUSY ? "the system is on it" : vx_strerror(error));
        return 1;
    }
    printf("%s can be unplugged now\n", name);
    return 0;
}
