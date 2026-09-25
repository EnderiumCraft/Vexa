/* fetch: downloads a web page over HTTP and prints it (or saves it with -o).
 *
 *   fetch http://10.0.2.2:8000/hello.txt
 *   fetch -o page.html http://example.com/
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/net.h>

static int usage(void) {
    fprintf(stderr, "usage: fetch [-o file] http://host[:port]/path\n");
    return 2;
}

static int write_all(int handle, const char *data, long size) {
    while (size > 0) {
        long n = vx_write(handle, data, (size_t)size);
        if (n <= 0) {
            return -1;
        }
        data += n;
        size -= n;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *output = NULL, *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (!url) {
            url = argv[i];
        } else {
            return usage();
        }
    }
    if (!url || strncmp(url, "http://", 7) != 0) {
        return usage();
    }
    /* http://host[:port][/path] */
    char host[256];
    const char *p = url + 7, *path = strchr(p, '/');
    size_t host_length = path ? (size_t)(path - p) : strlen(p);
    if (host_length == 0 || host_length >= sizeof(host)) {
        return usage();
    }
    memcpy(host, p, host_length);
    host[host_length] = '\0';
    uint16_t port = 80;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = (uint16_t)atoi(colon + 1);
    }
    if (!path) {
        path = "/";
    }

    int handle = vx_connect_to(host, port);
    if (handle < 0) {
        fprintf(stderr, "fetch: %s: %s\n", host, vx_strerror(handle));
        return 1;
    }
    char request[1024];
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Vexa-fetch\r\n"
                     "Connection: close\r\n\r\n",
                     path, host);
    if (n <= 0 || (size_t)n >= sizeof(request) || write_all(handle, request, n)) {
        fprintf(stderr, "fetch: can't send the request\n");
        return 1;
    }
    int out = 1;
    if (output) {
        out = vx_open(output, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
        if (out < 0) {
            fprintf(stderr, "fetch: %s: %s\n", output, vx_strerror(out));
            return 1;
        }
    }
    /* The status line and headers, then the body as it comes. */
    static char buffer[16384];
    long used = 0, total = 0;
    bool in_body = false;
    int status = 0;
    for (;;) {
        long got = vx_read(handle, buffer + used, sizeof(buffer) - (size_t)used);
        if (got < 0) {
            fprintf(stderr, "fetch: %s\n", vx_strerror(got));
            return 1;
        }
        if (got == 0) {
            break;
        }
        used += got;
        if (!in_body) {
            char *end = NULL;
            for (long i = 0; i + 3 < used; i++) {
                if (memcmp(buffer + i, "\r\n\r\n", 4) == 0) {
                    end = buffer + i + 4;
                    break;
                }
            }
            if (!end) {
                if (used == (long)sizeof(buffer)) {
                    fprintf(stderr, "fetch: headers too long\n");
                    return 1;
                }
                continue;
            }
            if (used < 12 || strncmp(buffer, "HTTP/1.", 7) != 0) {
                fprintf(stderr, "fetch: not an HTTP answer\n");
                return 1;
            }
            status = atoi(buffer + 9);
            in_body = true;
            long body = used - (end - buffer);
            memmove(buffer, end, (size_t)body);
            used = body;
        }
        if (write_all(out, buffer, used)) {
            fprintf(stderr, "fetch: can't write the output\n");
            return 1;
        }
        total += used;
        used = 0;
    }
    vx_close(handle);
    if (output) {
        vx_close(out);
        printf("fetch: saved %ld bytes to %s (HTTP %d)\n", total, output, status);
    }
    if (status < 200 || status > 299) {
        fprintf(stderr, "fetch: the server answered HTTP %d\n", status);
        return 1;
    }
    return 0;
}
