/* fetch: downloads a web page over HTTP or HTTPS and prints it (or saves it
 * with -o), following redirects.
 *
 *   fetch http://10.0.2.2:8000/hello.txt
 *   fetch -o page.html https://example.com/
 *   fetch --ca my-ca.pem https://10.0.2.2:8443/   (a server with its own CA)
 *   fetch -k https://...                           (no certificate check)
 *
 * HTTPS is TLS 1.2 or 1.3 (Mbed TLS), checking the server's certificate
 * against the standard root certificates in /etc/ssl/certs.
 */
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <vexa/net.h>

#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"
#define MAX_REDIRECTS 5

static int usage(void) {
    fprintf(stderr, "usage: fetch [-o file] [--ca file] [-k] http[s]://host[:port]/path\n");
    return 2;
}

/* ---- A connection: plain, or through TLS ---- */

struct connection {
    int handle;
    bool tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
};

static int tls_send(void *arg, const unsigned char *data, size_t size) {
    long n = vx_write(*(int *)arg, data, size);
    return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}

static int tls_receive(void *arg, unsigned char *data, size_t size) {
    long n = vx_read(*(int *)arg, data, size);
    return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}

static void tls_problem(const char *what, int error) {
    char text[160];
    mbedtls_strerror(error, text, sizeof(text));
    fprintf(stderr, "fetch: %s: %s\n", what, text);
}

/* TLS on the connected handle, for `host`. */
static bool start_tls(struct connection *c, const char *host, const char *ca_file, bool check) {
    c->tls = true;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->config);
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->random);
    int e;
    if ((e = mbedtls_ctr_drbg_seed(&c->random, mbedtls_entropy_func, &c->entropy,
                                   (const unsigned char *)"vexa-fetch", 10)) != 0) {
        tls_problem("no random numbers", e);
        return false;
    }
    if (check && (e = mbedtls_x509_crt_parse_file(&c->ca, ca_file ? ca_file : CA_BUNDLE)) < 0) {
        tls_problem(ca_file ? ca_file : CA_BUNDLE, e);
        return false;
    }
    if ((e = mbedtls_ssl_config_defaults(&c->config, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        tls_problem("TLS", e);
        return false;
    }
    mbedtls_ssl_conf_authmode(&c->config,
                              check ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_ca_chain(&c->config, &c->ca, NULL);
    mbedtls_ssl_conf_rng(&c->config, mbedtls_ctr_drbg_random, &c->random);
    if ((e = mbedtls_ssl_setup(&c->ssl, &c->config)) != 0 ||
        (e = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0) {
        tls_problem("TLS", e);
        return false;
    }
    mbedtls_ssl_set_bio(&c->ssl, &c->handle, tls_send, tls_receive, NULL);
    while ((e = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (e != MBEDTLS_ERR_SSL_WANT_READ && e != MBEDTLS_ERR_SSL_WANT_WRITE) {
            uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
            if (flags && flags != (uint32_t)-1) {
                char why[256];
                mbedtls_x509_crt_verify_info(why, sizeof(why), "  ", flags);
                fprintf(stderr, "fetch: %s's certificate isn't trusted:\n%s", host, why);
            } else {
                tls_problem(host, e);
            }
            return false;
        }
    }
    return true;
}

static long connection_write(struct connection *c, const char *data, size_t size) {
    if (!c->tls) {
        return vx_write(c->handle, data, size);
    }
    int n;
    while ((n = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data, size)) ==
           MBEDTLS_ERR_SSL_WANT_WRITE) {
    }
    return n < 0 ? -VX_EIO : n;
}

static long connection_read(struct connection *c, char *data, size_t size) {
    if (!c->tls) {
        return vx_read(c->handle, data, size);
    }
    for (;;) {
        int n = mbedtls_ssl_read(&c->ssl, (unsigned char *)data, size);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) {
            return 0;
        }
        return n < 0 ? -VX_EIO : n;
    }
}

static void connection_close(struct connection *c) {
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->config);
        mbedtls_x509_crt_free(&c->ca);
        mbedtls_ctr_drbg_free(&c->random);
        mbedtls_entropy_free(&c->entropy);
    }
    vx_close(c->handle);
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

/* ---- One request ---- */

struct url {
    bool https;
    char host[256]; /* An IPv6 address without its brackets. */
    bool ipv6;
    uint16_t port;
    char path[1024];
};

static bool parse_url(const char *text, struct url *u) {
    if (strncmp(text, "http://", 7) == 0) {
        u->https = false;
        text += 7;
    } else if (strncmp(text, "https://", 8) == 0) {
        u->https = true;
        text += 8;
    } else {
        return false;
    }
    const char *path = strchr(text, '/');
    size_t length = path ? (size_t)(path - text) : strlen(text);
    if (length == 0 || length >= sizeof(u->host)) {
        return false;
    }
    memcpy(u->host, text, length);
    u->host[length] = '\0';
    u->port = u->https ? 443 : 80;
    char *colon = strchr(u->host, ':');
    u->ipv6 = u->host[0] == '[';
    if (u->ipv6) {
        /* [2001:db8::1]:8080 */
        char *end = strchr(u->host, ']');
        if (!end || (end[1] && end[1] != ':')) {
            return false;
        }
        if (end[1]) {
            u->port = (uint16_t)atoi(end + 2);
        }
        *end = '\0';
        memmove(u->host, u->host + 1, strlen(u->host + 1) + 1);
        colon = NULL;
    }
    if (colon) {
        *colon = '\0';
        u->port = (uint16_t)atoi(colon + 1);
    }
    snprintf(u->path, sizeof(u->path), "%s", path ? path : "/");
    return true;
}

/* Fetches `u` into `out`. Returns the HTTP status (a redirect's target in
 * `location`), or -1. */
static int request(const struct url *u, int out, const char *ca_file, bool check, char *location,
                   size_t location_size, long *total) {
    struct connection c = {0};
    c.handle = vx_connect_to(u->host, u->port);
    if (c.handle < 0) {
        fprintf(stderr, "fetch: %s: %s\n", u->host, vx_strerror(c.handle));
        return -1;
    }
    if (u->https && !start_tls(&c, u->host, ca_file, check)) {
        connection_close(&c);
        return -1;
    }
    char text[1400];
    int n = snprintf(text, sizeof(text),
                     "GET %s HTTP/1.0\r\nHost: %s%s%s\r\nUser-Agent: Vexa-fetch\r\n"
                     "Connection: close\r\n\r\n",
                     u->path, u->ipv6 ? "[" : "", u->host, u->ipv6 ? "]" : "");
    if (n <= 0 || (size_t)n >= sizeof(text) || connection_write(&c, text, (size_t)n) != n) {
        fprintf(stderr, "fetch: can't send the request\n");
        connection_close(&c);
        return -1;
    }
    /* The status line and headers, then the body as it comes. */
    static char buffer[16384];
    long used = 0;
    bool in_body = false;
    int status = -1;
    *total = 0;
    location[0] = '\0';
    for (;;) {
        long got = connection_read(&c, buffer + used, sizeof(buffer) - (size_t)used);
        if (got < 0) {
            fprintf(stderr, "fetch: %s\n", vx_strerror(got));
            connection_close(&c);
            return -1;
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
                    connection_close(&c);
                    return -1;
                }
                continue;
            }
            if (used < 12 || strncmp(buffer, "HTTP/1.", 7) != 0) {
                fprintf(stderr, "fetch: not an HTTP answer\n");
                connection_close(&c);
                return -1;
            }
            status = atoi(buffer + 9);
            for (char *line = strstr(buffer, "\r\n"); line && line + 2 < end;
                 line = strstr(line + 2, "\r\n")) {
                if (strncasecmp(line + 2, "Location:", 9) == 0) {
                    char *v = line + 11;
                    while (*v == ' ') {
                        v++;
                    }
                    size_t length = strcspn(v, "\r\n");
                    if (length < location_size) {
                        memcpy(location, v, length);
                        location[length] = '\0';
                    }
                }
            }
            in_body = true;
            long body = used - (end - buffer);
            memmove(buffer, end, (size_t)body);
            used = body;
            if (status >= 300 && status < 400 && location[0]) {
                break; /* (A redirect: its body doesn't matter.) */
            }
        }
        if (write_all(out, buffer, used)) {
            fprintf(stderr, "fetch: can't write the output\n");
            connection_close(&c);
            return -1;
        }
        *total += used;
        used = 0;
    }
    connection_close(&c);
    return status;
}

int main(int argc, char **argv) {
    const char *output = NULL, *text = NULL, *ca_file = NULL;
    bool check = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--ca") == 0 && i + 1 < argc) {
            ca_file = argv[++i];
        } else if (strcmp(argv[i], "-k") == 0) {
            check = false;
        } else if (!text) {
            text = argv[i];
        } else {
            return usage();
        }
    }
    struct url u;
    if (!text || !parse_url(text, &u)) {
        return usage();
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        fprintf(stderr, "fetch: the cryptography didn't start\n");
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
    int status;
    long total = 0;
    for (int redirects = 0;; redirects++) {
        char location[1024];
        status = request(&u, out, ca_file, check, location, sizeof(location), &total);
        if (status < 300 || status >= 400 || !location[0] || redirects == MAX_REDIRECTS) {
            break;
        }
        /* Somewhere else: a whole address, or a path on the same server. */
        if (location[0] == '/') {
            snprintf(u.path, sizeof(u.path), "%s", location);
        } else if (!parse_url(location, &u)) {
            fprintf(stderr, "fetch: can't follow a redirect to %s\n", location);
            return 1;
        }
        if (output) {
            vx_close(out);
            out = vx_open(output, VX_OPEN_WRITE | VX_OPEN_CREATE | VX_OPEN_TRUNCATE);
        }
    }
    if (status < 0) {
        return 1;
    }
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
