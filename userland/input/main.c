/* input: lists the input devices, or shows what one of them reports.
 *
 *   input                 the devices in /dev/input
 *   input watch N [count] events from /dev/input/eventN (count of them, or until Ctrl-C)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>

static const char *type_name(unsigned type) {
    switch (type) {
    case VX_EV_SYN: return "sync";
    case VX_EV_KEY: return "key";
    case VX_EV_REL: return "motion";
    default: return "?";
    }
}

static int watch(int number, long count) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/input/event%d", number);
    int handle = vx_open(path, VX_OPEN_READ);
    if (handle < 0) {
        fprintf(stderr, "input: %s: %s\n", path, vx_strerror(handle));
        return 1;
    }
    printf("input: watching %s\n", path);
    fflush(stdout);
    long seen = 0;
    while (count <= 0 || seen < count) {
        struct vx_input_event events[16];
        long n = vx_read(handle, events, sizeof(events));
        if (n < 0) {
            fprintf(stderr, "input: %s\n", vx_strerror(n));
            return 1;
        }
        for (long i = 0; i < n / (long)sizeof(events[0]); i++) {
            struct vx_input_event *e = &events[i];
            if (e->type == VX_EV_SYN) {
                continue;
            }
            printf("input: %s %u %d\n", type_name(e->type), e->code, e->value);
            seen++;
        }
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "watch") == 0) {
        return watch(atoi(argv[2]), argc > 3 ? atol(argv[3]) : 0);
    }
    if (argc > 1) {
        fprintf(stderr, "usage: input [watch N [count]]\n");
        return 2;
    }
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int handle = vx_open(path, VX_OPEN_READ);
        if (handle < 0) {
            break;
        }
        struct vx_input_info info;
        if (vx_control(handle, VX_INPUT_INFO, &info, sizeof(info)) == 0) {
            printf("%s: %s (%s)\n", path, info.name,
                   info.capabilities & VX_INPUT_POINTER ? "pointer" : "keys");
        }
        vx_close(handle);
    }
    return 0;
}
