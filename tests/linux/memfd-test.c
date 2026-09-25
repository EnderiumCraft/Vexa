/* memfd-test: memfd_create (a file in memory, mapped shared twice and read
 * back) and eventfd (counting, semaphore mode, non-blocking, poll). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("memfd-test: FAILED: %s (errno %d)\n", what, errno);
        failures++;
    }
}

int main(void) {
    /* memfd */
    int fd = memfd_create("vexa-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    check(fd >= 0, "memfd_create");
    check(ftruncate(fd, 8192) == 0, "ftruncate");
    char *a = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    char *b = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(a != MAP_FAILED && b != MAP_FAILED, "mmap");
    if (a != MAP_FAILED && b != MAP_FAILED) {
        strcpy(a + 4100, "shared through a memfd");
        check(strcmp(b + 4100, "shared through a memfd") == 0, "the other mapping sees it");
        char buffer[32] = {0};
        check(pread(fd, buffer, 22, 4100) == 22 && strcmp(buffer, "shared through a memfd") == 0,
              "read sees it");
    }
    check(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0, "F_ADD_SEALS");
    check((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "close-on-exec");
    close(fd);

    /* eventfd */
    int e = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    check(e >= 0, "eventfd");
    uint64_t value = 0;
    check(read(e, &value, 8) == -1 && errno == EAGAIN, "empty: EAGAIN");
    struct pollfd p = {e, POLLIN, 0};
    check(poll(&p, 1, 0) == 0, "empty: not readable");
    value = 3;
    check(write(e, &value, 8) == 8 && write(e, &value, 8) == 8, "write");
    check(poll(&p, 1, 0) == 1 && (p.revents & POLLIN), "readable");
    check(read(e, &value, 8) == 8 && value == 6, "read the sum");
    close(e);

    int s = eventfd(2, EFD_SEMAPHORE);
    check(read(s, &value, 8) == 8 && value == 1 && read(s, &value, 8) == 8 && value == 1,
          "semaphore reads 1 at a time");
    close(s);

    if (!failures) {
        printf("memfd-test: passed\n");
    }
    return failures != 0;
}
