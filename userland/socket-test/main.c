/* socket-test: checks sockets without leaving the machine: local socket
 * pairs, TCP and UDP over the loopback interface (IPv4 and IPv6), refused
 * connections and non-blocking mode. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vexa/net.h>
#include <vexa/thread.h>

#define TCP_BYTES (300 * 1024)

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("socket-test: FAILED: %s\n", what);
        failures++;
    }
}

static unsigned char pattern(long i) {
    return (unsigned char)(i * 7 + i / 251);
}

static void test_pair(void) {
    int pair[2];
    check(vx_socket_pair(VX_AF_UNIX, VX_SOCK_STREAM, pair) == 0, "socket pair");
    check(vx_write(pair[0], "ping", 4) == 4, "write to a pair");
    char buffer[8] = {0};
    check(vx_read(pair[1], buffer, sizeof(buffer)) == 4 && memcmp(buffer, "ping", 4) == 0,
          "read from a pair");
    vx_close(pair[0]);
    check(vx_read(pair[1], buffer, sizeof(buffer)) == 0, "end of data once the other end closes");
    vx_close(pair[1]);
}

struct server {
    int listener;
    long received;
    int ok;
};

static void *serve(void *arg) {
    struct server *s = arg;
    struct vx_socket_address peer;
    int connection = vx_accept(s->listener, &peer, 0);
    if (connection < 0) {
        return NULL;
    }
    static unsigned char buffer[8192];
    s->ok = 1;
    for (;;) {
        long n = vx_read(connection, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        for (long i = 0; i < n; i++) {
            if (buffer[i] != pattern(s->received + i)) {
                s->ok = 0;
            }
        }
        s->received += n;
    }
    vx_write(connection, "thanks", 6);
    vx_close(connection);
    return NULL;
}

static void test_tcp(void) {
    struct server s = {0};
    s.listener = vx_socket(VX_AF_INET, VX_SOCK_STREAM, 0);
    struct vx_socket_address local = vx_inet_address(0x0100007f, 0); /* 127.0.0.1, any port */
    check(vx_bind(s.listener, &local, sizeof(local.inet)) == 0, "bind");
    check(vx_listen(s.listener, 4) == 0, "listen");
    struct vx_socket_address bound;
    vx_socket_address(s.listener, 0, &bound);
    check(bound.inet.port != 0, "a port was chosen");
    struct vx_thread *thread = vx_thread_create(serve, &s);

    int client = vx_socket(VX_AF_INET, VX_SOCK_STREAM, 0);
    long error = vx_connect(client, &bound, sizeof(bound.inet));
    check(error == 0, "connect over loopback");
    static unsigned char data[TCP_BYTES];
    for (long i = 0; i < TCP_BYTES; i++) {
        data[i] = pattern(i);
    }
    long sent = 0;
    while (sent < TCP_BYTES) {
        long n = vx_write(client, data + sent, TCP_BYTES - sent);
        if (n <= 0) {
            break;
        }
        sent += n;
    }
    check(sent == TCP_BYTES, "send everything");
    vx_shutdown(client, VX_SHUT_WRITE);
    char reply[16] = {0};
    long n = vx_read(client, reply, sizeof(reply));
    check(n == 6 && memcmp(reply, "thanks", 6) == 0, "the server's reply");
    vx_close(client);
    vx_thread_join(thread);
    check(s.received == TCP_BYTES && s.ok, "the server got every byte, in order");
    vx_close(s.listener);

    /* Nobody listens there any more. */
    client = vx_socket(VX_AF_INET, VX_SOCK_STREAM, 0);
    check(vx_connect(client, &bound, sizeof(bound.inet)) == -VX_ECONNREFUSED,
          "connection refused");
    vx_close(client);
}

static void test_udp(void) {
    int a = vx_socket(VX_AF_INET, VX_SOCK_DGRAM, 0);
    int b = vx_socket(VX_AF_INET, VX_SOCK_DGRAM, 0);
    struct vx_socket_address any = vx_inet_address(0, 0);
    check(vx_bind(b, &any, sizeof(any.inet)) == 0, "bind UDP");
    struct vx_socket_address where;
    vx_socket_address(b, 0, &where);
    where.inet.address = 0x0100007f;
    struct vx_message out = {"datagram", 8, 0, sizeof(where.inet), &where};
    check(vx_send(a, &out) == 8, "send a datagram");
    char buffer[32];
    struct vx_socket_address from;
    struct vx_message in = {buffer, sizeof(buffer), 0, 0, &from};
    check(vx_receive(b, &in) == 8 && memcmp(buffer, "datagram", 8) == 0, "receive it");
    check(from.inet.address == 0x0100007f && from.inet.port != 0, "the sender's address");
    vx_close(a);
    vx_close(b);
}

static void test_nonblocking(void) {
    int listener = vx_socket(VX_AF_INET, VX_SOCK_STREAM | VX_SOCK_NONBLOCK, 0);
    struct vx_socket_address local = vx_inet_address(0x0100007f, 0);
    vx_bind(listener, &local, sizeof(local.inet));
    vx_listen(listener, 1);
    check(vx_accept(listener, NULL, 0) == -VX_EAGAIN, "non-blocking accept");
    struct vx_poll poll = {listener, VX_POLL_READ, 0};
    check(vx_poll(&poll, 1, 50) == 0, "poll times out");
    vx_close(listener);
}

static const uint8_t loopback6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

static void test_ipv6_text(void) {
    uint8_t a[16];
    char text[46];
    check(vx_parse_ipv6("2001:db8::ff00:42:8329", a) == 0 &&
              strcmp(vx_format_ipv6(a, text), "2001:db8::ff00:42:8329") == 0,
          "IPv6 address text");
    check(vx_parse_ipv6("::1", a) == 0 && memcmp(a, loopback6, 16) == 0, "::1");
    check(vx_parse_ipv6("::ffff:10.0.2.2", a) == 0 && a[10] == 0xff && a[12] == 10 &&
              a[15] == 2 && strcmp(vx_format_ipv6(a, text), "::ffff:10.0.2.2") == 0,
          "an IPv4-mapped address");
    check(vx_parse_ipv6("1::2::3", a) != 0 && vx_parse_ipv6("12345::", a) != 0 &&
              vx_parse_ipv6("1:2:3:4:5:6:7", a) != 0,
          "bad IPv6 addresses are refused");
    check(vx_parse_ipv6("1:0:0:2:0:0:0:3", a) == 0 &&
              strcmp(vx_format_ipv6(a, text), "1:0:0:2::3") == 0,
          "the longest run of zeros becomes ::");
}

static void test_ipv6(void) {
    /* TCP over ::1. */
    struct server s = {0};
    s.listener = vx_socket(VX_AF_INET6, VX_SOCK_STREAM, 0);
    check(s.listener >= 0, "an IPv6 socket");
    struct vx_socket_address local = vx_inet6_address(loopback6, 0);
    check(vx_bind(s.listener, &local, sizeof(local.inet6)) == 0, "bind to ::1");
    check(vx_listen(s.listener, 4) == 0, "listen on ::1");
    struct vx_socket_address bound;
    vx_socket_address(s.listener, 0, &bound);
    check(bound.family == VX_AF_INET6 && bound.inet6.port != 0, "an IPv6 address and port");
    struct vx_thread *thread = vx_thread_create(serve, &s);
    int client = vx_socket(VX_AF_INET6, VX_SOCK_STREAM, 0);
    check(vx_connect(client, &bound, sizeof(bound.inet6)) == 0, "connect to ::1");
    static unsigned char data[20000];
    for (long i = 0; i < (long)sizeof(data); i++) {
        data[i] = pattern(i);
    }
    check(vx_write(client, data, sizeof(data)) == (long)sizeof(data), "send over IPv6");
    vx_shutdown(client, VX_SHUT_WRITE);
    char reply[16] = {0};
    check(vx_read(client, reply, sizeof(reply)) == 6, "the reply over IPv6");
    vx_close(client);
    vx_thread_join(thread);
    check(s.received == (long)sizeof(data) && s.ok, "every byte over IPv6");
    vx_close(s.listener);

    /* An IPv6 socket on :: takes IPv4 connections too, from ::ffff:a.b.c.d. */
    int listener = vx_socket(VX_AF_INET6, VX_SOCK_STREAM, 0);
    local = vx_inet6_address((const uint8_t[16]){0}, 0);
    check(vx_bind(listener, &local, sizeof(local.inet6)) == 0 && vx_listen(listener, 1) == 0,
          "listen on ::");
    vx_socket_address(listener, 0, &bound);
    client = vx_socket(VX_AF_INET, VX_SOCK_STREAM, 0);
    struct vx_socket_address to = vx_inet_address(0x0100007f, vx_net16(bound.inet6.port));
    check(vx_connect(client, &to, sizeof(to.inet)) == 0, "IPv4 to an IPv6 listener");
    struct vx_socket_address peer;
    int connection = vx_accept(listener, &peer, 0);
    char text[46];
    check(connection >= 0 && peer.family == VX_AF_INET6 &&
              strcmp(vx_format_ipv6(peer.inet6.address, text), "::ffff:127.0.0.1") == 0,
          "the IPv4 peer as ::ffff:127.0.0.1");
    vx_close(connection);
    vx_close(client);
    vx_close(listener);

    /* UDP over ::1. */
    int a = vx_socket(VX_AF_INET6, VX_SOCK_DGRAM, 0);
    int b = vx_socket(VX_AF_INET6, VX_SOCK_DGRAM, 0);
    local = vx_inet6_address(loopback6, 0);
    check(vx_bind(b, &local, sizeof(local.inet6)) == 0, "bind UDP to ::1");
    struct vx_socket_address where;
    vx_socket_address(b, 0, &where);
    struct vx_message out = {"datagram6", 9, 0, sizeof(where.inet6), &where};
    check(vx_send(a, &out) == 9, "send a datagram over IPv6");
    char buffer[32];
    struct vx_socket_address from;
    struct vx_message in = {buffer, sizeof(buffer), 0, 0, &from};
    check(vx_receive(b, &in) == 9 && memcmp(buffer, "datagram6", 9) == 0 &&
              from.family == VX_AF_INET6 && memcmp(from.inet6.address, loopback6, 16) == 0,
          "receive it, from ::1");
    vx_close(a);
    vx_close(b);

    /* An IPv4 socket takes no IPv6 address. */
    a = vx_socket(VX_AF_INET, VX_SOCK_DGRAM, 0);
    check(vx_bind(a, &local, sizeof(local.inet6)) == -VX_EAFNOSUPPORT,
          "IPv6 addresses on IPv4 sockets are refused");
    vx_close(a);
}

int main(void) {
    test_pair();
    test_tcp();
    test_udp();
    test_nonblocking();
    test_ipv6_text();
    test_ipv6();
    if (failures) {
        printf("socket-test: %d checks failed\n", failures);
        return 1;
    }
    printf("socket-test: passed\n");
    return 0;
}
